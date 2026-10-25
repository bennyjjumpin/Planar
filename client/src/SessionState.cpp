#include "SessionState.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

void to_json(json& j, const Member& m) {
    j = json{{"id", m.id}, {"name", m.name}, {"awayUntil", m.awayUntil ? json(*m.awayUntil) : json(nullptr)}};
}

void from_json(const json& j, Member& m) {
    j.at("id").get_to(m.id);
    m.name = j.value("name", "");
    const auto& away = j.contains("awayUntil") ? j["awayUntil"] : json(nullptr);
    m.awayUntil = away.is_number() ? std::optional(away.get<std::int64_t>()) : std::nullopt;
}

void to_json(json& j, const QueueEntry& e) {
    j = json{{"videoId", e.videoId},
             {"addedBy", e.addedBy},
             {"title", e.title},
             {"durationSec", e.durationSec},
             {"thumbnailUrl", e.thumbnailUrl}};
}

void from_json(const json& j, QueueEntry& e) {
    j.at("videoId").get_to(e.videoId);
    e.addedBy = j.value("addedBy", "");
    // Metadata may be missing or null until someone fills it in.
    e.title = j.contains("title") && j["title"].is_string() ? j["title"].get<std::string>() : "";
    e.durationSec = j.contains("durationSec") && j["durationSec"].is_number() ? j["durationSec"].get<double>() : 0.0;
    e.thumbnailUrl = j.contains("thumbnailUrl") && j["thumbnailUrl"].is_string() ? j["thumbnailUrl"].get<std::string>() : "";
}

void to_json(json& j, const SessionState& s) {
    j = json{{"sessionId", s.sessionId},
             {"members", s.members},
             {"queue", s.queue},
             {"currentIndex", s.currentIndex},
             {"startedAt", s.startedAt},
             {"pausedAt", s.pausedAt ? json(*s.pausedAt) : json(nullptr)},
             {"version", s.version},
             {"hostId", s.hostId ? json(*s.hostId) : json(nullptr)}};
}

void from_json(const json& j, SessionState& s) {
    j.at("sessionId").get_to(s.sessionId);
    s.members = j.value("members", std::vector<Member>{});
    s.queue = j.value("queue", std::vector<QueueEntry>{});
    s.currentIndex = j.value("currentIndex", -1);
    s.startedAt = j.value("startedAt", std::int64_t{0});

    const auto& pausedAt = j.contains("pausedAt") ? j["pausedAt"] : json(nullptr);
    s.pausedAt = pausedAt.is_null() ? std::nullopt : std::optional(pausedAt.get<std::int64_t>());

    j.at("version").get_to(s.version);

    const auto& hostId = j.contains("hostId") ? j["hostId"] : json(nullptr);
    s.hostId = hostId.is_null() ? std::nullopt : std::optional(hostId.get<std::string>());
}
