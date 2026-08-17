#include "monitors/MonitorManager.h"

#include <algorithm>
#include <map>
#include <utility>

#include "graphics/D3D11DeviceManager.h"

namespace vw::monitors {

namespace {

struct EnumCtx {
    std::vector<MonitorInfo>* out;
};

BOOL CALLBACK enumMonitorProc(HMONITOR handle, HDC, LPRECT, LPARAM lparam) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lparam);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(handle, &info)) {
        return TRUE; // skip this monitor, keep enumerating
    }
    MonitorInfo m;
    m.id = info.szDevice;
    m.handle = handle;
    m.bounds = info.rcMonitor;
    m.workArea = info.rcWork;
    m.width = static_cast<UINT>(info.rcMonitor.right - info.rcMonitor.left);
    m.height = static_cast<UINT>(info.rcMonitor.bottom - info.rcMonitor.top);
    m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    ctx->out->push_back(std::move(m));
    return TRUE;
}

// deviceName (e.g. L"\\\\.\\DISPLAY1") -> current-mode refresh rate, matched
// from DXGI output enumeration (docs/02 §2.7). Whole-Hz via OutputInfo.
std::map<std::wstring, std::pair<UINT, UINT>> refreshRatesByDevice() {
    std::map<std::wstring, std::pair<UINT, UINT>> rates;
    auto adapters = gfx::D3D11DeviceManager::enumerateAdapters();
    if (!adapters) {
        return rates;
    }
    for (UINT i = 0; i < adapters->size(); ++i) {
        auto adapter = gfx::D3D11DeviceManager::getAdapter(i);
        if (!adapter) {
            continue;
        }
        auto outputs = gfx::D3D11DeviceManager::enumerateOutputs(adapter->Get());
        if (!outputs) {
            continue;
        }
        for (const auto& o : *outputs) {
            if (o.refreshHz > 0) {
                rates[o.deviceName] = {static_cast<UINT>(o.refreshHz), 1};
            }
        }
    }
    return rates;
}

} // namespace

Result<std::vector<MonitorInfo>> MonitorManager::enumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    EnumCtx ctx{&monitors};
    if (!::EnumDisplayMonitors(nullptr, nullptr, enumMonitorProc, reinterpret_cast<LPARAM>(&ctx))) {
        return std::unexpected(L"EnumDisplayMonitors failed (error " +
                               std::to_wstring(::GetLastError()) + L")");
    }

    const auto rates = refreshRatesByDevice();
    for (auto& m : monitors) {
        const auto it = rates.find(m.id);
        if (it != rates.end()) {
            m.refreshRateNumerator = it->second.first;
            m.refreshRateDenominator = it->second.second;
        }
    }

    // Stable order so diffs are deterministic (matches ConfigManager style).
    std::sort(monitors.begin(), monitors.end(),
              [](const MonitorInfo& a, const MonitorInfo& b) { return a.id < b.id; });
    return monitors;
}

Result<std::vector<MonitorInfo>> MonitorManager::refresh() {
    auto current = enumerateMonitors();
    if (!current) {
        return current;
    }

    std::map<std::wstring, const MonitorInfo*> prev;
    for (const auto& m : last_) {
        prev[m.id] = &m;
    }

    for (const auto& m : *current) {
        const auto it = prev.find(m.id);
        if (it == prev.end()) {
            if (onAdded_) {
                onAdded_(m.id);
            }
        } else {
            const auto& old = *it->second;
            const bool moved = old.bounds.left != m.bounds.left ||
                               old.bounds.top != m.bounds.top ||
                               old.bounds.right != m.bounds.right ||
                               old.bounds.bottom != m.bounds.bottom;
            const bool resized = old.width != m.width || old.height != m.height;
            const bool refreshChanged =
                old.refreshRateNumerator != m.refreshRateNumerator ||
                old.refreshRateDenominator != m.refreshRateDenominator;
            // HMONITOR is deliberately excluded: it is not stable across
            // redetection and would fire spurious "changed" events.
            if (moved || resized || refreshChanged || old.primary != m.primary) {
                if (onChanged_) {
                    onChanged_(m.id);
                }
            }
        }
    }

    for (const auto& [id, old] : prev) {
        const bool stillHere =
            std::any_of(current->begin(), current->end(),
                        [&](const MonitorInfo& m) { return m.id == id; });
        if (!stillHere && onRemoved_) {
            onRemoved_(id);
        }
    }

    last_ = *current;
    return current;
}

} // namespace vw::monitors
