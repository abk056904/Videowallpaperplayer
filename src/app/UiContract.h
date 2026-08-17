#pragma once

// Shared UI contract types (docs/02 §2.x, spec §10.12–10.13, namespace vw::ui).
// This header is deliberately free of Windows.h and engine types so ANY module
// can include it — the UI reads engine state through these pure data types.
//
// This file currently defines only TelemetrySnapshot (the piece the M6/M9
// stats pipeline feeds). The rest of the contract — commands, the
// INotificationSink, PlaybackStateNotification, MonitorEvent, library/playlist
// notifications, and UiSnapshot — lands with the M11 UI milestone per the
// authoritative reference in spec §10.13.
//
// TelemetrySnapshot mirrors PerformanceStats (docs/01 §1.5.14) plus per-monitor
// detail. The workload fields (cpuUsage/gpuUsage/memory) are populated by
// WorkloadMonitor at M9; the playback fields are fed by StatsCollector from
// PlaybackController's per-second stats (M6 collection).

#include <cstdint>
#include <string>
#include <vector>

namespace vw::ui {

// Current engine telemetry, pushed ~1–2 Hz while the UI is open (and available
// for pull via the snapshot on window open). Zero/default values mean "not yet
// sampled" (e.g. workload fields before M9, or playback fields while stopped).
struct TelemetrySnapshot {
    // System workload (M9: WorkloadMonitor samples these ~1–2 s).
    double cpuUsage = 0.0;          // % overall
    double gpuUsage = 0.0;          // % (engine-utilization estimate; see R-03)
    uint64_t gpuMemoryUsed = 0;     // bytes (IDXGIAdapter3::QueryVideoMemoryInfo)
    uint64_t gpuMemoryBudget = 0;   // bytes
    uint64_t systemMemoryUsed = 0;  // bytes

    // Playback (M6 collection; StatsCollector aggregates from PlaybackStats).
    double decodedFps = 0.0;      // frames produced by the decode worker / s
    double presentedFps = 0.0;    // frames presented / s
    uint64_t droppedFrames = 0;   // stale frames dropped by the queue
    double decodeLatencyMs = 0.0; // avg decode -> present latency
    double renderTimeMs = 0.0;    // most recent render/present duration
    bool hardwareDecode = false;  // true when a hardware MFT is active

    // Per-monitor detail — only monitors with an active wallpaper session.
    struct PerMonitor {
        std::wstring monitorId;
        double presentedFps = 0.0;
        uint64_t droppedFrames = 0;
    };
    std::vector<PerMonitor> perMonitor;
};

} // namespace vw::ui
