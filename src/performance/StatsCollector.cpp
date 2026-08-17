#include "performance/StatsCollector.h"

#include <algorithm>
#include <utility>

#include "logging/Logger.h"

namespace vw::performance {

void StatsCollector::updatePlayback(double decodedFps, double presentedFps,
                                    uint64_t droppedFrames, double decodeLatencyMs,
                                    double renderTimeMs, bool hardwareDecode) {
    {
        std::lock_guard lock(mu_);
        latest_.decodedFps = decodedFps;
        latest_.presentedFps = presentedFps;
        latest_.droppedFrames = droppedFrames;
        latest_.decodeLatencyMs = decodeLatencyMs;
        latest_.renderTimeMs = renderTimeMs;
        latest_.hardwareDecode = hardwareDecode;
    }
    // DEBUG telemetry line (~1 Hz while playing) — the same stream the M11 UI
    // will render; visible in Debug builds only.
    log::Logger::instance().debug(
        L"telemetry: decoded {:.1f} fps, presented {:.1f} fps, {} dropped, "
        L"decodeLatency {:.1f} ms, render {:.1f} ms, hardwareDecode={}",
        decodedFps, presentedFps, droppedFrames, decodeLatencyMs, renderTimeMs,
        hardwareDecode ? L"yes" : L"no");
}

void StatsCollector::updateWorkload(double cpuUsage, double gpuUsage, uint64_t gpuMemoryUsed,
                                    uint64_t gpuMemoryBudget, uint64_t systemMemoryUsed) {
    std::lock_guard lock(mu_);
    latest_.cpuUsage = cpuUsage;
    latest_.gpuUsage = gpuUsage;
    latest_.gpuMemoryUsed = gpuMemoryUsed;
    latest_.gpuMemoryBudget = gpuMemoryBudget;
    latest_.systemMemoryUsed = systemMemoryUsed;
}

void StatsCollector::updatePerMonitor(const std::wstring& monitorId, double presentedFps,
                                      uint64_t droppedFrames) {
    std::lock_guard lock(mu_);
    auto it = std::find_if(latest_.perMonitor.begin(), latest_.perMonitor.end(),
                           [&](const vw::ui::TelemetrySnapshot::PerMonitor& m) {
                               return m.monitorId == monitorId;
                           });
    if (it != latest_.perMonitor.end()) {
        it->presentedFps = presentedFps;
        it->droppedFrames = droppedFrames;
    } else {
        latest_.perMonitor.push_back(
            vw::ui::TelemetrySnapshot::PerMonitor{monitorId, presentedFps, droppedFrames});
    }
}

vw::ui::TelemetrySnapshot StatsCollector::snapshot() const {
    std::lock_guard lock(mu_);
    return latest_;
}

void StatsCollector::reset() {
    std::lock_guard lock(mu_);
    latest_ = {};
}

} // namespace vw::performance
