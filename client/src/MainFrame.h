#pragma once

#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <wx/frame.h>
#include <wx/timer.h>

#include "ClockSync.h"
#include "PlaybackController.h"
#include "RelayClient.h"
#include "SessionConnection.h"
#include "SessionState.h"

class wxButton;
class wxListBox;
class wxListCtrl;
class wxPanel;
class wxSearchCtrl;
class wxSimplebook;
class wxSlider;
class wxStaticText;
class wxTextCtrl;
class YouTubePlayer;

// Planar's relay on Cloudflare. Not shown or editable in the UI; --relay
// overrides it for testing against `wrangler dev`.
inline constexpr const char* kDefaultRelayUrl = "wss://music-player-relay.bennyjjumpin05.workers.dev";

// Settings that can come from the command line (see App.cpp).
struct LaunchOptions {
    std::string relayUrl = kDefaultRelayUrl;
    std::string name;        // remembered between runs
    std::string roomCode;    // --room: join this room on startup (testing)
    bool createRoom = false; // --create: create a room on startup (testing)
    std::string clientId;    // persistent identity (see App.cpp)
    bool persist = true;     // remember name/volume/latencies in the registry
    int volume = 100;        // local only
    std::string logPath;     // optional CSV log of sync measurements
};

class MainFrame : public wxFrame {
public:
    explicit MainFrame(const LaunchOptions& options);

private:
    // ---- UI construction
    wxPanel* BuildStartPage(wxWindow* parent);
    wxPanel* BuildRoomPage(wxWindow* parent);
    void BindRoomControls();
    void BindQueueDrag();
    void WireSession();

    // ---- Start page
    void OnCreateRoom();
    void OnJoinRoom();
    void EnterRoom(const std::string& code);
    // Back to the start page, optionally explaining why.
    void ShowStartPage(const wxString& message = {}, bool isError = false);
    void SetStartBusy(bool busy);
    std::string CurrentName() const;

    // ---- Room page actions
    void OnLeave();
    void OnSearchEnter();
    void ShowQueueList();
    void ShowSearchResults();
    void OnAddSearchResult();
    void ShowQueueMenu();
    void MoveSelected(int delta);
    void RemoveSelected();
    void OnPlayPause();
    void OnSeekBarChanged();
    void UpdateSeekBar(double positionSec, double durationSec);
    bool Send(const nlohmann::json& intent); // false (with a footer note) if offline

    // ---- Session
    void OnSessionStatus(SessionConnection::Status status);
    void OnSessionState(const SessionState& state);
    void StartSyncRound();
    void OnSyncTimer();
    void OnTick();
    bool InSession() const;

    // ---- Player
    void OnPlayerEvent(const nlohmann::json& event);
    // Brings the player in line with what PlaybackController says should be
    // happening. `status` is a fresh player report, if this was triggered by
    // one. `force` skips the post-command cooldown (new server state, a
    // scheduled start, etc.).
    void Reconcile(const nlohmann::json* status, bool force);
    void ArmStartTimer(const DesiredPlayback& desired);
    // After a command settles, compare where the player ended up with where
    // it should be, and adjust that command kind's latency estimate.
    void LearnLatency(const DesiredPlayback& desired, double reportedSec);
    void ReportEndAndMeta(const nlohmann::json& event);

    // ---- Display
    void RefreshQueue();
    void UpdateHeader();
    void UpdateSyncStatus();
    void UpdatePlayPause();
    // A short message in the footer, shown for a few seconds instead of the
    // sync status.
    void FlashFooter(const wxString& message, bool isError = false);
    // Enables/disables controls by connection state and permissions.
    void UpdateControls();
    bool IsHost() const;
    bool CanRemove(int index) const;
    void Log(const std::string& line);

