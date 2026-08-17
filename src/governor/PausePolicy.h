#pragma once

#include <cstdint>
#include <utility>

// Pure resource-state policy (docs/02 §2.4 / docs/03 §3.12, M10). Deliberately
// free of any Windows/COM/playback state so the transition table (docs/03 §95)
// is unit-testable: the caller (ResourceGovernor) feeds it the active reasons
// and elapsed-pause time, and it answers the three transition questions.
//
// States: ACTIVE -> PAUSED -> SUSPENDED (TERMINATED is the app's exit path).
//   ACTIVE:   normal playback.
//   PAUSED:   playback frozen (position kept) — decoder kept for a quick
//             resume, or released after longPauseReleaseSeconds (SUSPENDED).
//   SUSPENDED: decoder + temp GPU resources released; keep position/path/
//             config. Resume recreates the decoder (seeks to the position).
//
// Pause-reason logic (docs/02 §2.4):
//   - Any REQUIRED reason set  => PAUSED.
//   - Resume only when ALL reasons clear.
//   - Workload reasons (HighCPU/HighGPU/HighMemory) are DEBOUNCED by the
//     hysteresis engine upstream (M9) — they only reach the policy once the
//     metric has been high for the pause delay, so the policy treats them as
//     immediate. Immediate reasons (User/Game/Fullscreen/Locked/DisplayOff/
//     SystemSuspended/Battery) also arrive instantly.
//   - Battery mode Pause is a required reason; Battery Continue/ReduceQuality
//     do NOT pause (reduce-quality is an M13 optimization — documented).

namespace vw::governor {

enum class State { Active, Paused, Suspended };

// Pause-reason bitmask (docs/03 §3.12). One bit per reason; the policy is
// reason-agnostic — any set bit pauses, zero bits resumes.
enum Reason : uint32_t {
    User = 1u << 0,          // explicit user pause (tray/UI)
    Game = 1u << 1,          // foreground game (allow-list)
    Fullscreen = 1u << 2,    // foreground fullscreen window
    HighCPU = 1u << 3,       // hysteresis-latched CPU (M9)
    HighGPU = 1u << 4,       // hysteresis-latched GPU
    HighMemory = 1u << 5,    // hysteresis-latched RAM
    Battery = 1u << 6,       // battery mode = Pause
    Locked = 1u << 7,        // session locked
    DisplayOff = 1u << 8,    // monitor off (GUID_MONITOR_POWER_ON)
    SystemSuspended = 1u << 9, // system suspend/hibernate
    MonitorHidden = 1u << 10,  // wallpaper hidden (future; reserved)
};
constexpr uint32_t kAllReasons = 0x7FFu; // bits 0..10

inline const char* reasonName(uint32_t bit) {
    switch (bit) {
        case Reason::User: return "user";
        case Reason::Game: return "game";
        case Reason::Fullscreen: return "fullscreen";
        case Reason::HighCPU: return "high-cpu";
        case Reason::HighGPU: return "high-gpu";
        case Reason::HighMemory: return "high-memory";
        case Reason::Battery: return "battery";
        case Reason::Locked: return "locked";
        case Reason::DisplayOff: return "display-off";
        case Reason::SystemSuspended: return "system-suspended";
        case Reason::MonitorHidden: return "monitor-hidden";
    }
    return "?";
}

class PausePolicy {
public:
    struct Config {
        // A long PAUSED stretch (>= this) releases the decoder -> SUSPENDED.
        int longPauseReleaseSeconds = 5;
        // Battery mode: Pause = Battery is a required reason; Continue =
        // battery never pauses (ReduceQuality is an M13 perf-mode decision).
        bool batteryPauses = true;
    };

    explicit PausePolicy(Config cfg) : cfg_(std::move(cfg)) {}

    // Transition decision for the CURRENT state given the active reasons and
    // whether the long-pause release time has elapsed. Pure: no side effects,
    // no timers — the ResourceGovernor feeds its mask + elapsed-pause clock
    // here and maps the returned state onto concrete playback actions.
    State nextState(State current, uint32_t reasons, bool longPauseElapsed) const {
        const bool paused = reasons != 0;
        switch (current) {
            case State::Active:
                return paused ? State::Paused : State::Active;
            case State::Paused:
                if (!paused) {
                    return State::Active;
                }
                return longPauseElapsed ? State::Suspended : State::Paused;
            case State::Suspended:
                return paused ? State::Suspended : State::Active;
        }
        return State::Active;
    }

    const Config& config() const { return cfg_; }

private:
    Config cfg_;
};

} // namespace vw::governor
