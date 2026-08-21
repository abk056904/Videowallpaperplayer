#include "performance/WorkloadMonitor.h"

#include <algorithm>

#include <windows.h>

#include <dxgi1_4.h> // IDXGIAdapter3 + QueryVideoMemoryInfo (this SDK split)
#include <wrl/client.h>

#include "logging/Logger.h"

namespace vw::performance {

namespace {

// Lazy singleton DXGI factory (created once; the adapter query needs a
// factory that supports IDXGIAdapter3). Guarded by the UI-thread-only
// sampling contract (WorkloadMonitor is used from the app's message loop).
IDXGIFactory3* gfxFactory() {
    static Microsoft::WRL::ComPtr<IDXGIFactory3> factory = [] {
        Microsoft::WRL::ComPtr<IDXGIFactory3> f;
        if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) {
            return Microsoft::WRL::ComPtr<IDXGIFactory3>{};
        }
        return f;
    }();
    return factory.Get();
}

// GPU VRAM: IDXGIAdapter3::QueryVideoMemoryInfo (docs/03 §3.11, M9). Uses the
// adapter that drives the primary output (index 0) — the same one the app's
// D3D11 device usually lands on. Failure (no DXGI, WDDM < 2.0) -> zeros.
void sampleGpuMemory(uint64_t& used, uint64_t& budget) {
    used = 0;
    budget = 0;
    auto* factory = gfxFactory();
    if (!factory) {
        return;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(factory->EnumAdapters(0, &adapter))) {
        return;
    }
    if (FAILED(adapter.As(&adapter3))) {
        return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        used = info.CurrentUsage;
        budget = info.Budget;
    }
}

} // namespace

WorkloadMonitor::WorkloadMonitor(Config cfg) : cfg_(std::move(cfg)),
                                               cpuEngine_({cfg_.cpuPause, cfg_.cpuResume,
                                                           cfg_.pauseDelay, cfg_.resumeDelay}),
                                               gpuEngine_({cfg_.gpuPause, cfg_.gpuResume,
                                                           cfg_.pauseDelay, cfg_.resumeDelay}),
                                               memEngine_({cfg_.memoryPause, cfg_.memoryResume,
                                                           cfg_.pauseDelay, cfg_.resumeDelay}) {}

void WorkloadMonitor::reconfigure(Config cfg) {
    cfg_ = std::move(cfg);
    cpuEngine_ = HysteresisEngine(
        {cfg_.cpuPause, cfg_.cpuResume, cfg_.pauseDelay, cfg_.resumeDelay});
    gpuEngine_ = HysteresisEngine(
        {cfg_.gpuPause, cfg_.gpuResume, cfg_.pauseDelay, cfg_.resumeDelay});
    memEngine_ = HysteresisEngine(
        {cfg_.memoryPause, cfg_.memoryResume, cfg_.pauseDelay, cfg_.resumeDelay});
}

double WorkloadMonitor::CpuSampler::cpuPercent(uint64_t idle, uint64_t kernel, uint64_t user,
                                               uint64_t prevIdle, uint64_t prevKernel,
                                               uint64_t prevUser, bool hasPrev) {
    if (!hasPrev) {
        return 0.0;
    }
    const uint64_t idleDelta = idle > prevIdle ? idle - prevIdle : 0;
    const uint64_t kernelDelta = kernel > prevKernel ? kernel - prevKernel : 0;
    const uint64_t userDelta = user > prevUser ? user - prevUser : 0;
    const uint64_t total = idleDelta + kernelDelta + userDelta;
    if (total == 0) {
        return 0.0;
    }
    // kernel includes idle on Windows; busy = total - idle.
    const double busy = static_cast<double>(total - idleDelta);
    return std::clamp(busy * 100.0 / static_cast<double>(total), 0.0, 100.0);
}

WorkloadState WorkloadMonitor::sample(std::chrono::steady_clock::time_point now) {
    WorkloadState s = state_; // carry gpuUsage (never sampled -> stays 0)

    // CPU: GetSystemTimes (kernel includes idle). First sample is a baseline.
    FILETIME idleFt{}, kernelFt{}, userFt{};
    if (::GetSystemTimes(&idleFt, &kernelFt, &userFt) != FALSE) {
        const auto to100ns = [](const FILETIME& f) -> uint64_t {
            return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        const uint64_t idle = to100ns(idleFt);
        const uint64_t kernel = to100ns(kernelFt);
        const uint64_t user = to100ns(userFt);
        s.cpuUsage = cpu_.cpuPercent(idle, kernel, user, cpu_.prevIdle, cpu_.prevKernel,
                                     cpu_.prevUser, cpu_.hasPrev);
        cpu_.prevIdle = idle;
        cpu_.prevKernel = kernel;
        cpu_.prevUser = user;
        cpu_.hasPrev = true;
    } else {
        s.cpuUsage = 0.0;
    }

    // RAM: GlobalMemoryStatusEx (% of physical in use + used bytes).
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (::GlobalMemoryStatusEx(&mem) != FALSE) {
        s.memoryUsage = static_cast<double>(mem.dwMemoryLoad);
        s.systemMemoryUsed = mem.ullTotalPhys - mem.ullAvailPhys;
    } else {
        s.memoryUsage = 0.0;
        s.systemMemoryUsed = 0;
    }

    // GPU: VRAM utilization (utilization counters unavailable in the SDK,
    // so we use VRAM pressure as a proxy: high VRAM usage = GPU-heavy work).
    sampleGpuMemory(s.gpuMemoryUsed, s.gpuMemoryBudget);
    if (s.gpuMemoryBudget > 0) {
        s.gpuUsage = std::clamp(
            static_cast<double>(s.gpuMemoryUsed) * 100.0 / static_cast<double>(s.gpuMemoryBudget),
            0.0, 100.0);
    }

    // Hysteresis: latch HIGH after the debounce delay, clear after the resume
    // delay. The engine uses monotonic time, so an irregular tick is fine.
    s.cpuHigh = cpuEngine_.update(s.cpuUsage, now);
    s.gpuHigh = gpuEngine_.update(s.gpuUsage, now);
    s.memoryHigh = memEngine_.update(s.memoryUsage, now);

    state_ = s;
    return s;
}

void WorkloadMonitor::pushToCollector(StatsCollector& collector, const WorkloadState& s) {
    // updateWorkload is the M9 extension point on StatsCollector (fills the
    // TelemetrySnapshot system fields). The collector holds the lock; this is
    // a plain copy-in.
    collector.updateWorkload(s.cpuUsage, s.gpuUsage, s.gpuMemoryUsed, s.gpuMemoryBudget,
                             s.systemMemoryUsed);
}

} // namespace vw::performance
