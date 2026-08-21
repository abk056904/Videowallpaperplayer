#include "playback/FrameScheduler.h"

#include <algorithm>
#include <limits>

namespace vw::playback {

namespace {
constexpr SchedulerTime k100nsPerSecond = 10'000'000LL;
} // namespace

void FrameScheduler::setSourceFps(double fps) {
    sourceFps_ = fps;
    interval100ns_ = (fps > 0.0) ? static_cast<SchedulerTime>(k100nsPerSecond / fps) : 0;
}

void FrameScheduler::setSpeed(double speed) {
    speed_ = (speed > 0.0) ? speed : 1.0;
}

void FrameScheduler::reset(SchedulerTime now, SchedulerTime mediaAnchor) {
    anchorWall_ = now;
    anchorMedia_ = mediaAnchor;
    nextDeadline_ = now; // the anchor media time is due immediately
}

SchedulerTime FrameScheduler::dueMediaAt(SchedulerTime now) const {
    if (!pacingEnabled()) {
        return (std::numeric_limits<SchedulerTime>::max)();
    }
    // Media time advances at speed × wall time from the anchor.
    // speed=1: 1:1 (real-time); speed=2: media runs 2× faster.
    return anchorMedia_ + static_cast<SchedulerTime>((now - anchorWall_) * speed_);
}

void FrameScheduler::advanceAfterPresent(SchedulerTime now, SchedulerTime frameTimestamp) {
    if (!pacingEnabled()) {
        return;
    }
    // The frame after `frameTimestamp` is due one interval later in MEDIA time;
    // map that back to wall time through the anchor and speed. Exact even when
    // the frame was presented early (ahead of its own deadline) or late.
    const SchedulerTime nextMedia = frameTimestamp + interval100ns_;
    const SchedulerTime nextDue = anchorWall_ + static_cast<SchedulerTime>((nextMedia - anchorMedia_) / speed_);
    const SchedulerTime wallInterval = static_cast<SchedulerTime>(interval100ns_ / speed_);
    if (nextDue <= now) {
        // We're behind (next frame should already be on screen). Allow
        // catch-up by arming immediately instead of waiting a full interval.
        // This prevents the scheduler from drifting permanently behind after
        // a momentary decode stall (disk I/O, OS scheduling jitter) — the
        // consumer drains the queued frames as fast as vsync allows until
        // real-time catches up.
        nextDeadline_ = now + 1; // 1 = minimum for SetWaitableTimer
    } else {
        nextDeadline_ = (std::max)(nextDue, now + wallInterval);
    }
}

void FrameScheduler::advanceIdle(SchedulerTime now) {
    if (!pacingEnabled()) {
        return;
    }
    // The previous deadline passed with nothing to present (decode still
    // working). Re-arm with a short delay so the timer re-checks quickly
    // when the decoder produces a frame — no long wait that would cause
    // an avoidable frame skip.
    nextDeadline_ = now + std::min<SchedulerTime>(
        static_cast<SchedulerTime>(interval100ns_ / speed_),
        50000); // cap at 5 ms — fast retry when decode is catching up
}

long long FrameScheduler::msUntilNextDeadline(SchedulerTime now) const {
    if (!pacingEnabled()) {
        return 1;
    }
    const SchedulerTime remaining = nextDeadline_ - now;
    if (remaining <= 0) {
        return 1; // due now; >= 1 ms so the timer can fire and be re-armed
    }
    // 100 ns -> ms, rounding up so we never wake early.
    const long long ms = static_cast<long long>((remaining + 9999) / 10000);
    return ms < 1 ? 1 : ms;
}

} // namespace vw::playback
