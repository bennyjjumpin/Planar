#include "YouTubeBrowser.h"

#include <optional>
#include <regex>

#include <nlohmann/json.hpp>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include "RelayClient.h"
#include "VideoId.h"

#ifdef __WXMSW__
// See YouTubePlayer.cpp: WebView2.h needs the COM declarations that
// WIN32_LEAN_AND_MEAN strips out.
#include <ole2.h>
#include <WebView2.h>
#endif

namespace {

#ifdef __WXMSW__
const char* const kBackend = wxWebViewBackendEdge;
#else
const char* const kBackend = wxWebViewBackendDefault; // WebKit on macOS
#endif

// A phone's user agent gets the mobile site, which fits the side column. It
// names the same engine the web view really is, so YouTube doesn't serve
// code for a browser we aren't.
#ifdef __WXMSW__
constexpr const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/138.0.0.0 Mobile Safari/537.36";
#else
constexpr const char* kUserAgent =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/18.5 Mobile/15E148 Safari/604.1";
#endif

constexpr const char* kHomeUrl = "https://m.youtube.com/";

// Name of the JS object wxWebView injects (window.planar.postMessage).
constexpr const char* kMessageHandler = "planar";

// Runs in every page the browser loads. Clicks on links to a video are
// stopped before YouTube's own handlers see them and reported instead. As a
// fallback for navigations that don't come from a link (the site is a
// single-page app), a watch page that is reached anyway is reported and left
// at once.
constexpr const char* kPickerScript = R"JS(
(function () {
  if (window.top !== window || window.__planarPicker) return;
  window.__planarPicker = true;

  function videoIdFrom(href) {
    var u;
    try { u = new URL(href, location.href); } catch (e) { return null; }
    if (!/(^|\.)youtube\.com$/.test(u.hostname)) return null;
    var m = u.pathname === "/watch" ? /^[A-Za-z0-9_-]{11}$/.exec(u.searchParams.get("v") || "")
                                    : /^\/shorts\/([A-Za-z0-9_-]{11})/.exec(u.pathname);
    return m ? (m[1] || m[0]) : null;
  }

  function clean(s) { return (s || "").replace(/\s+/g, " ").trim(); }

  // Title and duration from the list item around the link, best effort:
  // YouTube's markup changes, and the player fills in both later anyway.
  function describe(a) {
    var item = a.closest("ytm-video-with-context-renderer, ytm-compact-video-renderer, " +
                         "ytm-rich-item-renderer, ytm-media-item, ytm-video-card-renderer, " +
                         "ytm-playlist-video-renderer, ytm-reel-item-renderer") || a;
    var heading = item.querySelector("h3, h4, .media-item-headline");
    var title = clean(heading ? heading.textContent : "");
    var duration = 0;
    var nodes = item.querySelectorAll("span, div");
    for (var i = 0; i < nodes.length && !duration; i++) {
      if (nodes[i].children.length) continue;
      var t = /^(?:(\d+):)?(\d{1,2}):(\d{2})$/.exec(clean(nodes[i].textContent));
      if (t) duration = (+(t[1] || 0)) * 3600 + (+t[2]) * 60 + (+t[3]);
    }
    return { title: title, durationSec: duration };
  }

  var lastId = "", lastAt = 0;
  function pick(id, info) {
    var now = Date.now();
    if (id === lastId && now - lastAt < 1500) return;
    lastId = id;
    lastAt = now;
    try {
      window.planar.postMessage(JSON.stringify({
        type: "pick", videoId: id, title: info.title, durationSec: info.durationSec }));
    } catch (e) {}
  }

  window.addEventListener("click", function (e) {
    var a = e.target && e.target.closest ? e.target.closest("a[href]") : null;
    var id = a && videoIdFrom(a.href);
    if (!id) return;
    e.preventDefault();
    e.stopImmediatePropagation();
    pick(id, describe(a));
  }, true);

  setInterval(function () {
    var id = videoIdFrom(location.href);
    if (!id) return;
    document.querySelectorAll("video").forEach(function (v) { v.muted = true; v.pause(); });
    pick(id, { title: clean(document.title).replace(/ - YouTube$/, ""), durationSec: 0 });
    if (history.length > 1) history.back();
    else location.replace("https://m.youtube.com/");
  }, 250);
})();
)JS";

// The video a watch or Shorts page on YouTube shows. Not /embed/ URLs, which
// ads and other frames use.
std::optional<std::string> VideoPageId(const std::string& url) {
    static const std::regex page(R"(^https?://([a-z]+\.)?youtube\.com/(watch\?|shorts/))");
    if (!std::regex_search(url, page))
        return std::nullopt;
    return ParseVideoId(url);
}

} // namespace

