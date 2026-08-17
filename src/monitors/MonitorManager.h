#pragma once

#include <functional>
#include <string>
#include <vector>

#include <windows.h>

#include "graphics/D3D11DeviceManager.h"
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
    // M8: the DXGI adapter driving this output (multi-GPU locality, docs/03
    // §3.10). index = enumeration order; luid = stable identity. Filled by
    // enumerateMonitors() by matching the output's DesktopCoordinates.
    UINT adapterIndex = 0;
    LUID adapterLuid{};
};

// Pure diff helper (M8): computes add/remove/change between two monitor
// snapshots and invokes the callbacks for the differences, keyed by stable
// id. Deliberately free of any windowing/COM state so simulated topologies
// are unit-testable (docs/03 §3.10 — real hot-plug is NOT MEASURED on the
// single-display dev machine; simulated topologies are the substitute).
// `changed` fires when bounds/work area/size/refresh/primary differ (HMONITOR
// excluded — not stable across redetection).
struct MonitorDiff {
    std::vector<std::wstring> added;
    std::vector<std::wstring> removed;
    std::vector<std::wstring> changed;
};
MonitorDiff diffMonitorSets(const std::vector<MonitorInfo>& prev,
                            const std::vector<MonitorInfo>& current);

// Adapter association (M8): for each monitor, find the DXGI adapter whose
// output contains the monitor's bounds (via DesktopCoordinates) and fill
// adapterIndex/adapterLuid. Pure over the supplied adapter/output lists so it
// is unit-testable; enumerateMonitors() feeds it the real DXGI enumeration.
void associateAdapters(std::vector<MonitorInfo>& monitors,
                       const std::vector<gfx::AdapterInfo>& adapters,
                       const std::vector<std::vector<gfx::OutputInfo>>& outputsByAdapter);

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

    // M8 test hook: applies the diff of a SIMULATED topology vs the previous
    // snapshot and fires the events (the substitute for real hot-plug on the
    // single-display dev machine — docs/03 §3.10).
    void setSnapshotForTest(std::vector<MonitorInfo> snapshot) {
        const MonitorDiff diff = diffMonitorSets(last_, snapshot);
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
        last_ = std::move(snapshot);
    }

private:
    std::vector<MonitorInfo> last_;
    MonitorEvent onAdded_;
    MonitorEvent onRemoved_;
    MonitorEvent onChanged_;
};

} // namespace vw::monitors
