#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// Client-side mirror of the server's authoritative session state.
// Field names and JSON shape match protocol/PROTOCOL.md and server/src/protocol.ts.
//
// The client never edits this itself: it's replaced wholesale whenever a
// newer "state" snapshot arrives from the server.

struct Member {
    std::string id;   // public ID, derived by the server from our clientId
    std::string name; // display name from the "join" intent
    // Disconnected but within the grace period: server ms when they'll be
    // removed. They keep their place (and host role) if they come back.
    std::optional<std::int64_t> awayUntil;
    bool IsAway() const { return awayUntil.has_value(); }
};

struct QueueEntry {
    std::string videoId;
    std::string addedBy; // Member::id
    // Metadata. The server can't use the IFrame API, so clients report
    // title and duration ("meta" intent) once their player has loaded the
    // video; the server derives the thumbnail URL from the ID.
    std::string title;
    double durationSec = 0.0;
    std::string thumbnailUrl;
};

struct SessionState {
    std::string sessionId; // also the join code
    std::vector<Member> members;
    std::vector<QueueEntry> queue;
    int currentIndex = -1; // index into queue; -1 = nothing playing

    // Playback timing, in server time (ms since Unix epoch, i.e. Date.now()
    // on the server). Position is derived, never stored:
    //   playing: position = now - startedAt
    //   paused:  position = pausedAt - startedAt
    std::int64_t startedAt = 0;
    std::optional<std::int64_t> pausedAt; // nullopt = playing

    std::int64_t version = 0; // bumped by the server on every change

    // Permissions hook. TODO: decide the policy (host-only skip/remove? votes?)
    // and enforce it on the server. The client only uses this to grey out UI.
    std::optional<std::string> hostId; // Member::id of the host, if any

    const QueueEntry* CurrentEntry() const {
        if (currentIndex < 0 || currentIndex >= static_cast<int>(queue.size()))
            return nullptr;
        return &queue[currentIndex];
    }
    bool IsPaused() const { return pausedAt.has_value(); }
};

// nlohmann/json ADL hooks. from_json throws nlohmann::json::exception on a
// malformed snapshot; callers should catch that and keep the old state.
void to_json(nlohmann::json& j, const Member& m);
void from_json(const nlohmann::json& j, Member& m);
void to_json(nlohmann::json& j, const QueueEntry& e);
void from_json(const nlohmann::json& j, QueueEntry& e);
void to_json(nlohmann::json& j, const SessionState& s);
void from_json(const nlohmann::json& j, SessionState& s);
