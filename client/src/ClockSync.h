#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include <nlohmann/json_fwd.hpp>

// Estimates the offset between this machine's clock and the relay server's
// clock using Cristian's algorithm, so that every client can agree on "now"
// in server time.
//
//   1. Send {"type":"timeSync","clientSentAt": localNow}.
//   2. Server replies immediately with {"serverTime", "clientSentAt"}.
//   3. On receipt: rtt = localNow - clientSentAt
//                  offset = (serverTime + rtt/2) - localNow
//   4. Take several samples; keep the one with the smallest RTT (least
//      queueing delay, so the rtt/2 symmetry assumption is most accurate).
//   5. Re-sync periodically to correct drift.
//
// Local time comes from std::chrono::steady_clock, not the wall clock: it's
// monotonic, so the user (or NTP) adjusting the system clock mid-session
// can't make playback jump. The offset is therefore large (epoch vs. boot
// time), and that's fine since we only ever use localNow + offset.
//
// Server quirk worth knowing: inside a Cloudflare Worker / Durable Object,
// Date.now() only advances between I/O events (a Spectre mitigation). That's
// fine here because the server reads it once per incoming message, but it
// means serverTime is "when the message was delivered", not "when the
// reply was sent".
class ClockSync {
public:
    // Builds the timeSync intent to send. Stamps it with LocalNowMs().
    nlohmann::json MakeRequest() const;

    // Feed every server "timeSync" reply here.
    void OnResponse(const nlohmann::json& msg);

    // Forget everything (e.g. after reconnecting to a different session).
    void Reset();

    // True once at least one sample has been accepted.
    bool HasEstimate() const { return m_best.has_value(); }

    // serverTime - localTime, in ms. Only meaningful if HasEstimate().
    double OffsetMs() const { return m_best ? m_best->offsetMs : 0.0; }

    // RTT of the sample the offset came from; the offset's error is at most
    // half of this.
    double BestRttMs() const { return m_best ? m_best->rttMs : 0.0; }

    // Current time on the server's clock (ms since Unix epoch).
    double ServerNowMs() const { return LocalNowMs() + OffsetMs(); }

    // How many requests to fire for a sync round: more for the first one.
    int SamplesWanted() const { return HasEstimate() ? kResyncSamples : kInitialSamples; }

    // Whether it's time to start a fresh round of samples.
    bool NeedsResync() const;

    // Monotonic local clock, in ms.
    static double LocalNowMs();

    // Testing aid: make the local clock run fast (or slow) by this many
    // parts per million, to check that periodic resync keeps up (see
    // --clock-drift-ppm). A constant offset would be pointless to simulate:
    // steady_clock counts from boot, so the offset to server time is already
    // enormous. What genuinely differs between machines is the rate.
    static inline double s_debugDriftPpm = 0;

    static constexpr int kInitialSamples = 8;
    static constexpr int kResyncSamples = 3;
    static constexpr double kResyncIntervalMs = 30'000;
    // Samples older than this no longer compete for "best": local clocks
    // drift by tens of ppm (a few ms per minute), so an old low-RTT sample
    // slowly becomes wrong.
    static constexpr double kSampleMaxAgeMs = 120'000;
    // Anything slower than this is too noisy to be worth keeping.
    static constexpr double kMaxRttMs = 2'000;

private:
    struct Sample {
        double rttMs;
        double offsetMs;
        double takenAtLocalMs;
    };

    void PickBest();

    std::deque<Sample> m_samples;
    std::optional<Sample> m_best;
    double m_lastSampleLocalMs = 0.0;
};
