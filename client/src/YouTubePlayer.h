#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>
#include <wx/panel.h>

class wxWebView;
class wxWebViewEvent;

// A visible panel hosting the YouTube IFrame player (web/player.html) inside
// a wxWebView (WebView2 on Windows, WKWebView on macOS).
//
// This class is purely a transport: it forwards commands to the JS player and
// reports player events back. It never decides what should be playing; that's
// PlaybackController's job.
class YouTubePlayer : public wxPanel {
public:
    // Payload is the JSON object posted by player.html, always with a "type"
    // field: "ready", "stateChange" or "status" (all three carry state,
    // currentTime, duration, videoId, title) or "error" (code).
    using EventHandler = std::function<void(const nlohmann::json&)>;

    // Starts on an idle screen (no video) until LoadVideo/CueVideo.
    explicit YouTubePlayer(wxWindow* parent);

    void SetEventHandler(EventHandler handler) { m_onEvent = std::move(handler); }

    // False until the IFrame API has fired onReady. Commands sent before that
    // are dropped by the JS side.
    bool IsReady() const { return m_ready; }

    void Play();
    void Pause();
    void SeekTo(double seconds);
    void LoadVideo(const std::string& videoId, double startSeconds = 0.0);
    void CueVideo(const std::string& videoId, double startSeconds = 0.0);
    void SetVolume(int percent); // local only, never synced
    // Stops the video and shows the idle screen again.
    void ShowIdle();

    // Asks the page for a "status" event (current state + position).
    void RequestStatus();

    // Last state the player reported: "unstarted", "ended", "playing",
    // "paused", "buffering", "cued", or "" before the first report.
    const std::string& LastState() const { return m_lastState; }

private:
    void OnLoaded(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void OnWebViewError(wxWebViewEvent& evt);

#ifdef __WXMSW__
    bool MapVirtualHost();
#endif
    // Loads web/player.html with a real https origin.
    void LoadPlayerPage();
    void Call(const std::string& js);
    void Emit(const nlohmann::json& event);

    wxWebView* m_webView = nullptr;
    bool m_hostMapped = false;
    bool m_ready = false;
    std::string m_lastState;
    EventHandler m_onEvent;
};
