#include "MainFrame.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include <nlohmann/json.hpp>
#include <wx/app.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/config.h>
#include <wx/iconbndl.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/srchctrl.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valtext.h>

#include <wx/msw/wrapwin.h>
#include <commctrl.h>

#include "Logo.h"
#include "Theme.h"
#include "VideoId.h"
#include "YouTubePlayer.h"

namespace {

// UTF-8 glyphs, spelled as byte escapes so the source stays pure ASCII
// (an editor or tool re-encoding the file can't corrupt them).
const char* const kGlyphPlay = "\xE2\x96\xB6";                // U+25B6
const char* const kGlyphPause = "\xE2\x9D\x9A\xE2\x9D\x9A";   // U+275A x2
// Not U+23ED (next track): Windows renders it as a colour emoji.
const char* const kGlyphSkip = "\xE2\x96\xB6\xE2\x96\xB6";        // U+25B6 x2
const char* const kGlyphDot = "\xC2\xB7";                     // U+00B7
const char* const kGlyphDash = "\xE2\x80\x94";                // U+2014
const char* const kGlyphEllipsis = "\xE2\x80\xA6";            // U+2026

wxString U8(const std::string& s) { return wxString::FromUTF8(s); }

// How often the player is asked for its position (drift check).
constexpr int kTickMs = 500;
// Spacing between timeSync requests within a round, so they don't queue
// behind each other and inflate each other's RTT.
constexpr int kSyncSpacingMs = 100;
// After commanding the player, give it this long to act before judging it
// again; otherwise a seek that is still buffering looks like drift and
// triggers another seek.
constexpr double kCooldownMs = 1500;
// If the player is parked at most this far *ahead* of the timeline (a
// resume scheduled LEAD ms in the future), wait for the timeline to reach it
// instead of seeking backwards.
constexpr double kMaxWaitMs = 1000;
// Learned latencies are nudged by this fraction of each measured error.
constexpr double kLatencyGain = 0.5;
constexpr double kMaxLatencyMs = 1000;
// A measured error bigger than this isn't latency (it's a server-side jump
// or a stall) and must not be learned from.
constexpr double kMaxLearnErrorMs = 1000;
// How long a FlashFooter() message stays up.
constexpr double kFooterFlashMs = 4000;

// Identifies the server's timeline: changes on seek, skip, pause, resume.
std::string TimelineKey(const SessionState& s) {
    return std::to_string(s.currentIndex) + "@" + std::to_string(s.startedAt) + (s.pausedAt ? "p" : "");
}

bool IsRunning(const std::string& playerState) {
    return playerState == "playing" || playerState == "buffering";
}

std::string FormatTime(double sec) {
    const int s = static_cast<int>(std::max(0.0, sec) + 0.5);
    return wxString::Format("%d:%02d", s / 60, s % 60).ToStdString();
}

bool IsRoomCode(const wxString& s) {
    return s.length() == 4 && s.find_first_not_of("0123456789") == wxString::npos;
}

wxStaticText* Label(wxWindow* parent, const wxString& text, const wxColour& colour = theme::kText) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetForegroundColour(colour);
    return label;
}

// A thin horizontal rule.
wxWindow* Divider(wxWindow* parent) {
    auto* line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(theme::kDivider);
    line->SetMinSize(wxSize(-1, 1));
    return line;
}

} // namespace

MainFrame::MainFrame(const LaunchOptions& options)
    : wxFrame(nullptr, wxID_ANY, "Planar", wxDefaultPosition, wxDefaultSize),
      m_syncTimer(this), m_tickTimer(this), m_startTimer(this) {
    m_relayUrl = options.relayUrl;
    m_clientId = options.clientId;
    m_persist = options.persist;
    if (m_persist) {
        auto* config = wxConfig::Get();
        for (int k = kStart; k <= kSeek; ++k)
            config->Read(wxString::Format("latency/%d", k), &m_latencyMs[k], m_latencyMs[k]);
        // Don't let a bad saved value (e.g. from an older version) stick.
        for (double& latency : m_latencyMs)
            latency = std::clamp(latency, 0.0, kMaxLatencyMs);
    }
    if (!options.logPath.empty())
        m_log.open(options.logPath, std::ios::out | std::ios::trunc);
    Log("localMs,serverMs,event,state,desiredSec,reportedSec,driftMs,rttMs,version,videoId");

    // "appicon" is the icon resource in app.rc.
    SetIcons(wxIconBundle("appicon", ::GetModuleHandle(nullptr)));
    SetBackgroundColour(theme::kBackground);
    SetClientSize(FromDIP(wxSize(1180, 700)));
    SetMinClientSize(FromDIP(wxSize(900, 560)));

    m_relay = std::make_unique<RelayClient>(m_relayUrl, [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); });

    m_pages = new wxSimplebook(this);
    m_pages->AddPage(BuildStartPage(m_pages), "start");
    m_pages->AddPage(BuildRoomPage(m_pages), "room");
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_pages, 1, wxEXPAND);
    SetSizer(root);

    m_nameText->SetValue(U8(options.name));
    m_volume->SetValue(options.volume);

    WireSession();
    BindRoomControls();

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { OnSyncTimer(); }, m_syncTimer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { OnTick(); }, m_tickTimer.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Reconcile(nullptr, true); }, m_startTimer.GetId());
    m_tickTimer.Start(kTickMs);

    // Closing the window is leaving the room (immediately, not after the
    // grace period meant for dropped connections).
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        if (m_connection->GetStatus() != SessionConnection::Status::Disconnected)
            m_connection->Leave();
        e.Skip();
    });

    m_pages->SetSelection(0);
    UpdateControls();
    if (!options.roomCode.empty())
        CallAfter([this, code = options.roomCode] {
            m_codeText->SetValue(U8(code));
            OnJoinRoom();
        });
    else if (options.createRoom)
        CallAfter([this] { OnCreateRoom(); });
}

// ---- UI construction --------------------------------------------------------

