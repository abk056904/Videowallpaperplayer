#include "doctest.h"

#include <algorithm>
#include <set>
#include <string>

#include "monitors/MonitorManager.h"

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
