#pragma once

#include <functional>
#include <string>

#include <wx/panel.h>

class wxWebView;
class wxWebViewEvent;

// An in-app YouTube browser for finding songs: YouTube's mobile site, which
// suits the narrow side column, in a wxWebView.
//
// Picking a video doesn't open it. A script in the page catches clicks on
// video links and reports the video through the pick handler instead, so
// browsing and searching cost no YouTube Data API quota. The page never
// plays anything itself; playback stays with YouTubePlayer.
class YouTubeBrowser : public wxPanel {
public:
    struct Pick {
        std::string videoId;
        std::string title;      // best effort, from the page; may be empty
        double durationSec = 0; // best effort; 0 if unknown
    };
    using PickHandler = std::function<void(const Pick&)>;

    // The web view is only created on first use (Home or Search).
    explicit YouTubeBrowser(wxWindow* parent);

    void SetPickHandler(PickHandler handler) { m_onPick = std::move(handler); }

    // YouTube's home page, unless the browser is already showing something.
    void Home();
    void Search(const std::string& query);

private:
    void EnsureCreated();
    void Navigate(const std::string& url);
    void OnLoaded(wxWebViewEvent& evt);
    void OnNavigating(wxWebViewEvent& evt);
    void OnNewWindow(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
#ifdef __WXMSW__
    bool ApplyUserAgent();
#endif

    wxWebView* m_webView = nullptr;
    // Windows: WebView2 is created asynchronously, so the first URL waits for
    // the initial about:blank to finish loading (see YouTubePlayer).
    bool m_nativeReady = false;
    std::string m_pendingUrl;
    bool m_navigated = false;
    PickHandler m_onPick;
};