wxPanel* MainFrame::BuildStartPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    page->SetBackgroundColour(theme::kBackground);
    const int gap = FromDIP(8);
    const int width = FromDIP(280);

    auto* col = new wxBoxSizer(wxVERTICAL);
    col->Add(new LogoCanvas(page, 72), 0, wxALIGN_CENTER_HORIZONTAL);

    auto* title = Label(page, "Planar");
    title->SetFont(title->GetFont().Scaled(2.2f).Bold());
    col->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, gap * 2);
    col->Add(Label(page, "Listen together, in sync.", theme::kMuted), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, gap / 2);
    col->AddSpacer(gap * 4);

    m_nameText = new wxTextCtrl(page, wxID_ANY);
    m_nameText->SetHint("Your name");
    m_nameText->SetMaxLength(32);
    col->Add(m_nameText, 0, wxEXPAND);

    m_createBtn = new wxButton(page, wxID_ANY, "Create a room");
    m_createBtn->SetMinSize(wxSize(-1, FromDIP(34)));
    col->Add(m_createBtn, 0, wxEXPAND | wxTOP, gap);

    auto* orRow = new wxBoxSizer(wxHORIZONTAL);
    orRow->Add(Divider(page), 1, wxALIGN_CENTER_VERTICAL);
    orRow->Add(Label(page, "or join with a code", theme::kMuted), 0, wxLEFT | wxRIGHT, gap);
    orRow->Add(Divider(page), 1, wxALIGN_CENTER_VERTICAL);
    col->Add(orRow, 0, wxEXPAND | wxTOP | wxBOTTOM, gap * 2);

    auto* joinRow = new wxBoxSizer(wxHORIZONTAL);
    m_codeText = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxTE_CENTRE | wxTE_PROCESS_ENTER, wxTextValidator(wxFILTER_DIGITS));
    m_codeText->SetHint("0000");
    m_codeText->SetMaxLength(4);
    m_codeText->SetFont(m_codeText->GetFont().Scaled(1.4f));
    m_joinBtn = new wxButton(page, wxID_ANY, "Join");
    joinRow->Add(m_codeText, 1, wxEXPAND | wxRIGHT, gap);
    joinRow->Add(m_joinBtn, 0, wxEXPAND);
    col->Add(joinRow, 0, wxEXPAND);

    m_startStatus = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxSize(width, -1),
                                     wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE);
    col->Add(m_startStatus, 0, wxEXPAND | wxTOP, gap * 2);
    col->SetMinSize(wxSize(width, -1));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->AddStretchSpacer(2);
    outer->Add(col, 0, wxALIGN_CENTER_HORIZONTAL);
    outer->AddStretchSpacer(3);
    page->SetSizer(outer);

    m_createBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnCreateRoom(); });
    m_joinBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnJoinRoom(); });
    m_codeText->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnJoinRoom(); });
    m_codeText->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateControls(); });
    return page;
}

