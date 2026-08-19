#pragma once

#include <cstdint>

namespace vw::playback {

using SchedulerTime = std::int64_t; // 100 ns units (media + wall clock)

// Source-FPS pacing math (docs/02 §2.5 / docs/03 §3.8, M6). A pure state
// machine over a QPC-derived 100 ns clock — no threads, no Win32 waits: the
// PlaybackController owns the waitable timer and arms it to the scheduler's
// deadlines; the message loop wakes on {timer, new-frame event} and calls the
// controller, which asks the scheduler for the due media time.
//
// Model: media time advances at speed × wall time from an anchor
// (anchorWall_, anchorMedia_). A frame with media timestamp T is due when
//   now >= anchorWall_ + (T - anchorMedia_) / speed
// i.e. dueMediaAt(now) = anchorMedia_ + speed × (now - anchorWall_). The consumer
// presents the newest frame with timestamp <= dueMediaAt(now) and drops stale
// ones. nextDeadline_ is only the ARMIN- target: after presenting, advance by
// one frame interval (snapping forward when behind so deadlines never linger
// in the past — no immediate re-fire busy loop).
//
// Units: 100 ns (10 000 000 per second) throughout, matching media timestamps.
class FrameScheduler {
public:
    // Source rate from video metadata (frames/second). fps <= 0 disables
    // pacing: every frame is due immediately (presentation becomes purely
    // new-frame-event driven).
    void setSourceFps(double fps);
    double sourceFps() const { return sourceFps_; }

    // Playback speed multiplier (1.0 = normal, 2.0 = double speed, etc.).
    void setSpeed(double speed);
    double speed() const { return speed_; }

    // One frame interval in 100 ns units; 0 when pacing is disabled.
    SchedulerTime interval100ns() const { return interval100ns_; }
    bool pacingEnabled() const { return interval100ns_ != 0; }

    // (Re)anchors the timeline: media time `mediaAnchor` is due at wall time
    // `now` (both 100 ns). Call on start/resume. The first frame (at the
    // anchor) is due immediately.
    void reset(SchedulerTime now, SchedulerTime mediaAnchor);

    // The media time that should be on screen at wall time `now`. Returns
    // INT64_MAX when pacing is disabled (everything is due).
    SchedulerTime dueMediaAt(SchedulerTime now) const;

    // Advances the arming deadline after the frame with media timestamp
    // `frameTimestamp` was presented at wall time `now`: the next frame is due
    // one interval later in media time (mapped through the anchor — exact even
    // when the frame was presented early or late), but never in the past
    // (catch-up snaps to now + interval when decode is slower than real time).
    void advanceAfterPresent(SchedulerTime now, SchedulerTime frameTimestamp);

    // Advances the arming deadline after a wake found nothing due: snap to
    // now + interval so the timer does not immediately re-fire on a past
    // deadline (frame is still decoding).
    void advanceIdle(SchedulerTime now);

    // Milliseconds until the next deadline from `now` (>= 1, for arming a
    // waitable timer; SetWaitableTimer needs a negative-relative due time).
    long long msUntilNextDeadline(SchedulerTime now) const;

private:
    double sourceFps_ = 0.0;
    double speed_ = 1.0;              // playback speed multiplier
    SchedulerTime interval100ns_ = 0; // 10'000'000 / fps
    SchedulerTime anchorWall_ = 0;
    SchedulerTime anchorMedia_ = 0;
    SchedulerTime nextDeadline_ = 0;
};

} // namespace vw::playback
