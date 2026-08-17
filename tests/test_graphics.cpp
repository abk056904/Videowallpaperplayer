#include "doctest.h"

#include <dxgi.h>

#include "graphics/D3D11DeviceManager.h"

using vw::gfx::D3D11DeviceManager;

TEST_CASE("gfx: adapter enumeration is sane") {
    // Works headless: the Microsoft Basic Render Driver always exists, so the
    // list is never empty and every entry has a description + vendor id.
    auto adapters = D3D11DeviceManager::enumerateAdapters();
    REQUIRE(adapters);
    CHECK_FALSE(adapters->empty());
    for (const auto& a : *adapters) {
        CHECK_FALSE(a.description.empty());
        CHECK(a.vendor != 0);
        CHECK(a.device != 0);
    }
}

TEST_CASE("gfx: getAdapter by index matches enumeration") {
    auto adapters = D3D11DeviceManager::enumerateAdapters();
    REQUIRE(adapters);
    auto adapter = D3D11DeviceManager::getAdapter(0);
    REQUIRE(adapter);
    CHECK((*adapter) != nullptr);

    // Out-of-range index fails cleanly.
    const UINT beyond = static_cast<UINT>(adapters->size()) + 10;
    auto missing = D3D11DeviceManager::getAdapter(beyond);
    CHECK_FALSE(missing);
}

TEST_CASE("gfx: device-loss classification") {
    CHECK(D3D11DeviceManager::isDeviceLost(DXGI_ERROR_DEVICE_REMOVED));
    CHECK(D3D11DeviceManager::isDeviceLost(DXGI_ERROR_DEVICE_RESET));
    CHECK_FALSE(D3D11DeviceManager::isDeviceLost(S_OK));
    CHECK_FALSE(D3D11DeviceManager::isDeviceLost(E_FAIL));
    CHECK_FALSE(D3D11DeviceManager::isDeviceLost(DXGI_ERROR_INVALID_CALL));

    // Both reasons produce a readable message.
    CHECK(D3D11DeviceManager::deviceLostReason(DXGI_ERROR_DEVICE_REMOVED) != nullptr);
    CHECK(D3D11DeviceManager::deviceLostReason(DXGI_ERROR_DEVICE_RESET) != nullptr);
}

TEST_CASE("gfx: device-loss stub round-trips the recreate request") {
    D3D11DeviceManager mgr;
    CHECK_FALSE(mgr.consumeRecreateRequest()); // nothing pending initially
    mgr.scheduleRecreate();                    // stub: logs + sets flag
    CHECK(mgr.consumeRecreateRequest());       // consumed exactly once
    CHECK_FALSE(mgr.consumeRecreateRequest()); // cleared after consumption
}
