#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "SessionState.h"

namespace ix {
class WebSocket;
}

// Builders for client -> server intents (see protocol/PROTOCOL.md).
namespace intent {
nlohmann::json Join(const std::string& name, const std::string& clientId);
nlohmann::json Leave();
// title/durationSec: optional hints (e.g. from a search result), so the
// queue can show them before anyone's player has loaded the video.
nlohmann::json Add(const std::string& videoId, const std::string& title = {}, double durationSec = 0);
nlohmann::json Play();
nlohmann::json Pause();
nlohmann::json Skip();
nlohmann::json Seek(double positionSec);
nlohmann::json Remove(int index);
nlohmann::json Move(int from, int to);
nlohmann::json Ended(int index);
nlohmann::json Unplayable(int index, const std::string& videoId, int code);
nlohmann::json Meta(int index, const std::string& videoId, const std::string& title, double durationSec);
} // namespace intent

// One WebSocket to the session's relay (a Durable Object), plus parsing of
// server messages into typed callbacks.
//
// THREADING: IXWebSocket delivers messages on its own background thread.
// Handlers are passed through `post` (set in the constructor), which must
// hop them onto the GUI thread; MainFrame passes a wxTheApp->CallAfter
// wrapper. Never touch wx controls from a raw IXWebSocket callback.
class SessionConnection {
public:
    enum class Status { Disconnected, Connecting, Connected };

    using Post = std::function<void(std::function<void()>)>;

    explicit SessionConnection(Post post);
    ~SessionConnection();

    SessionConnection(const SessionConnection&) = delete;
    SessionConnection& operator=(const SessionConnection&) = delete;

    // relayUrl e.g. "ws://localhost:8787" (wrangler dev) or
    // "wss://music-player-relay.<you>.workers.dev". Connects to
    // <relayUrl>/session/<sessionId>.
    // clientId: this installation's secret, persistent identity. The server
    // maps it to the same member every time, so reconnects keep your place.
    void Connect(const std::string& relayUrl, const std::string& sessionId, const std::string& displayName,
                 const std::string& clientId);
    void Disconnect();
    // Tells the server we're leaving on purpose (so we're removed at once
    // rather than after the grace period), then disconnects.
    void Leave();

    // Fire-and-forget. The client does NOT apply intents locally; it waits
    // for the resulting "state" broadcast. Returns false if not connected.
    bool SendIntent(const nlohmann::json& intent);

    Status GetStatus() const { return m_status; }
    std::int64_t LastVersion() const { return m_lastVersion; }

    // Handlers (always invoked on the GUI thread, via Post).
    std::function<void(const SessionState&)> onState;
    std::function<void(const nlohmann::json&)> onTimeSync;
    std::function<void(const std::string& memberId)> onWelcome; // our public member ID
    std::function<void(const std::string&)> onError;
    std::function<void(Status)> onStatus;
    // The relay says this room code doesn't exist (never created, or
    // deleted after being empty for a while). The connection has stopped.
    std::function<void()> onRoomNotFound;

private:
    void HandleText(const std::string& text);   // GUI thread
    void HandleOpen();                          // GUI thread
    void HandleClose(int code);                 // GUI thread
    void HandleConnectError(int httpStatus, const std::string& why); // GUI thread
    void SetStatus(Status s);
    // m_post, guarded against our destruction and against stale connections.
    void Dispatch(int generation, std::function<void()> fn);

    Post m_post;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    std::unique_ptr<ix::WebSocket> m_ws;
    std::string m_displayName;
    std::string m_clientId;
    Status m_status = Status::Disconnected;
    bool m_wantConnected = false;
    int m_generation = 0; // bumped by Connect/Disconnect
    std::int64_t m_lastVersion = -1;
};