wxPanel* MainFrame::BuildRoomPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    page->SetBackgroundColour(theme::kBackground);
    const int gap = FromDIP(8);
    auto* root = new wxBoxSizer(wxVERTICAL);

    // Header: logo + name | room code, listeners, Leave.
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(new LogoCanvas(page, 22), 0, wxALIGN_CENTER_VERTICAL);
    auto* brand = Label(page, "Planar");
    brand->SetFont(brand->GetFont().Scaled(1.15f).Bold());
    header->Add(brand, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap);
    header->AddStretchSpacer();
    header->Add(Label(page, "Room", theme::kMuted), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    m_roomCodeLabel = Label(page, "----", theme::kAccent);
    m_roomCodeLabel->SetFont(m_roomCodeLabel->GetFont().Scaled(1.3f).Bold());
    header->Add(m_roomCodeLabel, 0, wxALIGN_CENTER_VERTICAL);
    m_copyBtn = new wxButton(page, wxID_ANY, "Copy", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_copyBtn->SetToolTip("Copy the room code to share it");
    header->Add(m_copyBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap);
    m_listenersLabel = Label(page, "", theme::kMuted);
    header->Add(m_listenersLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap * 3);
    m_leaveBtn = new wxButton(page, wxID_ANY, "Leave");
    header->Add(m_leaveBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap * 3);
    root->Add(header, 0, wxEXPAND | wxALL, gap * 1.5);
    root->Add(Divider(page), 0, wxEXPAND);

    // Body: player | search + queue.
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    // YouTube's API policy requires the player to be visible while playing,
    // so it gets the largest share of the window.
    m_player = new YouTubePlayer(page);
    m_player->SetMinSize(FromDIP(wxSize(480, 270)));
    m_player->SetEventHandler([this](const nlohmann::json& e) { OnPlayerEvent(e); });
    body->Add(m_player, 1, wxEXPAND);

    // Fixed-width side column: its lists must not grow with long titles
    // (they ellipsize instead), or they would squeeze the player.
    auto* sidePanel = new wxPanel(page);
    sidePanel->SetBackgroundColour(theme::kBackground);
    const wxSize sideWidth(FromDIP(340), -1);
    sidePanel->SetMinSize(sideWidth);
    sidePanel->SetMaxSize(sideWidth);
    auto* side = new wxBoxSizer(wxVERTICAL);
    m_searchCtrl = new wxSearchCtrl(sidePanel, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_searchCtrl->SetDescriptiveText("Search, or paste a YouTube link");
    m_searchCtrl->ShowCancelButton(true);
    side->Add(m_searchCtrl, 0, wxEXPAND);

    auto* listHeader = new wxBoxSizer(wxHORIZONTAL);
    m_listTitle = Label(sidePanel, "Up next");
    m_listTitle->SetFont(m_listTitle->GetFont().Bold());
    listHeader->Add(m_listTitle, 1, wxALIGN_CENTER_VERTICAL);
    m_backBtn = new wxButton(sidePanel, wxID_ANY, "Back to queue", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    listHeader->Add(m_backBtn, 0, wxALIGN_CENTER_VERTICAL);
    side->Add(listHeader, 0, wxEXPAND | wxTOP | wxBOTTOM, gap * 1.5);

    m_listBook = new wxSimplebook(sidePanel);
    m_queueList = new wxListBox(m_listBook, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxBORDER_NONE);
    m_queueList->SetToolTip("Drag to reorder, right-click for more (host)");
    m_searchList = new wxListCtrl(m_listBook, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_NO_HEADER | wxBORDER_NONE);
    m_searchList->AppendColumn("Song");
    m_searchList->AppendColumn("Length", wxLIST_FORMAT_RIGHT);
    m_listBook->AddPage(m_queueList, "queue");
    m_listBook->AddPage(m_searchList, "results");
    side->Add(m_listBook, 1, wxEXPAND);
    sidePanel->SetSizer(side);
    body->Add(sidePanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap * 1.5);
    root->Add(body, 1, wxEXPAND);

    // Seek bar. Positions in tenths of a second (wxSlider is integer-only).
    auto* seekRow = new wxBoxSizer(wxHORIZONTAL);
    m_posLabel = new wxStaticText(page, wxID_ANY, "0:00", wxDefaultPosition, FromDIP(wxSize(44, -1)), wxALIGN_RIGHT);
    m_posLabel->SetForegroundColour(theme::kMuted);
    m_seekBar = new wxSlider(page, wxID_ANY, 0, 0, 1);
    m_durLabel = Label(page, "0:00", theme::kMuted);
    m_durLabel->SetMinSize(FromDIP(wxSize(44, -1)));
    seekRow->Add(m_posLabel, 0, wxALIGN_CENTER_VERTICAL);
    seekRow->Add(m_seekBar, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, gap);
    seekRow->Add(m_durLabel, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(seekRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);

    // Controls: play/pause, skip | footer status | volume.
    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    const wxSize transportSize = FromDIP(wxSize(44, 32));
    m_playPauseBtn = new wxButton(page, wxID_ANY, U8(kGlyphPlay), wxDefaultPosition, transportSize);
    m_skipBtn = new wxButton(page, wxID_ANY, U8(kGlyphSkip), wxDefaultPosition, transportSize);
    m_skipBtn->SetToolTip("Skip (host)");
    controls->Add(m_playPauseBtn, 0, wxALIGN_CENTER_VERTICAL);
    controls->Add(m_skipBtn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, gap / 2);
    m_footer = new wxStaticText(page, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(100), -1),
                                wxALIGN_CENTRE_HORIZONTAL | wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END);
    m_footer->SetForegroundColour(theme::kMuted);
    controls->Add(m_footer, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, gap * 2);
    m_volume = new wxSlider(page, wxID_ANY, 100, 0, 100, wxDefaultPosition, FromDIP(wxSize(120, -1)));
    m_volume->SetToolTip("Volume (only on this computer)");
    controls->Add(m_volume, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(controls, 0, wxEXPAND | wxALL, gap * 1.5);

    page->SetSizer(root);
    return page;
}

void MainFrame::BindRoomControls() {
    m_leaveBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnLeave(); });
    m_copyBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(U8(m_roomCode)));
            wxTheClipboard->Close();
            FlashFooter("Room code copied");
        }
    });
    m_playPauseBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnPlayPause(); });
    m_skipBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Send(intent::Skip()); });
    m_volume->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
        m_player->SetVolume(m_volume->GetValue());
        if (m_persist)
            wxConfig::Get()->Write("volume", m_volume->GetValue());
    });

    // Search box: Enter searches, or adds directly if it's a YouTube link.
    m_searchCtrl->Bind(wxEVT_SEARCHCTRL_SEARCH_BTN, [this](wxCommandEvent&) { OnSearchEnter(); });
    m_searchCtrl->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnSearchEnter(); });
    m_searchCtrl->Bind(wxEVT_SEARCHCTRL_CANCEL_BTN, [this](wxCommandEvent&) {
        m_searchCtrl->Clear();
        ShowQueueList();
    });
    m_backBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowQueueList(); });
    m_searchList->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { OnAddSearchResult(); });
    m_searchList->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        const int lengthCol = FromDIP(52);
        m_searchList->SetColumnWidth(1, lengthCol);
        m_searchList->SetColumnWidth(0, std::max(FromDIP(80), m_searchList->GetClientSize().x - lengthCol - 1));
    });

    // Queue: drag to reorder, right-click menu, Delete key.
    BindQueueDrag();
    m_queueList->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) { ShowQueueMenu(); });
    m_queueList->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent& e) {
        // Select what was right-clicked, as Explorer does.
        const int hit = m_queueList->HitTest(e.GetPosition());
        if (hit != wxNOT_FOUND)
            m_queueList->SetSelection(hit);
        e.Skip();
    });
    m_queueList->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_DELETE)
            RemoveSelected();
        else
            e.Skip();
    });
    m_queueList->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { UpdateControls(); });

    // Seek bar: dragging previews the time; the seek happens on release
    // (wxEVT_SCROLL_CHANGED also covers keyboard changes).
    m_seekBar->Bind(wxEVT_SCROLL_THUMBTRACK, [this](wxScrollEvent&) {
        m_seekDragging = true;
        m_posLabel->SetLabel(FormatTime(m_seekBar->GetValue() / 10.0));
    });
    m_seekBar->Bind(wxEVT_SCROLL_CHANGED, [this](wxScrollEvent&) { OnSeekBarChanged(); });
    // A click on the track jumps there. (Windows' trackbar would only move one
    // page towards the click, which nobody expects from a seek bar.) Clicks on
    // the thumb fall through to the normal drag.
    m_seekBar->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        const auto hwnd = static_cast<HWND>(m_seekBar->GetHWND());
        RECT channel{}, thumb{};
        ::SendMessage(hwnd, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        ::SendMessage(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const POINT p{e.GetX(), e.GetY()};
        if (!m_seekBar->IsEnabled() || ::PtInRect(&thumb, p) || channel.right <= channel.left) {
            e.Skip();
            return;
        }
        // The thumb's centre travels between these two x positions.
        const int half = (thumb.right - thumb.left) / 2;
        const double left = channel.left + half, right = channel.right - half;
        const double frac = std::clamp((p.x - left) / std::max(1.0, right - left), 0.0, 1.0);
        m_seekBar->SetValue(static_cast<int>(frac * m_seekBar->GetMax() + 0.5));
        OnSeekBarChanged();
        // Not skipped: the native control must not also do its page step.
    });
}

