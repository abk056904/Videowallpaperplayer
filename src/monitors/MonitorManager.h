#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>

#include "util/Result.h"

namespace vw::monitors {

// A single connected display (docs/02 §2.7). `id` is the stable device name
// (e.g. L"\\\\.\\DISPLAY1"); HMONITOR values are NOT stable across
// connect/disconnect events, so the id is the map key everywhere.
struct MonitorInfo {
    std::wstring id;                 // e.g. L"\\\\.\\DISPLAY1"
    HMONITOR handle = nullptr;
    RECT bounds{};                   // physical pixels (per-monitor DPI aware)
    RECT workArea{};
    UINT width = 0;
    UINT height = 0;
    UINT refreshRateNumerator = 0;   // current mode, from the DXGI output
    UINT refreshRateDenominator = 0;
    bool primary = false;
    bool active = true;              // currently connected
};

// Enumerates the connected monitors (EnumDisplayMonitors + GetMonitorInfo)
// with refresh rates matched from DXGI output enumeration. Emits
// add/remove/change events keyed by stable id (docs/02 §2.7). M8 extends
// this with full adapter association and multi-GPU locality.
class MonitorManager {
public:
    using MonitorEvent = std::function<void(const std::wstring& id)>;

    MonitorManager() = default;

    // Fresh scan of the current monitor set (no events fired).
    static Result<std::vector<MonitorInfo>> enumerateMonitors();

    // Re-scans and fires onAdded/onRemoved/onChanged for differences versus
    // the previous snapshot (diff keyed by stable id). Returns the new
    // snapshot. The FIRST call after wiring events reports every current
    // monitor as "added".
    Result<std::vector<MonitorInfo>> refresh();

    void setOnAdded(MonitorEvent e) { onAdded_ = std::move(e); }
    void setOnRemoved(MonitorEvent e) { onRemoved_ = std::move(e); }
    void setOnChanged(MonitorEvent e) { onChanged_ = std::move(e); }

private:
    std::vector<MonitorInfo> last_;
    MonitorEvent onAdded_;
    MonitorEvent onRemoved_;
    MonitorEvent onChanged_;
};

} // namespace vw::monitors
