#include "PlaybackController.h"

#include <algorithm>
#include <cmath>

#include "ClockSync.h"

DesiredPlayback PlaybackController::Compute(const SessionState& state) const {
    DesiredPlayback out;
    const QueueEntry* entry = state.CurrentEntry();
    if (!entry)
        return out; // nothing queued / nothing selected

    out.index = state.currentIndex;
    out.videoId = entry->videoId;

    if (state.pausedAt) {
        // Paused: a fixed position, independent of the clock.
        out.positionSec = std::max(0.0, static_cast<double>(*state.pausedAt - state.startedAt) / 1000.0);
        out.playing = false;
    } else if (!m_clock.HasEstimate()) {
        // We can't know "now" in server time yet. Hold at the start; the
        // first sync round takes a few hundred ms at most.
        out.positionSec = 0;
        out.playing = false;
        return out;
    } else {
        const double positionMs = m_clock.ServerNowMs() - static_cast<double>(state.startedAt);
        if (positionMs < 0) {
            // The server scheduled the start in the future (LEAD): be ready
            // at 0 and start exactly on time.
            out.positionSec = 0;
            out.playing = false;
            out.startsInMs = -positionMs;
        } else {
            out.positionSec = positionMs / 1000.0;
            out.playing = true;
        }
    }

    // Past the end: hold at the end. Advancing the queue is the server's job
    // (triggered by an "ended" intent).
    if (entry->durationSec > 0 && out.positionSec >= entry->durationSec) {
        out.positionSec = entry->durationSec;
        out.finished = true;
    }
    return out;
}

std::optional<double> PlaybackController::CorrectionFor(const DesiredPlayback& desired,
                                                        double reportedPositionSec) const {
    if (desired.videoId.empty() || desired.finished)
        return std::nullopt;
    if (std::abs(reportedPositionSec - desired.positionSec) <= kDriftToleranceSec)
        return std::nullopt;
    return desired.positionSec;
}
