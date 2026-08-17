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

MonitorDiff diffMonitorSets(const std::vector<MonitorInfo>& prev,
                            const std::vector<MonitorInfo>& current) {
    MonitorDiff diff;
    std::map<std::wstring, const MonitorInfo*> prevById;
    for (const auto& m : prev) {
        prevById[m.id] = &m;
    }
    for (const auto& m : current) {
        const auto it = prevById.find(m.id);
        if (it == prevById.end()) {
            diff.added.push_back(m.id);
            continue;
        }
        const auto& old = *it->second;
        const bool moved = old.bounds.left != m.bounds.left ||
                           old.bounds.top != m.bounds.top ||
                           old.bounds.right != m.bounds.right ||
                           old.bounds.bottom != m.bounds.bottom;
        const bool resized = old.width != m.width || old.height != m.height;
        const bool refreshChanged =
            old.refreshRateNumerator != m.refreshRateNumerator ||
            old.refreshRateDenominator != m.refreshRateDenominator;
        const bool workChanged = old.workArea.left != m.workArea.left ||
                                 old.workArea.top != m.workArea.top ||
                                 old.workArea.right != m.workArea.right ||
                                 old.workArea.bottom != m.workArea.bottom;
        // HMONITOR is deliberately excluded: it is not stable across
        // redetection and would fire spurious "changed" events.
        if (moved || resized || refreshChanged || workChanged || old.primary != m.primary) {
            diff.changed.push_back(m.id);
        }
    }
    for (const auto& [id, old] : prevById) {
        const bool stillHere = std::any_of(current.begin(), current.end(),
                                           [&](const MonitorInfo& m) { return m.id == id; });
        if (!stillHere) {
            diff.removed.push_back(id);
        }
    }
    // Deterministic order (stable ids) for testability.
    std::sort(diff.added.begin(), diff.added.end());
    std::sort(diff.removed.begin(), diff.removed.end());
    std::sort(diff.changed.begin(), diff.changed.end());
    return diff;
}

void associateAdapters(std::vector<MonitorInfo>& monitors,
                       const std::vector<gfx::AdapterInfo>& adapters,
                       const std::vector<std::vector<gfx::OutputInfo>>& outputsByAdapter) {
    // A monitor belongs to the adapter whose output's DesktopCoordinates
    // CONTAIN the monitor's bounds center (a monitor is one output). Multiple
    // adapters can drive the same desktop (hybrid laptops) — match precisely.
    for (auto& m : monitors) {
        m.adapterIndex = 0;
        m.adapterLuid = {};
        const LONG cx = (m.bounds.left + m.bounds.right) / 2;
        const LONG cy = (m.bounds.top + m.bounds.bottom) / 2;
        for (size_t a = 0; a < adapters.size() && a < outputsByAdapter.size(); ++a) {
            for (const auto& o : outputsByAdapter[a]) {
                if (cx >= o.left && cx < o.right && cy >= o.top && cy < o.bottom) {
                    m.adapterIndex = static_cast<UINT>(a);
                    m.adapterLuid = adapters[a].luid;
                    break;
                }
            }
        }
    }
}

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

    // M8: monitor -> driving adapter (multi-GPU locality).
    std::vector<gfx::AdapterInfo> adapters;
    std::vector<std::vector<gfx::OutputInfo>> outputsByAdapter;
    if (auto ads = gfx::D3D11DeviceManager::enumerateAdapters()) {
        adapters = std::move(*ads);
        for (size_t i = 0; i < adapters.size(); ++i) {
            if (auto ad = gfx::D3D11DeviceManager::getAdapter(static_cast<UINT>(i))) {
                if (auto outs = gfx::D3D11DeviceManager::enumerateOutputs(ad->Get())) {
                    outputsByAdapter.push_back(std::move(*outs));
                } else {
                    outputsByAdapter.emplace_back();
                }
            } else {
                outputsByAdapter.emplace_back();
            }
        }
    }
    associateAdapters(monitors, adapters, outputsByAdapter);

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
    // Pure diff over the previous snapshot (simulated topologies are
    // unit-tested via diffMonitorSets directly).
    const MonitorDiff diff = diffMonitorSets(last_, *current);
    for (const auto& id : diff.added) {
        if (onAdded_) {
            onAdded_(id);
        }
    }
    for (const auto& id : diff.removed) {
        if (onRemoved_) {
            onRemoved_(id);
        }
    }
    for (const auto& id : diff.changed) {
        if (onChanged_) {
            onChanged_(id);
        }
    }
    last_ = *current;
    return current;
}

} // namespace vw::monitors