// Drag-to-reorder: press on an item, release on another. wxListBox has no
// built-in drag and drop, but the native list box already tracks the mouse
// (the selection follows it while the button is down), so press/release
// positions are all we need.
void MainFrame::BindQueueDrag() {
    m_queueList->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        m_dragFrom = m_queueList->HitTest(e.GetPosition());
        e.Skip(); // keep normal selection behaviour
    });
    m_queueList->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (m_dragFrom != wxNOT_FOUND && e.LeftIsDown() && IsHost()) {
            const int over = m_queueList->HitTest(e.GetPosition());
            m_queueList->SetCursor(over != wxNOT_FOUND && over != m_dragFrom ? wxCursor(wxCURSOR_SIZENS) : wxNullCursor);
        }
        e.Skip();
    });
    m_queueList->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        e.Skip();
        m_queueList->SetCursor(wxNullCursor);
        const int from = m_dragFrom;
        m_dragFrom = wxNOT_FOUND;
        if (from == wxNOT_FOUND || !IsHost())
            return;
        int to = m_queueList->HitTest(e.GetPosition());
        const int count = static_cast<int>(m_state.queue.size());
        if (to == wxNOT_FOUND)
            to = e.GetPosition().y < 0 ? 0 : count - 1; // dropped above/below the items
        if (to == from || from >= count || to < 0 || to >= count)
            return;
        Send(intent::Move(from, to));
        // The list is rebuilt when the new state arrives; keep the moved item
        // selected.
        CallAfter([this, to] {
            if (to < static_cast<int>(m_queueList->GetCount()))
                m_queueList->SetSelection(to);
            UpdateControls();
        });
    });
}

// ---- Start page -------------------------------------------------------------

std::string MainFrame::CurrentName() const {
    const auto name = m_nameText->GetValue().Trim().Trim(false).utf8_string();
    return name.empty() ? "Guest" : name;
}

void MainFrame::SetStartBusy(bool busy) {
    m_createBtn->Enable(!busy);
    m_joinBtn->Enable(!busy && IsRoomCode(m_codeText->GetValue()));
    m_nameText->Enable(!busy);
    m_codeText->Enable(!busy);
}

void MainFrame::OnCreateRoom() {
    SetStartBusy(true);
    m_startStatus->SetForegroundColour(theme::kMuted);
    m_startStatus->SetLabel(U8(std::string("Creating a room") + kGlyphEllipsis));
    m_relay->CreateRoom([this](const std::string& code, const std::string& error) {
        SetStartBusy(false);
        if (!error.empty()) {
            ShowStartPage(U8(error), true);
            return;
        }
        EnterRoom(code);
    });
}

void MainFrame::OnJoinRoom() {
    const wxString code = m_codeText->GetValue().Trim().Trim(false);
    if (!IsRoomCode(code)) {
        ShowStartPage("Room codes are 4 digits.", true);
        return;
    }
    EnterRoom(code.utf8_string());
}

void MainFrame::EnterRoom(const std::string& code) {
    if (m_persist) {
        wxConfig::Get()->Write("name", m_nameText->GetValue());
        wxConfig::Get()->Flush();
    }
    m_roomCode = code;
    m_roomCodeLabel->SetLabel(U8(code));
    m_startStatus->SetLabel("");
    m_state = SessionState{};
    m_haveState = false;
    m_myMemberId.clear();
    m_clock.Reset();
    ShowQueueList();
    RefreshQueue();
    UpdateHeader();
    m_pages->SetSelection(1);
    m_pages->GetPage(1)->Layout();
    m_searchCtrl->SetFocus();
    m_connection->Connect(m_relayUrl, code, CurrentName(), m_clientId);
}

void MainFrame::ShowStartPage(const wxString& message, bool isError) {
    if (m_connection->GetStatus() != SessionConnection::Status::Disconnected)
        m_connection->Disconnect();
    m_startStatus->SetForegroundColour(isError ? theme::kError : theme::kMuted);
    m_startStatus->SetLabel(message);
    m_pages->SetSelection(0);
    SetStartBusy(false);
    UpdateControls();
}

// ---- Room page actions ------------------------------------------------------

bool MainFrame::Send(const nlohmann::json& intent) {
    if (m_connection->SendIntent(intent))
        return true;
    FlashFooter(U8(std::string("Not connected right now ") + kGlyphDash + " try again in a moment."), true);
    return false;
}

void MainFrame::OnLeave() {
    m_connection->Leave(); // removes us at once (no grace period)
    ShowStartPage();
}

void MainFrame::OnSearchEnter() {
    const auto text = m_searchCtrl->GetValue().Trim().Trim(false).utf8_string();
    if (text.empty()) {
        ShowQueueList();
        return;
    }
    // A pasted link (or bare video ID) is added directly.
    if (text.find("youtu") != std::string::npos || text.size() == 11) {
        if (const auto id = ParseVideoId(text)) {
            if (Send(intent::Add(*id))) {
                m_searchCtrl->Clear();
                ShowQueueList();
                FlashFooter("Added to the queue");
            }
            return;
        }
    }

    m_listTitle->SetLabel(U8(std::string("Searching") + kGlyphEllipsis));
    ShowSearchResults();
    m_searchList->DeleteAllItems();
    m_relay->Search(text, [this, text](const std::vector<SearchResult>& results, const std::string& error) {
        m_searchResults = results;
        m_searchList->DeleteAllItems();
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            const long row = m_searchList->InsertItem(static_cast<long>(i),
                                                      U8(r.title + "  " + kGlyphDot + "  " + r.channel));
            m_searchList->SetItem(row, 1, r.durationSec ? U8(FormatTime(*r.durationSec)) : wxString());
        }
        if (!error.empty()) {
            m_listTitle->SetLabel("Search failed");
            FlashFooter(U8(error), true);
        } else if (results.empty()) {
            m_listTitle->SetLabel("No results");
        } else {
            m_listTitle->SetLabel("Double-click to add");
        }
        m_listTitle->GetParent()->Layout();
    });
}

void MainFrame::ShowQueueList() {
    m_listBook->SetSelection(0);
    m_listTitle->SetLabel("Up next");
    m_backBtn->Hide();
    m_listTitle->GetParent()->Layout();
}

void MainFrame::ShowSearchResults() {
    m_listBook->SetSelection(1);
    m_backBtn->Show();
    m_listTitle->GetParent()->Layout();
}

void MainFrame::OnAddSearchResult() {
    const long row = m_searchList->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || row >= static_cast<long>(m_searchResults.size()))
        return;
    const auto& r = m_searchResults[row];
    if (Send(intent::Add(r.videoId, r.title, r.durationSec.value_or(0))))
        FlashFooter(U8("Added: " + r.title));
}

