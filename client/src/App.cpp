#include <algorithm>
#include <random>

#include <wx/app.h>
#include <wx/cmdline.h>
#include <wx/config.h>
#include <wx/utils.h>

#include <ixwebsocket/IXNetSystem.h>

#include "MainFrame.h"

class App : public wxApp {
public:
    bool OnInit() override {
        // Settings live in HKCU\Software\Planar.
        SetAppName("Planar");
        SetAppDisplayName("Planar");

        // Parses the command line (OnInitCmdLine / OnCmdLineParsed below).
        if (!wxApp::OnInit())
            return false;

        LoadIdentity();

        // Chromium's autoplay policy blocks unmuted playVideo() calls that
        // don't come from a click *inside* the page. Our native buttons don't
        // count, and neither does a play triggered by a server broadcast. wx
        // 3.2 has no API for WebView2 environment options, but WebView2 reads
        // this variable when it starts, so it must be set before any webview
        // exists.
        //
        // HardwareMediaKeyHandling: otherwise the embedded video registers
        // with Windows' media controls, and media keys, the volume flyout or
        // Bluetooth headphones can pause it behind the app's back. The
        // server decides what plays, so OS media controls must not reach the
        // player. (Seen in testing: a friend's player kept pausing itself.)
        wxSetEnv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                 "--autoplay-policy=no-user-gesture-required --disable-features=HardwareMediaKeyHandling");

        // IXWebSocket needs WSAStartup on Windows; it doesn't do it itself.
        ix::initNetSystem();

        auto* frame = new MainFrame(m_options);
        frame->Show();
        return true;
    }

    int OnExit() override {
        ix::uninitNetSystem();
        return wxApp::OnExit();
    }

    // Normal use needs no options. For testing, e.g.:
    //   Planar --relay ws://localhost:8787 --create --name Ann
    //   Planar --room 4821 --name Bob --client-id bob_test_identity_01
    void OnInitCmdLine(wxCmdLineParser& parser) override {
        wxApp::OnInitCmdLine(parser);
        parser.AddOption("", "relay", "relay URL (default: Planar's server), e.g. ws://localhost:8787");
        parser.AddOption("", "room", "join this 4-digit room on startup");
        parser.AddSwitch("", "create", "create a room on startup");
        parser.AddOption("", "name", "display name");
        parser.AddOption("", "volume", "local volume 0-100", wxCMD_LINE_VAL_NUMBER);
        parser.AddOption("", "log", "write sync measurements to this CSV file");
        parser.AddOption("", "client-id", "use this identity instead of the saved one (testing); settings aren't saved");
        parser.AddOption("", "clock-drift-ppm", "make the local clock drift (testing)", wxCMD_LINE_VAL_NUMBER);
    }

    bool OnCmdLineParsed(wxCmdLineParser& parser) override {
        // Saved settings first; command-line options override them.
        auto* config = wxConfig::Get();
        m_options.name = config->Read("name", "").utf8_string();
        m_options.volume = std::clamp(config->ReadLong("volume", 100), 0L, 100L);

        wxString s;
        if (parser.Found("relay", &s))
            m_options.relayUrl = s.utf8_string();
        if (parser.Found("room", &s))
            m_options.roomCode = s.utf8_string();
        m_options.createRoom = parser.Found("create");
        if (parser.Found("name", &s))
            m_options.name = s.utf8_string();
        if (parser.Found("log", &s))
            m_options.logPath = s.utf8_string();
        long volume = 0;
        if (parser.Found("volume", &volume))
            m_options.volume = static_cast<int>(std::clamp(volume, 0L, 100L));
        if (parser.Found("client-id", &s)) {
            m_options.clientId = s.utf8_string();
            m_options.persist = false; // a test instance; don't overwrite the user's settings
        }
        long ppm = 0;
        if (parser.Found("clock-drift-ppm", &ppm))
            ClockSync::s_debugDriftPpm = static_cast<double>(ppm);
        return wxApp::OnCmdLineParsed(parser);
    }

private:
    // The secret, persistent identity of this installation: generated on
    // first run and kept in the registry. Anyone holding it can act as this
    // member, so it's never shown in the UI or sent anywhere but the relay.
    void LoadIdentity() {
        if (!m_options.clientId.empty())
            return;
        auto* config = wxConfig::Get();
        wxString id;
        if (!config->Read("clientId", &id) || id.length() < 16) {
            std::random_device rd;
            for (int i = 0; i < 8; ++i)
                id += wxString::Format("%08x", rd());
            config->Write("clientId", id);
            config->Flush();
        }
        m_options.clientId = id.utf8_string();
    }

    LaunchOptions m_options;
};

wxIMPLEMENT_APP(App);
