#pragma once

#include <algorithm>
#include <chrono>

// Pure hysteresis + debounce state machine (docs/02 §2.9 / docs/03 §3.11, M9).
// Each metric (CPU/GPU/memory) has a PAUSE threshold and a RESUME threshold.
// A condition latches HIGH only after the metric has stayed >= pauseThreshold
// for pauseDelay (debounce — no pause on a 1-sample spike); it clears only
// after the metric has stayed < resumeThreshold for resumeDelay (hysteresis —
// no resume/pause flapping at the boundary).
//
// Deliberately free of any Windows/COM state so the transition table is
// unit-testable (docs/04 §2: fires after ~3 s, clears after ~5 s, no
// oscillation). The caller (WorkloadMonitor) advances it with real timestamps
// (steady_clock) and samples.
//
// The resume threshold defaults to the pause threshold when not supplied
// (single-threshold mode) — but the config always supplies both.

namespace vw::performance {

enum class LoadMetric { Cpu, Gpu, Memory };

// One metric's latch state: idle until the pause delay elapses, then HIGH
// until the metric drops below the resume threshold for the resume delay.
class HysteresisEngine {
public:
    struct Config {
        double pauseThreshold = 90.0;  // % — latch HIGH at/above this
        double resumeThreshold = 70.0; // % — clear below this
        std::chrono::milliseconds pauseDelay{3000};
        std::chrono::milliseconds resumeDelay{5000};
    };

    explicit HysteresisEngine(Config cfg) : cfg_(std::move(cfg)) {
        if (cfg_.resumeThreshold <= 0.0) {
            cfg_.resumeThreshold = cfg_.pauseThreshold;
        }
    }

    // Feeds one sample at time `now` (monotonic). Returns the latched state.
    // `now` MUST be monotonic non-decreasing; the caller owns the clock.
    bool update(double value, std::chrono::steady_clock::time_point now) {
        const bool over = value >= cfg_.pauseThreshold;
        const bool under = value < cfg_.resumeThreshold;

        if (high_) {
            if (under) {
                if (edgeSince_ == std::chrono::steady_clock::time_point{}) {
                    edgeSince_ = now;
                } else if (now - edgeSince_ >= cfg_.resumeDelay) {
                    high_ = false;
                    edgeSince_ = {};
                }
            } else {
                edgeSince_ = {}; // back over the resume threshold — restart the clear timer
            }
            return high_;
        }

        // idle: latch when the metric stays over the pause threshold.
        if (over) {
            if (edgeSince_ == std::chrono::steady_clock::time_point{}) {
                edgeSince_ = now;
            } else if (now - edgeSince_ >= cfg_.pauseDelay) {
                high_ = true;
                edgeSince_ = {};
            }
        } else {
            edgeSince_ = {}; // dipped below — cancel any pending latch
        }
        return high_;
    }

    bool high() const { return high_; }
    const Config& config() const { return cfg_; }

    // Reset (config reload / playback session start).
    void reset() {
        high_ = false;
        edgeSince_ = {};
    }

private:
    Config cfg_;
    bool high_ = false;
    std::chrono::steady_clock::time_point edgeSince_{};
};

} // namespace vw::performance