    // ---- Widgets
    wxSimplebook* m_pages = nullptr;
    // Start page
    wxTextCtrl* m_nameText = nullptr;
    wxButton* m_createBtn = nullptr;
    wxTextCtrl* m_codeText = nullptr;
    wxButton* m_joinBtn = nullptr;
    wxStaticText* m_startStatus = nullptr;
    // Room page: header
    wxStaticText* m_roomCodeLabel = nullptr;
    wxButton* m_copyBtn = nullptr;
    wxStaticText* m_listenersLabel = nullptr;
    wxButton* m_leaveBtn = nullptr;
    // Room page: body
    YouTubePlayer* m_player = nullptr;
    wxSearchCtrl* m_searchCtrl = nullptr;
    wxStaticText* m_listTitle = nullptr;
    wxButton* m_backBtn = nullptr;
    wxSimplebook* m_listBook = nullptr;
    wxListBox* m_queueList = nullptr;
    wxListCtrl* m_searchList = nullptr;
    std::vector<SearchResult> m_searchResults;
    // Room page: seek bar and controls
    wxSlider* m_seekBar = nullptr;
    wxStaticText* m_posLabel = nullptr;
    wxStaticText* m_durLabel = nullptr;
    wxButton* m_playPauseBtn = nullptr;
    wxButton* m_skipBtn = nullptr;
    wxStaticText* m_footer = nullptr;
    wxSlider* m_volume = nullptr;
    double m_durationSec = 0; // of the video in the player
    bool m_seekDragging = false;
    int m_dragFrom = -1; // drag-to-reorder in the queue list
    double m_footerFlashUntilMs = 0;

    // ---- Session plumbing
    std::string m_relayUrl;
    std::unique_ptr<RelayClient> m_relay;
    ClockSync m_clock;
    PlaybackController m_playback{m_clock};
    std::unique_ptr<SessionConnection> m_connection;
    SessionState m_state;
    bool m_haveState = false;
    std::string m_roomCode;
    std::string m_myMemberId; // from the server's "welcome"
    std::string m_clientId;
    bool m_persist = true;

    // ---- Player bookkeeping
    std::string m_playerVideoId;   // what we last told the player to load/cue
    double m_cooldownUntilMs = 0;  // local ms; no corrections before this
    double m_lastStatusPos = -1;   // last position the player reported (or was parked at)
    double m_lastDriftMs = 0;
    bool m_haveDrift = false;
    // Something outside the app (a click on the video, media keys, the OS)
    // paused the player while the session is playing. Rather than fight it,
    // this computer holds until the user presses Play; others are unaffected.
    bool m_localHold = false;
    int m_steadyCount = 0; // consecutive same-sign samples beyond kSteadyToleranceSec
    int m_steadySign = 0;
    double m_lastPauseCmdMs = -1e18; // local ms of our own last Pause()
    // How long each kind of player command takes to take effect, learned
    // from measurements (see LearnLatency). Commands are issued this much
    // early (or seek this much ahead) to compensate.
    enum CommandKind { kStart, kResume, kSeek, kNone };
    // Defaults are typical values measured on a Windows 11 / WebView2 machine;
    // learned values are saved in the settings, since they are properties of
    // the machine and its connection.
    double m_latencyMs[3] = {220, 150, 130};
    CommandKind m_pendingKind = kNone;
    // The session timeline (index, startedAt, paused) when the pending
    // command was issued. If the server changes it before we measure (a seek,
    // a skip, a pause), the measurement says nothing about our latency.
    std::string m_pendingTimeline;
    double m_lastStatusAtMs = -1; // local ms of the last player status

    std::string m_endedSentFor;      // "index@startedAt" we already reported ended
    std::string m_unplayableSentFor; // likewise for unplayable
    std::set<std::string> m_metaSentFor;

    int m_syncRequestsLeft = 0;
    wxTimer m_syncTimer;  // spaces out timeSync requests within a round
    wxTimer m_tickTimer;  // status polling, drift checks, resync
    wxTimer m_startTimer; // fires at a scheduled start

    std::ofstream m_log;
};
