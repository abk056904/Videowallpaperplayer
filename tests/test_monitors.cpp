#include "doctest.h"

#include <algorithm>
#include <set>
#include <string>

#include "monitors/MonitorManager.h"

namespace {

// M8: simulated-topology helper — builds a MonitorInfo with the given id and
// optional overrides (docs/03 §3.10: real hot-plug is NOT MEASURED on the
// single-display dev machine; simulated topologies are the substitute).
struct SimMonitor {
    std::wstring id;
    LONG left = 0, top = 0, right = 1920, bottom = 1080;
    LONG workLeft = 0, workTop = 0, workRight = 1920, workBottom = 1080;
    UINT refresh = 144;
    bool primary = false;
};

vw::monitors::MonitorInfo makeMonitor(const SimMonitor& s) {
    vw::monitors::MonitorInfo m;
    m.id = s.id;
    m.bounds = {s.left, s.top, s.right, s.bottom};
    m.workArea = {s.workLeft, s.workTop, s.workRight, s.workBottom};
    m.width = static_cast<UINT>(s.right - s.left);
    m.height = static_cast<UINT>(s.bottom - s.top);
    m.refreshRateNumerator = s.refresh;
    m.refreshRateDenominator = 1;
    m.primary = s.primary;
    return m;
}

} // namespace

// Live tests: require a real display (like test_graphics.cpp — this dev
// machine has one). M8 extends with multi-monitor / adapter-association cases.

TEST_CASE("MonitorManager enumerates at least one monitor with sane bounds") {
    auto monitors = vw::monitors::MonitorManager::enumerateMonitors();
    REQUIRE(monitors.has_value());
    REQUIRE(monitors->size() >= 1);

    std::set<std::wstring> ids;
    int primaries = 0;
    for (const auto& m : *monitors) {
        ids.insert(m.id);
        REQUIRE(m.id.size() > 0);
        REQUIRE(m.handle != nullptr);
        REQUIRE(m.width > 0);
        REQUIRE(m.height > 0);
        REQUIRE(m.bounds.right > m.bounds.left);
        REQUIRE(m.bounds.bottom > m.bounds.top);
        if (m.primary) {
            ++primaries;
        }
    }
    // Stable unique ids — the key for add/remove/change diffs.
    REQUIRE(ids.size() == monitors->size());
    // Exactly one primary monitor.
    REQUIRE(primaries == 1);
}

TEST_CASE("MonitorManager matches the current-mode refresh rate from DXGI") {
    // This machine's display is 1920x1080 @ 144 Hz (verified by the gfx
    // harness). The primary monitor should carry that refresh rate.
    auto monitors = vw::monitors::MonitorManager::enumerateMonitors();
    REQUIRE(monitors.has_value());
    const auto primary = std::find_if(monitors->begin(), monitors->end(),
                                      [](const vw::monitors::MonitorInfo& m) { return m.primary; });
    REQUIRE(primary != monitors->end());
    REQUIRE(primary->refreshRateNumerator > 0);
    REQUIRE(primary->refreshRateDenominator > 0);
}

TEST_CASE("MonitorManager::refresh fires events only for real differences") {
    vw::monitors::MonitorManager mgr;
    int added = 0, removed = 0, changed = 0;
    mgr.setOnAdded([&](const std::wstring&) { ++added; });
    mgr.setOnRemoved([&](const std::wstring&) { ++removed; });
    mgr.setOnChanged([&](const std::wstring&) { ++changed; });

    // First refresh reports every current monitor as "added" (diff vs empty).
    auto first = mgr.refresh();
    REQUIRE(first.has_value());
    REQUIRE(added == static_cast<int>(first->size()));
    REQUIRE(removed == 0);
    REQUIRE(changed == 0);

    // Second refresh with an unchanged desktop fires nothing.
    auto second = mgr.refresh();
    REQUIRE(second.has_value());
    REQUIRE(added == static_cast<int>(first->size()));
    REQUIRE(removed == 0);
    REQUIRE(changed == 0);

    // Snapshots are identical (stable id ordering).
    REQUIRE(first->size() == second->size());
    for (size_t i = 0; i < first->size(); ++i) {
        REQUIRE((*first)[i].id == (*second)[i].id);
    }
}

// ---- M8: simulated topologies ----------------------------------------------

TEST_CASE("monitors: diff is empty for identical snapshots") {
    const auto a = makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080});
    const auto b = makeMonitor({L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080});
    const std::vector<vw::monitors::MonitorInfo> snap{a, b};
    auto diff = vw::monitors::diffMonitorSets(snap, snap);
    CHECK(diff.added.empty());
    CHECK(diff.removed.empty());
    CHECK(diff.changed.empty());
}