bool MainFrame::CanRemove(int index) const {
    // Mirrors the server's permissionError(): the server enforces it, this
    // just avoids offering actions that would be refused.
    if (index < 0 || index >= static_cast<int>(m_state.queue.size()))
        return false;
    return IsHost() || m_state.queue[index].addedBy == m_myMemberId;
}

void MainFrame::ShowQueueMenu() {
    const int sel = m_queueList->GetSelection();
    if (sel == wxNOT_FOUND || sel >= static_cast<int>(m_state.queue.size()))
        return;
    const int count = static_cast<int>(m_state.queue.size());
    std::string addedBy = "someone who left";
    for (const auto& m : m_state.members)
        if (m.id == m_state.queue[sel].addedBy)
            addedBy = m.id == m_myMemberId ? "you" : m.name;

    wxMenu menu;
    menu.Append(1, "Move up")->Enable(IsHost() && sel > 0);
    menu.Append(2, "Move down")->Enable(IsHost() && sel + 1 < count);
    menu.AppendSeparator();
    menu.Append(3, "Remove")->Enable(CanRemove(sel));
    menu.AppendSeparator();
    menu.Append(4, U8("Added by " + addedBy))->Enable(false);
    switch (m_queueList->GetPopupMenuSelectionFromUser(menu)) {
    case 1: MoveSelected(-1); break;
    case 2: MoveSelected(+1); break;
    case 3: RemoveSelected(); break;
    default: break;
    }
}

void MainFrame::MoveSelected(int delta) {
    const int sel = m_queueList->GetSelection();
    const int to = sel + delta;
    if (!IsHost() || sel == wxNOT_FOUND || to < 0 || to >= static_cast<int>(m_state.queue.size()))
        return;
    if (Send(intent::Move(sel, to)))
        m_queueList->SetSelection(to); // follow the item
}

void MainFrame::RemoveSelected() {
    const int sel = m_queueList->GetSelection();
    if (CanRemove(sel))
        Send(intent::Remove(sel));
}

void MainFrame::OnPlayPause() {
    if (m_localHold) {
        // Rejoin the group where it is now.
        m_localHold = false;
        Log(",,releaseHold,,,,,,,");
        UpdateSyncStatus();
        UpdatePlayPause();
        Reconcile(nullptr, true);
        return;
    }
    const bool sessionPlaying = m_state.CurrentEntry() && !m_state.IsPaused();
    Send(sessionPlaying ? intent::Pause() : intent::Play());
}

void MainFrame::OnSeekBarChanged() {
    m_seekDragging = false;
    const double target = m_seekBar->GetValue() / 10.0;
    m_posLabel->SetLabel(FormatTime(target));
    // Asks the server; everyone (including us) seeks when the new state
    // arrives.
    Send(intent::Seek(target));
}

void MainFrame::UpdateSeekBar(double positionSec, double durationSec) {
    if (durationSec != m_durationSec) {
        m_durationSec = durationSec;
        m_seekBar->SetRange(0, std::max(1, static_cast<int>(durationSec * 10)));
        m_durLabel->SetLabel(FormatTime(durationSec));
        UpdateControls();
    }
    if (m_seekDragging)
        return; // don't yank the thumb out from under the user
    m_seekBar->SetValue(std::clamp(static_cast<int>(positionSec * 10), 0, m_seekBar->GetMax()));
    m_posLabel->SetLabel(FormatTime(positionSec));
}

// ---- Session ----------------------------------------------------------------

void MainFrame::WireSession() {
    // IXWebSocket callbacks arrive on a worker thread; CallAfter queues them
    // onto the GUI thread's event loop.
    m_connection = std::make_unique<SessionConnection>(
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); });

    m_connection->onStatus = [this](SessionConnection::Status s) { OnSessionStatus(s); };
    m_connection->onState = [this](const SessionState& s) { OnSessionState(s); };
    m_connection->onTimeSync = [this](const nlohmann::json& msg) {
        const bool hadEstimate = m_clock.HasEstimate();
        m_clock.OnResponse(msg);
        UpdateSyncStatus();
        // The first estimate turns "hold at 0" into real playback.
        if (!hadEstimate && m_clock.HasEstimate())
            Reconcile(nullptr, true);
    };
    m_connection->onWelcome = [this](const std::string& memberId) {
        m_myMemberId = memberId;
        UpdateControls();
        UpdateHeader();
    };
    m_connection->onRoomNotFound = [this] {
        ShowStartPage(U8("No room with code " + m_roomCode + "."), true);
    };
    m_connection->onError = [this](const std::string& message) {
        Log(",,error,,,,,,," + message);
        if (message.find("somewhere else") != std::string::npos)
            ShowStartPage("You joined this room from another window.");
        else
            FlashFooter(U8(message), true);
    };
}

bool MainFrame::InSession() const {
    // Keep following the last known state while reconnecting, so a network
    // blip doesn't stop the music (the clock offset is still valid).
    return m_haveState && m_connection->GetStatus() != SessionConnection::Status::Disconnected;
}

void MainFrame::OnSessionStatus(SessionConnection::Status status) {
    using S = SessionConnection::Status;
    if (status == S::Connected) {
        StartSyncRound();
    } else if (status == S::Disconnected) {
        // Left the room: stop following it.
        m_haveState = false;
        m_state = SessionState{};
        m_startTimer.Stop();
        m_localHold = false;
        m_playerVideoId.clear();
        m_player->ShowIdle();
        UpdateSeekBar(0, 0);
        RefreshQueue();
    }
    UpdateHeader();
    UpdateSyncStatus();
    UpdatePlayPause();
    UpdateControls();
}

void MainFrame::OnSessionState(const SessionState& state) {
    m_state = state;
    m_haveState = true;
    RefreshQueue();
    UpdateHeader();
    UpdatePlayPause();
    UpdateControls(); // the host may have changed

    const auto d = m_playback.Compute(m_state);
    Log(wxString::Format("%.0f,%.0f,state,,%.3f,,,,%lld,%s", ClockSync::LocalNowMs(), m_clock.ServerNowMs(),
                         d.positionSec, static_cast<long long>(m_state.version), d.videoId)
            .ToStdString());
    Reconcile(nullptr, true);
}

