#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "util/Result.h"

namespace vw::gfx {

struct AdapterInfo {
    std::wstring description;
    UINT vendor = 0;
    UINT device = 0;
    uint64_t dedicatedVram = 0;
    uint64_t sharedSystem = 0;
    LUID luid{}; // M8: stable adapter identity (monitor->adapter association)
};

struct OutputInfo {
    std::wstring deviceName; // e.g. L"\\\\.\\DISPLAY1"
    LONG left = 0, top = 0, right = 0, bottom = 0;
    LONG width = 0, height = 0;   // current mode
    LONG refreshHz = 0;           // current mode
};

// Device, DXGI enumeration, and swap-chain management (docs/02 §2.3, M2).
// RAII via ComPtr; feature levels {11_1, 11_0}; BGRA support; debug layer on
// request with graceful fallback. Device-loss handling is a stub here and is
// fully wired in M12 (docs/03 §3.14).
class D3D11DeviceManager {
public:
    // Enumerates all DXGI adapters. Works headless (the Basic Render Driver is
    // always present), so it is unit-testable.
    static Result<std::vector<AdapterInfo>> enumerateAdapters();

    // The IDXGIAdapter1 COM pointer for a given index (for createDevice/…).
    static Result<Microsoft::WRL::ComPtr<IDXGIAdapter1>> getAdapter(UINT index);

    // Outputs of one adapter, including the current mode (resolution + refresh).
    static Result<std::vector<OutputInfo>> enumerateOutputs(IDXGIAdapter1* adapter);

    D3D11DeviceManager() = default;
    ~D3D11DeviceManager() = default;

    D3D11DeviceManager(const D3D11DeviceManager&) = delete;
    D3D11DeviceManager& operator=(const D3D11DeviceManager&) = delete;

    // Creates the device on the given adapter (nullptr = default). Debug layer
    // is attempted when requested and silently falls back if unavailable.
    Result<void> createDevice(IDXGIAdapter1* adapter, bool wantDebugLayer);

    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* context() const { return context_.Get(); }
    D3D_FEATURE_LEVEL featureLevel() const { return featureLevel_; }
    bool debugLayer() const { return debugLayer_; }

    // Flip-model swap chain for an HWND (2 buffers, B8G8R8A8, vsync at present).
    Result<Microsoft::WRL::ComPtr<IDXGISwapChain1>> createSwapChain(
        HWND hwnd, UINT width, UINT height) const;

    // ---- device-loss plumbing (docs/03 M2 stub; full sequence M12) ----
    // True when a present/render HRESULT indicates the device was lost/reset.
    static bool isDeviceLost(HRESULT hr);
    // Human-readable reason for logging.
    static const wchar_t* deviceLostReason(HRESULT hr);
    // Records that a recreate is pending (called when a present fails with a
    // device-lost code; the WallpaperManager's 1 Hz tick consumes it and runs
    // the teardown/recreate/rebuild sequence — M12).
    void scheduleRecreate();
    // Consumes the recreate request (returns true if one was pending).
    bool consumeRecreateRequest();
    // True while a recreate is requested but not yet consumed (the app uses
    // this to suppress per-frame render-failure log spam during the gap).
    bool recreatePending() const { return recreatePending_; }
    // M12: releases the (lost) device and recreates it on the SAME adapter
    // with the same debug-layer request as createDevice. The caller must have
    // already released every device-dependent resource (swap chains, RTVs,
    // textures) — this manager owns only the device/context.
    Result<void> recreate();
    // Diagnostic: the lost device's GetDeviceRemovedReason(), formatted.
    // Valid until the device is recreated (call before recreate()).
    std::wstring deviceRemovedReasonString() const;

private:
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter_; // recreated on the same adapter
    bool wantDebugLayer_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_11_0;
    bool debugLayer_ = false;
    bool recreatePending_ = false;
};

} // namespace vw::gfx
