#include "YouTubePlayer.h"

#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/webview.h>

#ifdef __WXMSW__
// WebView2.h relies on COM declarations that WIN32_LEAN_AND_MEAN (set for
// the whole target, to keep winsock happy) strips out of <windows.h>.
#include <ole2.h>
#include <WebView2.h>
#endif

namespace {

// Virtual host that WebView2 maps onto <exe dir>/web. Any hostname works as
// long as it never resolves for real: ".example" is reserved for that
// (RFC 2606). Avoid ".local", which triggers slow mDNS lookups on Windows.
// On macOS the page is loaded with this URL as its base instead, which gives
// it the same origin.
#ifdef __WXMSW__
constexpr const wchar_t* kVirtualHost = L"planar.example";
#endif
constexpr const char* kPlayerUrl = "https://planar.example/player.html";

#ifdef __WXMSW__
const char* const kBackend = wxWebViewBackendEdge;
#else
const char* const kBackend = wxWebViewBackendDefault; // WebKit on macOS
#endif

// Name of the JS object wxWebView injects: player.html calls
// window.wx.postMessage(...).
constexpr const char* kMessageHandler = "wx";

wxString WebDir() {
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    exe.AppendDir("web");
    return exe.GetPath();
}

// Formats a value as a JS literal. JSON is valid JS, and unlike
// std::to_string this ignores the C locale (no "1,5" decimals).
template <typename T>
std::string Js(const T& value) {
    return nlohmann::json(value).dump();
}

} // namespace

YouTubePlayer::YouTubePlayer(wxWindow* parent) : wxPanel(parent) {
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(sizer);

    if (!wxWebView::IsBackendAvailable(kBackend)) {
        // Happens if the Evergreen WebView2 runtime isn't installed (it ships
        // with Windows 11, but can be missing on older Windows 10 machines).
        // Deferred so the owner has a chance to call SetEventHandler first.
        CallAfter([this] { Emit({{"type", "error"}, {"code", "webview2-missing"}}); });
        return;
    }

    // Start on about:blank. wx 3.2 creates the WebView2 asynchronously and
    // gives us no "created" event, so the first wxEVT_WEBVIEW_LOADED (for
    // about:blank) is our signal that the native ICoreWebView2 exists and we
    // can set up the virtual host before navigating to the real page.
    m_webView = wxWebView::New(this, wxID_ANY, "about:blank", wxDefaultPosition,
                               wxDefaultSize, kBackend);
    m_webView->AddScriptMessageHandler(kMessageHandler);
    m_webView->EnableContextMenu(false);
    sizer->Add(m_webView, 1, wxEXPAND);

    m_webView->Bind(wxEVT_WEBVIEW_LOADED, &YouTubePlayer::OnLoaded, this);
    m_webView->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &YouTubePlayer::OnScriptMessage, this);
    m_webView->Bind(wxEVT_WEBVIEW_ERROR, &YouTubePlayer::OnWebViewError, this);

#ifndef __WXMSW__
    // WKWebView is created synchronously, so there's nothing to wait for (and
    // wx doesn't report a load event for about:blank there anyway).
    m_hostMapped = true;
    LoadPlayerPage();
#endif
}

#ifdef __WXMSW__
bool YouTubePlayer::MapVirtualHost() {
    auto* core = static_cast<ICoreWebView2*>(m_webView->GetNativeBackend());
    if (!core)
        return false;

    // SetVirtualHostNameToFolderMapping lives on ICoreWebView2_3, which every
    // supported WebView2 runtime implements.
    ICoreWebView2_3* core3 = nullptr;
    if (FAILED(core->QueryInterface(IID_PPV_ARGS(&core3))) || !core3)
        return false;

    const HRESULT hr = core3->SetVirtualHostNameToFolderMapping(
        kVirtualHost, WebDir().wc_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    core3->Release();
    return SUCCEEDED(hr);
}
#endif

void YouTubePlayer::LoadPlayerPage() {
#ifdef __WXMSW__
    if (!MapVirtualHost()) {
        Emit({{"type", "error"}, {"code", "virtual-host-failed"}});
        return;
    }
    m_webView->LoadURL(kPlayerUrl);
#else
    // WKWebView has no folder mapping, but loading the HTML with an https
    // base URL gives the page that origin, which is all YouTube checks.
    wxString html;
    wxFFile file(WebDir() + wxFILE_SEP_PATH + "player.html", "rb");
    if (!file.IsOpened() || !file.ReadAll(&html, wxConvUTF8)) {
        Emit({{"type", "error"}, {"code", "player-page-missing"}});
        return;
    }
    m_webView->SetPage(html, kPlayerUrl);
#endif
}

void YouTubePlayer::OnLoaded(wxWebViewEvent& evt) {
    evt.Skip();
    if (m_hostMapped)
        return;

    m_hostMapped = true;
    LoadPlayerPage();
}

void YouTubePlayer::OnScriptMessage(wxWebViewEvent& evt) {
    const auto msg = nlohmann::json::parse(evt.GetString().utf8_string(), nullptr, false);
    if (msg.is_discarded() || !msg.is_object() || !msg.contains("type"))
        return;

    if (msg["type"] == "ready")
        m_ready = true;
    if (msg.contains("state") && msg["state"].is_string())
        m_lastState = msg["state"].get<std::string>();
    Emit(msg);
}

void YouTubePlayer::OnWebViewError(wxWebViewEvent& evt) {
    Emit({{"type", "error"},
          {"code", "navigation-failed"},
          {"url", evt.GetURL().utf8_string()},
          {"detail", evt.GetString().utf8_string()}});
}

void YouTubePlayer::Call(const std::string& js) {
    if (!m_webView || !m_ready)
        return;
    // RunScriptAsync rather than RunScript: in wx 3.2, RunScript on Edge
    // blocks by pumping the event loop until the result arrives, which can
    // re-enter our own handlers. Fire-and-forget is all we need here.
    m_webView->RunScriptAsync(wxString::FromUTF8(js));
}

void YouTubePlayer::Emit(const nlohmann::json& event) {
    if (m_onEvent)
        m_onEvent(event);
}

void YouTubePlayer::Play() { Call("app.play()"); }

void YouTubePlayer::Pause() { Call("app.pause()"); }

void YouTubePlayer::SeekTo(double seconds) {
    Call("app.seekTo(" + Js(seconds) + ")");
}

void YouTubePlayer::LoadVideo(const std::string& videoId, double startSeconds) {
    Call("app.load(" + Js(videoId) + "," + Js(startSeconds) + ")");
}

void YouTubePlayer::CueVideo(const std::string& videoId, double startSeconds) {
    Call("app.cue(" + Js(videoId) + "," + Js(startSeconds) + ")");
}

void YouTubePlayer::RequestStatus() { Call("app.reportStatus()"); }

void YouTubePlayer::ShowIdle() { Call("app.idle()"); }

void YouTubePlayer::SetVolume(int percent) {
    Call("app.setVolume(" + Js(percent) + ")");
}
