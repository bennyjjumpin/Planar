#include "ClockSync.h"

#include <chrono>

#include <nlohmann/json.hpp>

double ClockSync::LocalNowMs() {
    using namespace std::chrono;
    const double raw = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    if (s_debugDriftPpm == 0)
        return raw;
    static const double origin = raw;
    return raw + (raw - origin) * s_debugDriftPpm / 1e6;
}

nlohmann::json ClockSync::MakeRequest() const {
    return {{"type", "timeSync"}, {"clientSentAt", LocalNowMs()}};
}

void ClockSync::OnResponse(const nlohmann::json& msg) {
    const double receivedAt = LocalNowMs();
    if (!msg.contains("serverTime") || !msg["serverTime"].is_number() ||
        !msg.contains("clientSentAt") || !msg["clientSentAt"].is_number())
        return;

    // clientSentAt is echoed back unchanged, so no need to track requests.
    const double serverTime = msg["serverTime"].get<double>();
    const double sentAt = msg["clientSentAt"].get<double>();
    const double rtt = receivedAt - sentAt;
    if (rtt < 0 || rtt > kMaxRttMs)
        return; // reply to a request from before a Reset(), or hopelessly slow

    // The server stamped serverTime roughly halfway through the round trip.
    const double offset = (serverTime + rtt / 2) - receivedAt;

    m_samples.push_back({rtt, offset, receivedAt});
    m_lastSampleLocalMs = receivedAt;
    PickBest();
}

void ClockSync::PickBest() {
    const double now = LocalNowMs();
    while (!m_samples.empty() && now - m_samples.front().takenAtLocalMs > kSampleMaxAgeMs)
        m_samples.pop_front();

    // If every sample aged out, keep the previous best until new ones arrive.
    if (m_samples.empty())
        return;

    const Sample* best = &m_samples.front();
    for (const auto& s : m_samples)
        if (s.rttMs < best->rttMs)
            best = &s;

    // The new offset is applied immediately. Successive estimates usually
    // differ by a millisecond or two, well inside the drift tolerance, so
    // slewing isn't worth the complexity yet.
    m_best = *best;
}

void ClockSync::Reset() {
    m_samples.clear();
    m_best.reset();
    m_lastSampleLocalMs = 0.0;
}

bool ClockSync::NeedsResync() const {
    return !HasEstimate() || LocalNowMs() - m_lastSampleLocalMs > kResyncIntervalMs;
}
