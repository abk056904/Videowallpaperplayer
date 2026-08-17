#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

#include "governor/PausePolicy.h"

namespace vw::playback {
class PlaybackController;
}

namespace vw::governor {

// Central state authority for decode/render (docs/02 §2.4 / docs/03 §3.12,
// M10). Subsystems never start/stop the decoder directly — they feed reasons
// here and the governor maps the PausePolicy transition onto the playback
// controller.
//
//   ACTIVE   -> PAUSED    : any required reason set (pause playback, keep
//                           position + decoder).
//   PAUSED   -> SUSPENDED : the pause outlives longPauseReleaseSeconds —
//                           the decoder is released (CPU/GPU near zero).
//   PAUSED/SUSPENDED -> ACTIVE : all reasons clear (recreate decoder, seek to
//                           the saved position, restart pacing).
//
// The long-pause clock is wall time (monotonic, injectable for tests). The
// governor is UI-thread only (same contract as the app's message loop).
class ResourceGovernor {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    using Action = std::function<void(State from, State to, uint32_t reasons)>;

    explicit ResourceGovernor(playback::PlaybackController& playback, PausePolicy::Config cfg);

    // Test hook: inject the clock used for the long-pause elapsed timer
    // (defaults to the real steady_clock).
    void setClockForTest(Clock c) { clock_ = std::move(c); }

    // Feeds one reason transition: `on` = reason appearing/clearing. Returns
    // the reasons mask after the change.
    uint32_t setReason(Reason reason, bool on);

    // Replaces the whole mask (e.g. battery re-read). Returns the new mask.
    uint32_t setReasons(uint32_t reasons);

    // The current authoritative mask.
    uint32_t reasons() const { return reasons_; }

    // Advances the state machine (long-pause -> SUSPENDED): call on the app's
    // low-frequency tick while PAUSED. Uses the injected clock (real by
    // default; tests substitute a controllable one).
    void onTick();

    State state() const { return state_; }

    // The reason set as a human-readable string (debug log).
    static std::wstring describeReasons(uint32_t reasons);

    // M11 live config: battery-pause policy and the long-pause release
    // threshold are read by onTick/transitionTo at use time, so changing the
    // policy config takes effect immediately (no recreation needed).
    void setBatteryPauses(bool pause) { policy_.config().batteryPauses = pause; }
    void setLongPauseReleaseSeconds(int seconds) {
        policy_.config().longPauseReleaseSeconds = seconds;
    }
    // Test/UI read access to the live policy config.
    const PausePolicy::Config& policyConfig() const { return policy_.config(); }

    // Test hook: action observer fired on every transition (assert the
    // transition table without a real playback session).
    void setActionObserver(Action a) { action_ = std::move(a); }

    // Called when SUSPENDED -> ACTIVE: the decoder was released by the
    // suspension, so playback cannot resume in place — the app reopens the
    // current playlist item (recreate decoder + seek to the saved position).
    void setResumeHandler(std::function<void()> h) { resumeHandler_ = std::move(h); }

private:
    void transitionTo(State next);
    uint32_t setReasonsWithTransition(uint32_t newMask);
    std::chrono::steady_clock::time_point now();

    playback::PlaybackController& playback_;
    PausePolicy policy_;
    State state_ = State::Active;
    uint32_t reasons_ = 0;
    std::chrono::steady_clock::time_point pausedSince_{};
    Action action_;
    std::function<void()> resumeHandler_;
    Clock clock_; // injected for tests; default = realNow
};

} // namespace vw::governor
