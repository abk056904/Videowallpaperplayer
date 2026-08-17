#include "graphics/D3D11DeviceManager.h"

#include <format>
#include <windows.h>

#include "logging/Logger.h"

namespace vw::gfx {

namespace {

std::wstring formatHr(HRESULT hr) {
    return std::format(L"hr=0x{:08X}", static_cast<unsigned>(hr));
}

} // namespace

Result<std::vector<AdapterInfo>> D3D11DeviceManager::enumerateAdapters() {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return std::unexpected(L"CreateDXGIFactory2 failed: " + formatHr(hr));
    }
    std::vector<AdapterInfo> out;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        HRESULT enumHr = factory->EnumAdapters1(i, &adapter);
        if (enumHr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enumHr)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        AdapterInfo info;
        info.description = desc.Description;
        info.vendor = desc.VendorId;
        info.device = desc.DeviceId;
        info.dedicatedVram = desc.DedicatedVideoMemory;
        info.sharedSystem = desc.SharedSystemMemory;
        info.luid = desc.AdapterLuid; // M8
        out.push_back(std::move(info));
    }
    if (out.empty()) {
        return std::unexpected(L"no DXGI adapters found");
    }
    return out;
}

Result<Microsoft::WRL::ComPtr<IDXGIAdapter1>> D3D11DeviceManager::getAdapter(UINT index) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return std::unexpected(L"CreateDXGIFactory2 failed: " + formatHr(hr));
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(index, &adapter);
    if (FAILED(hr)) {
        return std::unexpected(std::format(L"no adapter at index {} ({})", index, formatHr(hr)));
    }
    return adapter;
}

Result<std::vector<OutputInfo>> D3D11DeviceManager::enumerateOutputs(IDXGIAdapter1* adapter) {
    std::vector<OutputInfo> out;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT i = 0;; ++i) {
        HRESULT hr = adapter->EnumOutputs(i, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;
        DXGI_OUTPUT_DESC od{};
        if (FAILED(output->GetDesc(&od))) {
            output.Reset();
            continue;
        }
        OutputInfo info;
        info.deviceName = od.DeviceName;
        info.left = od.DesktopCoordinates.left;
        info.top = od.DesktopCoordinates.top;
        info.right = od.DesktopCoordinates.right;
        info.bottom = od.DesktopCoordinates.bottom;
        // Current mode via the display settings API (physical pixels).
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (::EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            info.width = static_cast<LONG>(dm.dmPelsWidth);
            info.height = static_cast<LONG>(dm.dmPelsHeight);
            info.refreshHz = static_cast<LONG>(dm.dmDisplayFrequency);
        }
        out.push_back(std::move(info));
        output.Reset();
    }
    if (out.empty()) {
        return std::unexpected(L"adapter has no outputs (headless?)");
    }
    return out;
}

Result<void> D3D11DeviceManager::createDevice(IDXGIAdapter1* adapter, bool wantDebugLayer) {
    // Remember the adapter + debug request so recreate() (M12 device-loss
    // recovery) rebuilds an equivalent device without re-deriving them.
    adapter_ = adapter;
    wantDebugLayer_ = wantDebugLayer;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // VIDEO_SUPPORT is required for Media Foundation hardware decode (the
    // decoder MFT calls ID3D11VideoDevice on this device; without it the
    // DXGI surface path crashes). BGRA for RGB32 uploads/render targets.
    const UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    auto& log = log::Logger::instance();

    // D3D11CreateDevice contract: UNKNOWN driver type requires a non-null
    // adapter (default adapter selection only works with HARDWARE). The
    // harness always passes an adapter; the app may pass nullptr.
    const D3D_DRIVER_TYPE driverType =
        (adapter != nullptr) ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    if (wantDebugLayer) {
        HRESULT hr = ::D3D11CreateDevice(adapter, driverType, nullptr,
                                         baseFlags | D3D11_CREATE_DEVICE_DEBUG, levels, 2,
                                         D3D11_SDK_VERSION, &device_, &featureLevel_, &context_);
        if (SUCCEEDED(hr)) {
            debugLayer_ = true;
        } else {
            log.warn(L"D3D11 debug layer unavailable ({}), falling back", formatHr(hr));
            device_.Reset();
            context_.Reset();
        }
    }
    if (!device_) {
        HRESULT hr = ::D3D11CreateDevice(adapter, driverType, nullptr, baseFlags, levels, 2,
                                         D3D11_SDK_VERSION, &device_, &featureLevel_, &context_);
        if (FAILED(hr)) {
            return std::unexpected(L"D3D11CreateDevice failed: " + formatHr(hr));
        }
    }

    log.info(L"D3D11 device created: feature level 0x{:04X}, debug layer {}",
             static_cast<unsigned>(featureLevel_), debugLayer_ ? L"ON" : L"off");
    return {};
}

Result<Microsoft::WRL::ComPtr<IDXGISwapChain1>> D3D11DeviceManager::createSwapChain(
    HWND hwnd, UINT width, UINT height) const {
    if (!device_) return std::unexpected(L"device not created");

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_.As(&dxgiDevice))) {
            return std::unexpected(L"device is not an IDXGIDevice");
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
            return std::unexpected(L"GetAdapter failed");
        }
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return std::unexpected(L"GetParent(IDXGIFactory4) failed");
        }
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr,
                                                 &swapChain);
    if (FAILED(hr)) {
        return std::unexpected(L"CreateSwapChainForHwnd failed: " + formatHr(hr));
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return swapChain;
}

bool D3D11DeviceManager::isDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

const wchar_t* D3D11DeviceManager::deviceLostReason(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED) return L"DXGI_ERROR_DEVICE_REMOVED";
    if (hr == DXGI_ERROR_DEVICE_RESET) return L"DXGI_ERROR_DEVICE_RESET";
    return L"unknown device-loss code";
}

void D3D11DeviceManager::scheduleRecreate() {
    recreatePending_ = true;
}

bool D3D11DeviceManager::consumeRecreateRequest() {
    const bool pending = recreatePending_;
    recreatePending_ = false;
    return pending;
}

Result<void> D3D11DeviceManager::recreate() {
    // GetDeviceRemovedReason on the lost device for the log BEFORE release.
    const std::wstring removed = deviceRemovedReasonString();
    device_.Reset();
    context_.Reset();
    featureLevel_ = D3D_FEATURE_LEVEL_11_0;
    debugLayer_ = false;
    auto result = createDevice(adapter_.Get(), wantDebugLayer_);
    if (!result) {
        return std::unexpected(L"device recreate failed" + (removed.empty() ? std::wstring{}
                                                                           : L" (old device: " +
                                                                                 removed + L")"));
    }
    log::Logger::instance().info(L"D3D11 device recreated (old: {})",
                                 removed.empty() ? L"reason unknown" : removed);
    return {};
}

std::wstring D3D11DeviceManager::deviceRemovedReasonString() const {
    if (!device_) {
        return {};
    }
    const HRESULT reason = device_->GetDeviceRemovedReason();
    if (SUCCEEDED(reason)) {
        return {};
    }
    if (reason == DXGI_ERROR_DEVICE_REMOVED) return L"DXGI_ERROR_DEVICE_REMOVED";
    if (reason == DXGI_ERROR_DEVICE_RESET) return L"DXGI_ERROR_DEVICE_RESET";
    if (reason == DXGI_ERROR_DEVICE_HUNG) return L"DXGI_ERROR_DEVICE_HUNG";
    if (reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR) return L"DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    return formatHr(reason);
}

} // namespace vw::gfx