void MainFrame::StartSyncRound() {
    m_syncRequestsLeft = m_clock.SamplesWanted();
    OnSyncTimer();
    m_syncTimer.Start(kSyncSpacingMs);
}

void MainFrame::OnSyncTimer() {
    if (m_syncRequestsLeft <= 0 || !m_connection->SendIntent(m_clock.MakeRequest())) {
        m_syncTimer.Stop();
        m_syncRequestsLeft = 0;
        return;
    }
    --m_syncRequestsLeft;
}

void MainFrame::OnTick() {
    if (m_connection->GetStatus() == SessionConnection::Status::Connected && m_syncRequestsLeft == 0 &&
        m_clock.NeedsResync())
        StartSyncRound();

    // The answer arrives as a "status" player event, which runs Reconcile
    // and moves the seek bar.
    if (InSession() && m_player->IsReady())
        m_player->RequestStatus();

    // Let a flashed footer message expire.
    if (m_footerFlashUntilMs > 0 && ClockSync::LocalNowMs() > m_footerFlashUntilMs) {
        m_footerFlashUntilMs = 0;
        UpdateSyncStatus();
    }
}

// ---- Player -----------------------------------------------------------------

void MainFrame::OnPlayerEvent(const nlohmann::json& event) {
    const std::string type = event.value("type", "");

    if (type == "ready" || type == "stateChange" || type == "status") {
        if (type == "ready") {
            UpdateControls();
            m_player->SetVolume(m_volume->GetValue());
        }
        // While nothing is queued the player is idle; ignore what it says
        // about the last (stopped) video.
        if (m_state.CurrentEntry())
            UpdateSeekBar(event.value("currentTime", 0.0), event.value("duration", 0.0));
        else
            UpdateSeekBar(0, 0);

        if (InSession() && type == "stateChange" && event.value("state", "") == "paused" && !m_localHold &&
            ClockSync::LocalNowMs() - m_lastPauseCmdMs > 1000) {
            const auto d = m_playback.Compute(m_state);
            if (d.playing && !d.finished) {
                m_localHold = true;
                m_startTimer.Stop();
                Log(wxString::Format("%.0f,%.0f,externalPause,paused,%.3f,%.3f,,,,%s", ClockSync::LocalNowMs(),
                                     m_clock.ServerNowMs(), d.positionSec, event.value("currentTime", 0.0), d.videoId)
                        .ToStdString());
                UpdateSyncStatus();
                UpdatePlayPause();
            }
        }
        if (InSession()) {
            ReportEndAndMeta(event);
            if (type == "status")
                Reconcile(&event, false);
            else if (type == "ready")
                Reconcile(nullptr, true);
        }
    } else if (type == "error") {
        // IFrame API codes: 2 bad id, 5 HTML5 error, 100 not found,
        // 101/150 embedding disabled by the owner, 153 missing origin/referrer.
        const std::string code = event.contains("code") ? event["code"].dump() : "?";
        Log(",,playerError,,,,,,," + code + " " + event.dump());
        if (code == "\"webview2-missing\"") {
            wxMessageBox("Planar needs the Microsoft Edge WebView2 Runtime.\n"
                         "Install it from https://developer.microsoft.com/microsoft-edge/webview2/",
                         "Planar", wxICON_ERROR, this);
        }
        // Videos nobody can play (bad ID, removed, embedding disabled): every
        // client hits the same error and reports it; the server skips once.
        // The report names the failing video, since the error can arrive
        // after we've already moved on to the next entry; the server ignores
        // it unless that video is still current.
        const int numeric = event.contains("code") && event["code"].is_number_integer() ? event["code"].get<int>() : 0;
        const bool universal = numeric == 2 || numeric == 100 || numeric == 101 || numeric == 150;
        const std::string failedId = event.value("videoId", "");
        if (universal && !failedId.empty() && InSession() && m_state.CurrentEntry()) {
            const std::string key =
                failedId + ":" + std::to_string(m_state.currentIndex) + "@" + std::to_string(m_state.startedAt);
            if (key != m_unplayableSentFor) {
                m_unplayableSentFor = key;
                m_connection->SendIntent(intent::Unplayable(m_state.currentIndex, failedId, numeric));
                FlashFooter("That video can't be played here, skipping");
            }
        }
    }
}

void MainFrame::ReportEndAndMeta(const nlohmann::json& event) {
    const QueueEntry* entry = m_state.CurrentEntry();
    if (!entry || event.value("videoId", "") != entry->videoId)
        return;
    const int index = m_state.currentIndex;

    // Metadata the server can't get itself: once per entry.
    const double duration = event.value("duration", 0.0);
    const std::string title = event.value("title", "");
    if ((entry->title.empty() || entry->durationSec <= 0) && duration > 0 && !title.empty()) {
        const std::string key = std::to_string(index) + ":" + entry->videoId;
        if (m_metaSentFor.insert(key).second)
            m_connection->SendIntent(intent::Meta(index, entry->videoId, title, duration));
    }

    // Track finished: tell the server (it ignores duplicates from others).
    if (event.value("state", "") == "ended" && !m_state.IsPaused()) {
        const std::string key = std::to_string(index) + "@" + std::to_string(m_state.startedAt);
        if (key != m_endedSentFor) {
            m_endedSentFor = key;
            m_connection->SendIntent(intent::Ended(index));
            Log(",,ended,,,,,,," + entry->videoId);
        }
    }
}

void MainFrame::ArmStartTimer(const DesiredPlayback& desired) {
    // Fire early by the learned start latency, so the player is actually
    // moving at the scheduled moment.
    const double fireInMs = desired.startsInMs - m_latencyMs[kStart];
    if (desired.startsInMs > 0 && fireInMs > 0)
        m_startTimer.StartOnce(std::max(1, static_cast<int>(std::ceil(fireInMs))));
    else
        m_startTimer.Stop();
}