TEST_CASE("monitors: diff reports added/removed keyed by stable id") {
    const auto a = makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080});
    const auto b = makeMonitor({L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080});
    const auto c = makeMonitor({L"\\\\.\\DISPLAY3", 0, 1080, 1920, 2160});

    auto added = vw::monitors::diffMonitorSets({}, {a, b});
    REQUIRE(added.added.size() == 2);
    CHECK(added.removed.empty());
    CHECK(added.changed.empty());

    // b disconnects, c connects.
    auto mixed = vw::monitors::diffMonitorSets({a, b}, {a, c});
    REQUIRE(mixed.added.size() == 1);
    CHECK(mixed.added[0] == c.id);
    REQUIRE(mixed.removed.size() == 1);
    CHECK(mixed.removed[0] == b.id);
    CHECK(mixed.changed.empty());
}

TEST_CASE("monitors: diff reports move / resize / refresh / primary changes") {
    const auto orig = makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, 0, 0, 1920, 1080, 144, true});
    { // moved (bounds shifted)
        auto moved = orig;
        moved.bounds = {100, 50, 2020, 1130}; // same size, new origin
        auto d = vw::monitors::diffMonitorSets({orig}, {moved});
        REQUIRE(d.changed.size() == 1);
        CHECK(d.changed[0] == orig.id);
    }
    { // resized
        auto resized = orig;
        resized.width = 2560;
        resized.height = 1440;
        auto d = vw::monitors::diffMonitorSets({orig}, {resized});
        REQUIRE(d.changed.size() == 1);
    }
    { // refresh changed (mixed refresh rates)
        auto slower = orig;
        slower.refreshRateNumerator = 60;
        auto d = vw::monitors::diffMonitorSets({orig}, {slower});
        REQUIRE(d.changed.size() == 1);
    }
    { // primary moved
        auto other = orig;
        other.primary = false;
        auto d = vw::monitors::diffMonitorSets({orig}, {other});
        REQUIRE(d.changed.size() == 1);
    }
    { // work area changed (taskbar moved)
        auto w = orig;
        w.workArea.bottom = 1040; // taskbar taller
        auto d = vw::monitors::diffMonitorSets({orig}, {w});
        REQUIRE(d.changed.size() == 1);
    }
    { // HMONITOR change alone is NOT a change (unstable handle)
        auto re = orig;
        re.handle = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0xDEAD));
        auto d = vw::monitors::diffMonitorSets({orig}, {re});
        CHECK(d.changed.empty());
    }
}

TEST_CASE("monitors: refreshWithSnapshot publishes the snapshot BEFORE events") {
    // Regression (M8 review): the diff handlers must see the NEW monitor set
    // inside their own event — a hot-plugged display is invisible to its own
    // onAdded handler otherwise (WallpaperManager never creates its host).
    vw::monitors::MonitorManager mgr;
    size_t monitorsSeenAtLastAdd = 0;
    mgr.setOnAdded([&](const std::wstring& id) {
        // The snapshot is already live: the handler can find the added
        // monitor inside its own event (regression: before the fix, last_
        // was still the OLD set, so a hot-plugged monitor was invisible).
        const auto& live = mgr.snapshotForTest();
        const bool inLast = std::any_of(live.begin(), live.end(),
                                        [&](const vw::monitors::MonitorInfo& m) {
                                            return m.id == id;
                                        });
        REQUIRE(inLast);
        monitorsSeenAtLastAdd = live.size();
    });

    const auto a = makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080});
    const auto b = makeMonitor({L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080});
    auto first = mgr.refreshWithSnapshot({a});
    REQUIRE(first.has_value());
    REQUIRE(first->size() == 1);
    REQUIRE(monitorsSeenAtLastAdd == 1);

    // Hot-plug b: its onAdded fires while the new snapshot is already live.
    auto second = mgr.refreshWithSnapshot({a, b});
    REQUIRE(second.has_value());
    REQUIRE(second->size() == 2);
    REQUIRE(monitorsSeenAtLastAdd == 2); // handler saw b present
}

