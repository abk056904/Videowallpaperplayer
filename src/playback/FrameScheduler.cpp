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

void FrameScheduler::reset(SchedulerTime now, SchedulerTime mediaAnchor) {
    anchorWall_ = now;
    anchorMedia_ = mediaAnchor;
    nextDeadline_ = now; // the anchor media time is due immediately
}

SchedulerTime FrameScheduler::dueMediaAt(SchedulerTime now) const {
    if (!pacingEnabled()) {
        return (std::numeric_limits<SchedulerTime>::max)();
    }
    // Media time advances 1:1 with wall time from the anchor.
    return anchorMedia_ + (now - anchorWall_);
}

void FrameScheduler::advanceAfterPresent(SchedulerTime now, SchedulerTime frameTimestamp) {
    if (!pacingEnabled()) {
        return;
    }
    // The frame after `frameTimestamp` is due one interval later in MEDIA time;
    // map that back to wall time through the anchor. Exact even when the frame
    // was presented early (ahead of its own deadline) or late.
    const SchedulerTime nextDue = anchorWall_ + (frameTimestamp + interval100ns_ - anchorMedia_);
    nextDeadline_ = (std::max)(nextDue, now + interval100ns_);
}

void FrameScheduler::advanceIdle(SchedulerTime now) {
    if (!pacingEnabled()) {
        return;
    }
    // The previous deadline passed with nothing to present: re-arm relative to
    // now so the timer never immediately re-fires on a stale past deadline.
    nextDeadline_ = now + interval100ns_;
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