void MainFrame::LearnLatency(const DesiredPlayback& desired, double reportedSec) {
    if (m_pendingKind == kNone)
        return;
    // Positive = the player came out behind the timeline, so that kind of
    // command needs more lead next time.
    const double errorMs = (desired.positionSec - reportedSec) * 1000.0;
    if (TimelineKey(m_state) != m_pendingTimeline || std::abs(errorMs) > kMaxLearnErrorMs) {
        // The server moved the timeline since the command (someone seeked or
        // skipped), or the player stalled: this isn't a latency measurement.
        Log(wxString::Format(",,learnSkip%d,,,,%.1f,,,", static_cast<int>(m_pendingKind), errorMs).ToStdString());
        m_pendingKind = kNone;
        return;
    }
    double& latency = m_latencyMs[m_pendingKind];
    latency = std::clamp(latency + kLatencyGain * errorMs, 0.0, kMaxLatencyMs);
    if (m_persist)
        wxConfig::Get()->Write(wxString::Format("latency/%d", static_cast<int>(m_pendingKind)), latency);
    Log(wxString::Format(",,learn%d,,,,%.1f,%.1f,,", static_cast<int>(m_pendingKind), errorMs, latency).ToStdString());
    m_pendingKind = kNone;
}

void MainFrame::Reconcile(const nlohmann::json* status, bool force) {
    if (!InSession() || !m_player->IsReady())
        return;

    const auto d = m_playback.Compute(m_state);
    const double now = ClockSync::LocalNowMs();
    const std::string playerState = status ? status->value("state", "") : m_player->LastState();
    if (status) {
        m_lastStatusPos = status->value("currentTime", 0.0);
        m_lastStatusAtMs = now;
        Log(wxString::Format("%.0f,%.0f,status,%s,%.3f,%.3f,%.1f,%.1f,%lld,%s", now, m_clock.ServerNowMs(),
                             playerState, d.positionSec, m_lastStatusPos, (m_lastStatusPos - d.positionSec) * 1000.0,
                             m_clock.BestRttMs(), static_cast<long long>(m_state.version), d.videoId)
                .ToStdString());
    }

    if (m_localHold) {
        // Held: paused on this computer by something outside the app. Don't
        // force it to play. If it starts playing again by itself, the hold is
        // over and we go back to following the session.
        if (!IsRunning(playerState))
            return; // Play releases it
        m_localHold = false;
        UpdateSyncStatus();
        UpdatePlayPause();
        Log(",,autoReleaseHold,,,,,,,");
        force = true;
    }

    auto command = [&](const char* what, CommandKind kind = kNone) {
        m_cooldownUntilMs = now + kCooldownMs;
        m_steadyCount = 0; // any command restarts the steady-offset count
        if (std::string_view(what).starts_with("pause"))
            m_lastPauseCmdMs = now;
        m_pendingKind = kind;
        m_pendingTimeline = TimelineKey(m_state);
        Log(wxString::Format("%.0f,%.0f,%s,%s,%.3f,%.3f,,,%lld,%s", now, m_clock.ServerNowMs(), what, playerState,
                             d.positionSec, m_lastStatusPos, static_cast<long long>(m_state.version), d.videoId)
                .ToStdString());
    };

    // Nothing should be playing: back to the idle screen.
    if (d.videoId.empty()) {
        m_startTimer.Stop();
        if (!m_playerVideoId.empty()) {
            m_playerVideoId.clear();
            m_player->ShowIdle();
            UpdateSeekBar(0, 0);
            command("idle");
        }
        return;
    }

    // A different track: load it at the right spot. loadVideoById starts
    // playing on its own; cueVideoById just gets it ready.
    if (d.videoId != m_playerVideoId) {
        m_playerVideoId = d.videoId;
        m_haveDrift = false;
        if (d.playing) {
            m_player->LoadVideo(d.videoId, d.positionSec + m_latencyMs[kStart] / 1000.0);
            command("load", kStart);
        } else {
            m_player->CueVideo(d.videoId, d.positionSec);
            m_lastStatusPos = d.positionSec;
            command("cue");
        }
        ArmStartTimer(d);
        return;
    }

    ArmStartTimer(d);
    if (!force && now < m_cooldownUntilMs)
        return; // let the last command take effect first

    // Track is over; the "ended" report will move everyone on.
    if (d.finished || playerState == "ended")
        return;

    const double toleranceMs = PlaybackController::kDriftToleranceSec * 1000.0;

    if (!d.playing) {
        if (IsRunning(playerState)) {
            // Pause, then park exactly where the server says we paused, so
            // every client stops at the same spot even though the message
            // reached them at different times.
            m_player->Pause();
            m_player->SeekTo(d.positionSec);
            m_lastStatusPos = d.positionSec;
            command("pause");
        } else if (d.startsInMs > 0 && d.startsInMs <= m_latencyMs[kStart] + 1) {
            // A scheduled start is due (the timer fired, early by the start
            // latency): press play now so the video is moving on time.
            m_player->Play();
            command("start", kStart);
        } else if (status && std::abs(m_lastStatusPos - d.positionSec) * 1000.0 > toleranceMs) {
            m_player->SeekTo(d.positionSec); // e.g. a seek while paused
            m_lastStatusPos = d.positionSec;
            command("seekPaused");
        }
        return;
    }

    // Should be playing, but the player is parked.
    if (!IsRunning(playerState)) {
        const double aheadMs = (m_lastStatusPos - d.positionSec) * 1000.0;
        if (m_lastStatusPos >= 0 && aheadMs > 0 && aheadMs <= kMaxWaitMs) {
            // Parked slightly ahead of the timeline: the usual resume, which
            // the server schedules LEAD ms in the future. Wait for the
            // timeline to arrive rather than seeking backwards, pressing play
            // early by the learned resume latency.
            if (aheadMs > m_latencyMs[kResume]) {
                m_startTimer.StartOnce(std::max(1, static_cast<int>(aheadMs - m_latencyMs[kResume])));
                return;
            }
            m_player->Play();
            command("resume", kResume);
            return;
        }
        if (m_lastStatusPos < 0 || std::abs(aheadMs) > toleranceMs) {
            m_player->SeekTo(d.positionSec + m_latencyMs[kSeek] / 1000.0);
            m_player->Play();
            command("seekPlay", kSeek);
        } else {
            m_player->Play();
            command("resume", kResume);
        }
        return;
    }

    // New server state while playing (e.g. the host seeked): act now rather
    // than waiting up to a poll interval for the next status. Where the
    // player is now is predictable from the last report.
    if (!status && force && playerState == "playing" && m_lastStatusAtMs >= 0 && now - m_lastStatusAtMs < 2000) {
        const double predicted = m_lastStatusPos + (now - m_lastStatusAtMs) / 1000.0;
        if (std::abs(predicted - d.positionSec) * 1000.0 > toleranceMs) {
            m_player->SeekTo(d.positionSec + m_latencyMs[kSeek] / 1000.0);
            command("seekNow", kSeek);
        }
        return;
    }

    if (status && playerState == "playing") {
        m_lastDriftMs = (m_lastStatusPos - d.positionSec) * 1000.0;
        m_haveDrift = true;
        UpdateSyncStatus();
        LearnLatency(d, m_lastStatusPos);

        const double errorSec = m_lastStatusPos - d.positionSec;
        const int sign = errorSec > 0 ? 1 : -1;
        if (std::abs(errorSec) > PlaybackController::kSteadyToleranceSec) {
            m_steadyCount = sign == m_steadySign ? m_steadyCount + 1 : 1;
            m_steadySign = sign;
        } else {
            m_steadyCount = 0;
        }

        auto target = m_playback.CorrectionFor(d, m_lastStatusPos);
        const char* why = "correct";
        if (!target && m_steadyCount >= PlaybackController::kSteadySamples && !d.finished) {
            target = d.positionSec;
            why = "correctSteady";
        }
        if (target) {
            // Aim ahead by the learned seek latency: the playhead moves on
            // while the seek rebuffers.
            m_player->SeekTo(*target + m_latencyMs[kSeek] / 1000.0);
            m_steadyCount = 0;
            command(why, kSeek);
        }
    }
}