TEST_CASE("monitors: refresh with a simulated topology fires the right events") {
    vw::monitors::MonitorManager mgr;
    std::vector<std::wstring> added, removed, changed;
    mgr.setOnAdded([&](const std::wstring& id) { added.push_back(id); });
    mgr.setOnRemoved([&](const std::wstring& id) { removed.push_back(id); });
    mgr.setOnChanged([&](const std::wstring& id) { changed.push_back(id); });

    const auto a = makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080});
    const auto b = makeMonitor({L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080});

    // First refresh = everything added (diff vs empty).
    mgr.setSnapshotForTest({a, b});
    REQUIRE(added.size() == 2);
    CHECK(removed.empty());
    CHECK(changed.empty());

    // b unplugged.
    added.clear();
    mgr.setSnapshotForTest({a});
    CHECK(added.empty());
    REQUIRE(removed.size() == 1);
    CHECK(removed[0] == b.id);
    CHECK(changed.empty());

    // b re-plugged at a new position + resolution.
    removed.clear();
    mgr.setSnapshotForTest({a, makeMonitor({L"\\\\.\\DISPLAY2", 1920, 0, 4480, 1440, 0, 0, 4480, 1440, 60})});
    REQUIRE(added.size() == 1);
    CHECK(added[0] == b.id);
    CHECK(changed.empty());
}

TEST_CASE("monitors: adapter association matches output DesktopCoordinates") {
    // Two adapters: adapter 0 drives DISPLAY1 (left), adapter 1 drives
    // DISPLAY2 (right) — a hybrid-GPU laptop layout.
    std::vector<vw::gfx::AdapterInfo> adapters(2);
    adapters[0].description = L"Integrated";
    adapters[0].luid = LUID{10, 0};
    adapters[1].description = L"Discrete";
    adapters[1].luid = LUID{20, 0};

    std::vector<std::vector<vw::gfx::OutputInfo>> outputs(2);
    vw::gfx::OutputInfo o0;
    o0.deviceName = L"\\\\.\\DISPLAY1";
    o0.left = 0; o0.top = 0; o0.right = 1920; o0.bottom = 1080;
    outputs[0].push_back(o0);
    vw::gfx::OutputInfo o1;
    o1.deviceName = L"\\\\.\\DISPLAY2";
    o1.left = 1920; o1.top = 0; o1.right = 3840; o1.bottom = 1080;
    outputs[1].push_back(o1);

    std::vector<vw::monitors::MonitorInfo> monitors = {
        makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080}),
        makeMonitor({L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080}),
    };
    vw::monitors::associateAdapters(monitors, adapters, outputs);

    CHECK(monitors[0].adapterIndex == 0);
    CHECK(monitors[0].adapterLuid.HighPart == 0);
    CHECK(monitors[0].adapterLuid.LowPart == 10);
    CHECK(monitors[1].adapterIndex == 1);
    CHECK(monitors[1].adapterLuid.LowPart == 20);
}

TEST_CASE("monitors: adapter association keeps the FIRST match (overlapping outputs)") {
    // Two adapters whose outputs OVERLAP on the desktop (virtual display /
    // surround layouts): the monitor center is inside BOTH. The first adapter
    // must win — a later adapter must not overwrite the association.
    std::vector<vw::gfx::AdapterInfo> adapters(2);
    adapters[0].luid = LUID{11, 0};
    adapters[1].luid = LUID{22, 0};
    std::vector<std::vector<vw::gfx::OutputInfo>> outputs(2);
    vw::gfx::OutputInfo o0;
    o0.left = 0; o0.top = 0; o0.right = 1920; o0.bottom = 1080;
    outputs[0].push_back(o0);
    vw::gfx::OutputInfo o1; // same desktop rect, different adapter
    o1.left = 0; o1.top = 0; o1.right = 1920; o1.bottom = 1080;
    outputs[1].push_back(o1);

    std::vector<vw::monitors::MonitorInfo> monitors = {
        makeMonitor({L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080}),
    };
    vw::monitors::associateAdapters(monitors, adapters, outputs);
    CHECK(monitors[0].adapterIndex == 0);       // first match, not overwritten
    CHECK(monitors[0].adapterLuid.LowPart == 11);
}

TEST_CASE("monitors: adapter association falls back to adapter 0 when unmatched") {
    // A monitor whose center matches NO output (simulated oddity) defaults to
    // adapter 0 with a zero LUID — never an out-of-range index.
    std::vector<vw::gfx::AdapterInfo> adapters(2);
    adapters[0].luid = LUID{7, 0};
    adapters[1].luid = LUID{8, 0};
    std::vector<std::vector<vw::gfx::OutputInfo>> outputs(2);
    vw::gfx::OutputInfo o0;
    o0.left = 0; o0.top = 0; o0.right = 1920; o0.bottom = 1080;
    outputs[0].push_back(o0);

    std::vector<vw::monitors::MonitorInfo> monitors = {
        makeMonitor({L"\\\\.\\DISPLAY9", 9999, 9999, 12000, 12000}),
    };
    vw::monitors::associateAdapters(monitors, adapters, outputs);
    CHECK(monitors[0].adapterIndex == 0);
    CHECK(monitors[0].adapterLuid.LowPart == 0); // unmatched -> zero LUID
}
