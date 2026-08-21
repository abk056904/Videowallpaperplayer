#pragma once

#include <chrono>
#include <cstdint>

#include "performance/HysteresisEngine.h"
#include "performance/StatsCollector.h"

namespace vw::performance {

// One sampled snapshot of the machine's workload (docs/03 §3.11, M9).
struct WorkloadState {
    double cpuUsage = 0.0;          // % overall (GetSystemTimes delta)
    double gpuUsage = 0.0;          // % — VRAM pressure (gpuMemoryUsed/budget)
                                    // when available; 0 if no DXGI adapter
    double memoryUsage = 0.0;       // % of committed physical (RAM)
    uint64_t gpuMemoryUsed = 0;     // bytes (IDXGIAdapter3::QueryVideoMemoryInfo)
    uint64_t gpuMemoryBudget = 0;   // bytes
    uint64_t systemMemoryUsed = 0;  // bytes

    bool cpuHigh = false;   // latched by the hysteresis engine
    bool gpuHigh = false;
    bool memoryHigh = false;
    bool anyHigh() const { return cpuHigh || gpuHigh || memoryHigh; }
};

// Low-frequency CPU/GPU/RAM sampling with hysteresis + debounce (docs/02
// §2.9 / docs/03 §3.11, M9). The app ticks it every ~2 s while running (and
// skips it entirely in GAME/FULLSCREEN/LOCKED/DISPLAY_OFF unless the UI is
// open — cost control). Results flow into StatsCollector (UI telemetry) and
// the latched HIGH flags are what M10's ResourceGovernor consumes.
//
// GPU utilization: per-engine counters are unavailable in this SDK, so
// gpuUsage is derived from VRAM pressure (gpuMemoryUsed / gpuMemoryBudget)
// via IDXGIAdapter3::QueryVideoMemoryInfo. High VRAM usage is a strong proxy
// for GPU-heavy workloads. When no DXGI adapter is present, gpuUsage stays 0.
//
// The hysteresis transition table is unit-tested via HysteresisEngine (pure);
// the CPU/RAM samplers are thin Win32 calls over injectable primitives.
class WorkloadMonitor {
public:
    struct Config {
        double cpuPause = 85.0, cpuResume = 65.0;
        double gpuPause = 90.0, gpuResume = 70.0;
        double memoryPause = 90.0, memoryResume = 75.0;
        std::chrono::milliseconds pauseDelay{3000};
        std::chrono::milliseconds resumeDelay{5000};
    };

    explicit WorkloadMonitor(Config cfg);

    // Advances the samplers + hysteresis engines. `now` monotonic; must be
    // called with roughly the configured interval (the delay logic uses wall
    // time, so an irregular tick is fine). Returns the aggregated state.
    WorkloadState sample(std::chrono::steady_clock::time_point now);

    // Pushes the sampled workload into the collector (UI telemetry). The app
    // calls this on its telemetry tick; sample() itself never touches the
    // collector (testability — no hidden state).
    static void pushToCollector(StatsCollector& collector, const WorkloadState& s);

    const WorkloadState& state() const { return state_; }

    // Config reload (thresholds/delays changed).
    void reconfigure(Config cfg);

    // Pure CPU-delta helper (exposed for unit tests — no Windows state).
    struct CpuSampler {
        uint64_t prevIdle = 0, prevKernel = 0, prevUser = 0;
        bool hasPrev = false;
        // Delta over the idle/kernel/user counters (GetSystemTimes, 100 ns
        // units). Returns the % busy since the previous sample (0..100).
        static double cpuPercent(uint64_t idle, uint64_t kernel, uint64_t user,
                                 uint64_t prevIdle, uint64_t prevKernel, uint64_t prevUser,
                                 bool hasPrev);
    };

private:
    Config cfg_;
    CpuSampler cpu_;
    HysteresisEngine cpuEngine_, gpuEngine_, memEngine_;
    WorkloadState state_;
};

} // namespace vw::performance