// ---- Display ----------------------------------------------------------------

bool MainFrame::IsHost() const {
    return !m_myMemberId.empty() && m_state.hostId == m_myMemberId;
}

void MainFrame::UpdateControls() {
    // Start page
    if (m_pages->GetSelection() == 0)
        m_joinBtn->Enable(m_codeText->IsEnabled() && IsRoomCode(m_codeText->GetValue()));

    // Room page
    const bool ready = m_player->IsReady();
    const bool connected = m_connection && m_connection->GetStatus() == SessionConnection::Status::Connected;
    const bool online = connected && ready;
    const bool host = online && IsHost();
    const bool haveTrack = m_state.CurrentEntry() != nullptr;

    m_playPauseBtn->Enable(online && (haveTrack || !m_state.queue.empty() || m_localHold));
    m_skipBtn->Enable(host && haveTrack);
    m_seekBar->Enable(host && haveTrack && m_durationSec > 0);
    m_searchCtrl->Enable(connected);
    m_queueList->Enable(connected);
}

void MainFrame::UpdatePlayPause() {
    const bool showPause = !m_localHold && m_state.CurrentEntry() && !m_state.IsPaused();
    m_playPauseBtn->SetLabel(U8(showPause ? kGlyphPause : kGlyphPlay));
    m_playPauseBtn->SetToolTip(showPause ? "Pause for everyone" : (m_localHold ? "Rejoin the group" : "Play"));
}

void MainFrame::RefreshQueue() {
    wxArrayString rows;
    for (size_t i = 0; i < m_state.queue.size(); ++i) {
        const auto& e = m_state.queue[i];
        std::string row = static_cast<int>(i) == m_state.currentIndex ? std::string(kGlyphPlay) + "  " : "     ";
        row += e.title.empty() ? e.videoId : e.title;
        if (e.durationSec > 0)
            row += std::string("  ") + kGlyphDot + "  " + FormatTime(e.durationSec);
        rows.Add(U8(row));
    }

    const int sel = m_queueList->GetSelection();
    m_queueList->Set(rows);
    if (sel != wxNOT_FOUND && sel < static_cast<int>(rows.size()))
        m_queueList->SetSelection(sel);
}

void MainFrame::UpdateHeader() {
    const size_t n = m_state.members.size();
    wxString text = n == 1 ? wxString("1 listening") : wxString::Format("%zu listening", n);
    if (IsHost())
        text += U8(std::string("  ") + kGlyphDot + "  you're the host");
    m_listenersLabel->SetLabel(n == 0 ? wxString() : text);

    // Who's here, in the tooltip.
    wxString who;
    for (const auto& m : m_state.members) {
        if (!who.empty())
            who += "\n";
        who += U8(m.name);
        if (m_state.hostId && m.id == *m_state.hostId)
            who += " (host)";
        if (m.IsAway())
            who += " (away)";
    }
    m_listenersLabel->SetToolTip(who);
    m_listenersLabel->GetParent()->Layout();
}

void MainFrame::FlashFooter(const wxString& message, bool isError) {
    m_footer->SetForegroundColour(isError ? theme::kError : theme::kMuted);
    m_footer->SetLabel(message);
    m_footer->SetToolTip(message);
    m_footerFlashUntilMs = ClockSync::LocalNowMs() + kFooterFlashMs;
}

void MainFrame::UpdateSyncStatus() {
    if (m_footerFlashUntilMs > 0)
        return; // a flashed message is showing; OnTick restores this later
    using S = SessionConnection::Status;
    wxString text;
    wxString detail;
    if (m_connection->GetStatus() != S::Connected) {
        text = U8(std::string("Connecting") + kGlyphEllipsis);
    } else if (m_localHold) {
        text = U8(std::string("Paused on this computer ") + kGlyphDash + " press " + kGlyphPlay + " to rejoin");
    } else if (!m_clock.HasEstimate()) {
        text = U8(std::string("Syncing") + kGlyphEllipsis);
    } else if (!m_state.CurrentEntry()) {
        text = "Connected";
    } else {
        text = m_haveDrift && std::abs(m_lastDriftMs) > PlaybackController::kDriftToleranceSec * 1000.0
                   ? U8(std::string("Catching up") + kGlyphEllipsis)
                   : wxString("In sync");
        detail = wxString::Format("Round trip %.0f ms", m_clock.BestRttMs());
        if (m_haveDrift)
            detail += wxString::Format(", offset %+.0f ms", m_lastDriftMs);
    }
    m_footer->SetForegroundColour(theme::kMuted);
    m_footer->SetLabel(text);
    m_footer->SetToolTip(detail.empty() ? text : detail);
}

void MainFrame::Log(const std::string& line) {
    if (m_log.is_open())
        m_log << line << '\n' << std::flush;
}
