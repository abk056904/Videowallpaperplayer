#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "app/UiContract.h"

namespace vw::performance {

// Aggregates engine telemetry into the UI snapshot type (docs/02 §2.x, spec
// §10.12). The UI (M11) subscribes to this snapshot at ~1–2 Hz; until then the
// collector holds the latest values for pull/reading.
//
// Fed by: PlaybackController's per-second stats (M6 collection, ~1 Hz) and the
// wallpaper layer's per-monitor counters. The system-level fields
// (cpuUsage/gpuUsage/memory) are filled by WorkloadMonitor at M9 — they stay 0
// until then ("not yet sampled").
//
// Thread-safe: the message loop updates on the UI thread and M11's telemetry
// timer reads on the same thread, but the lock keeps any reader safe (e.g.
// future render/worker-thread consumers). snapshot() returns a consistent copy.
class StatsCollector {
public:
    // Playback session stats, fed from PlaybackController's per-second
    // recompute (~1 Hz while Playing; values freeze while paused/stopped).
    void updatePlayback(double decodedFps, double presentedFps, uint64_t droppedFrames,
                        double decodeLatencyMs, double renderTimeMs, bool hardwareDecode);

    // Per-monitor playback detail (replace-or-append keyed by monitorId).
    // Only monitors with an active wallpaper session should be reported.
    void updatePerMonitor(const std::wstring& monitorId, double presentedFps,
                          uint64_t droppedFrames);

    // M9: WorkloadMonitor fills the system-level fields (cpuUsage, gpuUsage,
    // gpuMemoryUsed/Budget, systemMemoryUsed). Never samples here — the
    // monitor owns the sampling loop; this is a plain copy-in.
    void updateWorkload(double cpuUsage, double gpuUsage, uint64_t gpuMemoryUsed,
                        uint64_t gpuMemoryBudget, uint64_t systemMemoryUsed);

    // Latest consistent snapshot (copy under the lock).
    vw::ui::TelemetrySnapshot snapshot() const;

    // Clears all fields (new playback session / UI reopen).
    void reset();

private:
    mutable std::mutex mu_;
    vw::ui::TelemetrySnapshot latest_;
};

} // namespace vw::performance