YouTubeBrowser::YouTubeBrowser(wxWindow* parent) : wxPanel(parent) {
    SetSizer(new wxBoxSizer(wxVERTICAL));
}

void YouTubeBrowser::EnsureCreated() {
    if (m_webView || !wxWebView::IsBackendAvailable(kBackend))
        return;

    m_webView = wxWebView::New(this, wxID_ANY, "about:blank", wxDefaultPosition, wxDefaultSize, kBackend);
    m_webView->AddScriptMessageHandler(kMessageHandler);
    m_webView->AddUserScript(kPickerScript);
    m_webView->EnableContextMenu(false);
    GetSizer()->Add(m_webView, 1, wxEXPAND);
    Layout();

    m_webView->Bind(wxEVT_WEBVIEW_LOADED, &YouTubeBrowser::OnLoaded, this);
    m_webView->Bind(wxEVT_WEBVIEW_NAVIGATING, &YouTubeBrowser::OnNavigating, this);
    m_webView->Bind(wxEVT_WEBVIEW_NEWWINDOW, &YouTubeBrowser::OnNewWindow, this);
    m_webView->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &YouTubeBrowser::OnScriptMessage, this);

#ifndef __WXMSW__
    // WKWebView exists right away and takes a user agent at any time.
    m_webView->SetUserAgent(kUserAgent);
    m_nativeReady = true;
#endif
}

void YouTubeBrowser::Home() {
    if (!m_navigated)
        Navigate(kHomeUrl);
}

void YouTubeBrowser::Search(const std::string& query) {
    Navigate(std::string(kHomeUrl) + "results?search_query=" + RelayClient::UrlEncode(query));
}

void YouTubeBrowser::Navigate(const std::string& url) {
    EnsureCreated();
    if (!m_webView)
        return;
    m_navigated = true;
    if (m_nativeReady)
        m_webView->LoadURL(wxString::FromUTF8(url));
    else
        m_pendingUrl = url; // OnLoaded picks it up
}

#ifdef __WXMSW__
// wx 3.2 can only pass a user agent to WebView2 as an environment option, and
// the environment is shared with the player's web view, whose options must
// match. The settings API sets it for this view alone.
bool YouTubeBrowser::ApplyUserAgent() {
    auto* core = static_cast<ICoreWebView2*>(m_webView->GetNativeBackend());
    if (!core)
        return false;
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(core->get_Settings(&settings)) || !settings)
        return false;
    ICoreWebView2Settings2* settings2 = nullptr;
    const bool ok = SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings2))) && settings2 &&
                    SUCCEEDED(settings2->put_UserAgent(kUserAgent));
    if (settings2)
        settings2->Release();
    settings->Release();
    return ok;
}
#endif

void YouTubeBrowser::OnLoaded(wxWebViewEvent& evt) {
    evt.Skip();
    if (m_nativeReady)
        return;
#ifdef __WXMSW__
    // The first load is about:blank: the native view exists now. Without the
    // user agent YouTube serves the desktop site, which still works.
    ApplyUserAgent();
#endif
    m_nativeReady = true;
    if (!m_pendingUrl.empty()) {
        m_webView->LoadURL(wxString::FromUTF8(m_pendingUrl));
        m_pendingUrl.clear();
    }
}

void YouTubeBrowser::OnNavigating(wxWebViewEvent& evt) {
    // A real page load of a video (the page script catches most picks before
    // they get this far).
    const auto id = VideoPageId(evt.GetURL().utf8_string());
    if (!id)
        return;
    evt.Veto();
    if (m_onPick)
        m_onPick({*id, {}, 0});
}

void YouTubeBrowser::OnNewWindow(wxWebViewEvent& evt) {
    // Links that want a new window: videos are picks, anything else (a
    // channel's website, an ad) opens in the user's own browser.
    if (const auto id = VideoPageId(evt.GetURL().utf8_string())) {
        if (m_onPick)
            m_onPick({*id, {}, 0});
    } else if (evt.GetURL().StartsWith("http")) {
        wxLaunchDefaultBrowser(evt.GetURL());
    }
}

void YouTubeBrowser::OnScriptMessage(wxWebViewEvent& evt) {
    const auto msg = nlohmann::json::parse(evt.GetString().utf8_string(), nullptr, false);
    if (msg.is_discarded() || !msg.is_object() || msg.value("type", "") != "pick")
        return;
    Pick pick;
    pick.videoId = msg.value("videoId", "");
    pick.title = msg.value("title", "");
    pick.durationSec = msg.value("durationSec", 0.0);
    if (m_onPick && ParseVideoId(pick.videoId) == pick.videoId)
        m_onPick(pick);
}
