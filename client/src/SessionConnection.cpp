#include "SessionConnection.h"

#include <ixwebsocket/IXWebSocket.h>

using nlohmann::json;

namespace intent {
json Join(const std::string& name, const std::string& clientId) {
    return {{"type", "join"}, {"name", name}, {"clientId", clientId}};
}
json Leave() { return {{"type", "leave"}}; }
json Add(const std::string& videoId, const std::string& title, double durationSec) {
    json j = {{"type", "add"}, {"videoId", videoId}};
    if (!title.empty())
        j["title"] = title;
    if (durationSec > 0)
        j["durationSec"] = durationSec;
    return j;
}
json Play() { return {{"type", "play"}}; }
json Pause() { return {{"type", "pause"}}; }
json Skip() { return {{"type", "skip"}}; }
json Seek(double positionSec) { return {{"type", "seek"}, {"positionSec", positionSec}}; }
json Remove(int index) { return {{"type", "remove"}, {"index", index}}; }
json Move(int from, int to) { return {{"type", "move"}, {"from", from}, {"to", to}}; }
json Ended(int index) { return {{"type", "ended"}, {"index", index}}; }
json Unplayable(int index, const std::string& videoId, int code) {
    return {{"type", "unplayable"}, {"index", index}, {"videoId", videoId}, {"code", code}};
}
json Meta(int index, const std::string& videoId, const std::string& title, double durationSec) {
    return {{"type", "meta"}, {"index", index}, {"videoId", videoId}, {"title", title}, {"durationSec", durationSec}};
}
} // namespace intent

SessionConnection::SessionConnection(Post post)
    : m_post(std::move(post)), m_ws(std::make_unique<ix::WebSocket>()) {
    // Reconnect policy: IXWebSocket retries automatically with exponential
    // backoff. Start at 1 s and cap at 30 s, so a relay that is down (or
    // over its free-plan limits, which show up as errors, not bills) isn't
    // hammered by every client at once.
    m_ws->setMinWaitBetweenReconnectionRetries(1'000);
    m_ws->setMaxWaitBetweenReconnectionRetries(30'000);

    // Heartbeats: Durable Objects hibernate idle sockets, and any message
    // wakes the object (billable duration). The periodic timeSync traffic
    // already keeps the connection alive, so pings can be rare.
    m_ws->setPingInterval(45);
}

SessionConnection::~SessionConnection() {
    // stop() joins IXWebSocket's thread. Clear the callback first so nothing
    // gets posted to a SessionConnection that's being destroyed.
    m_ws->setOnMessageCallback(nullptr);
    m_ws->stop();
}

void SessionConnection::Connect(const std::string& relayUrl, const std::string& sessionId,
                                const std::string& displayName, const std::string& clientId) {
    m_ws->stop(); // no-op if not running
    m_displayName = displayName;
    m_clientId = clientId;
    m_lastVersion = -1;
    m_wantConnected = true;
    const int generation = ++m_generation;

    m_ws->setUrl(relayUrl + "/session/" + sessionId);

    // Runs on IXWebSocket's thread: copy what we need and post to the GUI
    // thread, tagged with the generation so that messages from a previous
    // connection still sitting in the event queue are dropped.
    m_ws->setOnMessageCallback([this, generation](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            Dispatch(generation, [this] { HandleOpen(); });
            break;
        case ix::WebSocketMessageType::Close:
            Dispatch(generation, [this, code = msg->closeInfo.code] { HandleClose(code); });
            break;
        case ix::WebSocketMessageType::Error:
            Dispatch(generation, [this, status = msg->errorInfo.http_status, why = msg->errorInfo.reason] {
                HandleConnectError(status, why);
            });
            break;
        case ix::WebSocketMessageType::Message:
            Dispatch(generation, [this, text = msg->str] { HandleText(text); });
            break;
        default:
            break; // Ping / Pong / Fragment
        }
    });

    SetStatus(Status::Connecting);
    m_ws->start();
}

void SessionConnection::Disconnect() {
    m_wantConnected = false;
    ++m_generation; // drop anything still queued from this connection
    m_ws->stop();
    SetStatus(Status::Disconnected);
}

void SessionConnection::Leave() {
    // sendText only queues; IXWebSocket flushes it before the close frame
    // that stop() sends.
    SendIntent(intent::Leave());
    Disconnect();
}

void SessionConnection::HandleConnectError(int httpStatus, const std::string& why) {
    if (httpStatus == 404) {
        // Retrying can't help: the room doesn't exist.
        Disconnect();
        if (onRoomNotFound)
            onRoomNotFound();
        return;
    }
    HandleClose(0);
    if (onError)
        onError(why);
}

bool SessionConnection::SendIntent(const json& intent) {
    // Not queued while offline: intents are about "now", and replaying a
    // stale "pause" after a reconnect would surprise everyone.
    if (m_status != Status::Connected)
        return false;
    return m_ws->sendText(intent.dump()).success;
}

void SessionConnection::HandleOpen() {
    // A new socket means a new member ID on the server and possibly a
    // restarted session (versions start over), so accept the next snapshot
    // whatever its version.
    m_lastVersion = -1;
    SetStatus(Status::Connected);
    // The Durable Object knows nothing about this socket: introduce ourselves.
    SendIntent(intent::Join(m_displayName, m_clientId));
}

void SessionConnection::HandleClose(int code) {
    // 4000: the server says our identity connected again elsewhere (another
    // copy of the app). Reconnecting would just kick that one off in turn.
    constexpr int kCloseReplaced = 4000;
    if (code == kCloseReplaced && m_wantConnected) {
        Disconnect();
        if (onError)
            onError("this identity connected from somewhere else, so this window left the session");
        return;
    }
    // IXWebSocket keeps retrying in the background unless we stopped it.
    SetStatus(m_wantConnected ? Status::Connecting : Status::Disconnected);
}

void SessionConnection::HandleText(const std::string& text) {
    const auto msg = json::parse(text, nullptr, false);
    if (msg.is_discarded() || !msg.is_object())
        return;
    const std::string type = msg.value("type", "");

    if (type == "state") {
        // Stale or duplicate broadcast. Within one connection the server's
        // version is strictly increasing (it's persisted with the state).
        if (msg.value("version", std::int64_t{-1}) <= m_lastVersion)
            return;
        try {
            SessionState state = msg.at("state").get<SessionState>();
            m_lastVersion = state.version;
            if (onState)
                onState(state);
        } catch (const json::exception& e) {
            if (onError)
                onError(std::string("bad state message: ") + e.what());
        }
    } else if (type == "welcome") {
        if (onWelcome)
            onWelcome(msg.value("memberId", ""));
    } else if (type == "timeSync") {
        if (onTimeSync)
            onTimeSync(msg);
    } else if (type == "error") {
        if (onError)
            onError(msg.value("message", "unknown server error"));
    }
    // Unknown types are ignored, which lets the server add messages first.
}

void SessionConnection::Dispatch(int generation, std::function<void()> fn) {
    // The posted call may run after we're destroyed (it sits in the GUI
    // event queue), so it only runs if m_alive still exists by then, and
    // only if no Connect/Disconnect happened in between.
    m_post([this, generation, alive = std::weak_ptr<int>(m_alive), fn = std::move(fn)] {
        if (alive.lock() && generation == m_generation)
            fn();
    });
}

void SessionConnection::SetStatus(Status s) {
    if (s == m_status)
        return;
    m_status = s;
    if (onStatus)
        onStatus(s);
}
