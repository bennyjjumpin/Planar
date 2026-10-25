#pragma once

#include <optional>
#include <string>

#include "SessionState.h"

class ClockSync;

// What the local player should be doing right now, according to the server.
struct DesiredPlayback {
    int index = -1;          // queue index; -1 = nothing should be playing
    std::string videoId;     // empty = nothing should be playing
    double positionSec = 0;  // where the playhead should be at this instant
    bool playing = false;
    // If the server scheduled a start slightly in the future (the LEAD),
    // how long until it begins, in ms. 0 when already started or paused.
    double startsInMs = 0;
    // Position is past the track's known duration: the track is over and we
    // are waiting for the server to advance the queue.
    bool finished = false;
};

// Turns (authoritative state + synced clock) into what the player should do.
//
// It has no side effects and doesn't touch the player directly. MainFrame
// calls Compute() whenever a new state arrives and on every player status
// report, compares the result with what the YouTube player says, and issues
// corrective commands.
class PlaybackController {
public:
    explicit PlaybackController(const ClockSync& clock) : m_clock(clock) {}

    DesiredPlayback Compute(const SessionState& state) const;

    // Given the desired playback and the player's reported position, decide
    // whether a corrective seek is needed. Returns the seek target, if any.
    std::optional<double> CorrectionFor(const DesiredPlayback& desired, double reportedPositionSec) const;

    // How far the local player may drift before we seek, in seconds.
    // Seeking is audible (rebuffer, skip), so don't make this too small.
    // MainFrame compensates for command latency, so corrections land close
    // to the target and rarely repeat.
    static constexpr double kDriftToleranceSec = 0.12;

    // A smaller error that persists (same sign) for kSteadySamples status
    // polls is also corrected, once: steady offsets are typically left over
    // from a start or seek, and a single correction (with learned seek
    // latency) removes them. Momentary jitter never stays put that long.
    static constexpr double kSteadyToleranceSec = 0.06;
    static constexpr int kSteadySamples = 8; // 4 s at 500 ms polling

private:
    const ClockSync& m_clock;
};
