#include "wallpaper/WallpaperHost.h"

#include <string>

namespace vw::wallpaper {

namespace {

// Matches WallpaperManager's checkerboard (M3 test texture) so the initial
// bind uses the real size (a square is identity under every scaling mode).
constexpr UINT kTestTextureSize = 512;

} // namespace

const wchar_t* WallpaperHost::kClassName = L"VideoWallpaper.WallpaperHost";

WallpaperHost::~WallpaperHost() {
    destroy();
}

bool WallpaperHost::registerClass() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WallpaperHost::wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // No background brush: the renderer paints the whole surface; the default
    // brush would flash black/erased pixels before the first present.
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// The wallpaper host is a WS_CHILD parented into Explorer's WorkerW. BOTH the
// app (SetProcessDpiAwarenessContext PER_MONITOR_AWARE_V2) and Explorer are
// per-monitor DPI aware, so child-window coordinates are already PHYSICAL
// pixels — the monitor bounds (per-monitor-v2 units) map 1:1, no conversion.
// (An earlier version scaled by parentDpi/96; that was only correct at 96 DPI
// where the factor is identity. At 125% scaling it inflated the window to
// 2400x1350 on a 1920x1080 screen — the visible top-left corner looked like
// a zoomed/cropped video. Verified with a DPI-aware GetWindowRect probe.)
RECT WallpaperHost::scaleToParentDpi(HWND, const RECT& physical) const {
    return physical;
}

LRESULT CALLBACK WallpaperHost::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // renderer paints everything; never let the system erase
        case WM_PAINT: {
            // Nothing to draw here (the renderer owns the surface via the swap
            // chain); just keep the region valid so GDI never erases us.
            PAINTSTRUCT ps{};
            ::BeginPaint(hwnd, &ps);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

Result<void> WallpaperHost::init(gfx::D3D11DeviceManager* deviceManager, const Options& options) {
    if (hwnd_) {
        return std::unexpected(L"WallpaperHost already initialized");
    }
    if (!deviceManager || !deviceManager->device()) {
        return std::unexpected(L"WallpaperHost::init: no device");
    }
    if (!options.parent || !::IsWindow(options.parent)) {
        return std::unexpected(L"WallpaperHost::init: invalid parent");
    }
    deviceManager_ = deviceManager;
    monitorId_ = options.monitorId;
    width_ = static_cast<UINT>(options.bounds.right - options.bounds.left);
    height_ = static_cast<UINT>(options.bounds.bottom - options.bounds.top);

    if (!registerClass()) {
        return std::unexpected(L"failed to register host window class");
    }

    // DPI: see scaleToParentDpi() — the child is virtualized into the parent's
    // context, so create the window at the scaled rect; the swap chain keeps
    // the PHYSICAL size (crisp back buffer) and DWM maps it 1:1.
    const RECT scaled = scaleToParentDpi(options.parent, options.bounds);

    // Child of the wallpaper layer: stays behind desktop icons, never appears
    // in the taskbar, never takes focus. Created without WS_VISIBLE; render()
    // shows it after the first present (no black flash).
    hwnd_ = ::CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY, kClassName, L"VideoWallpaperHost",
        WS_CHILD | WS_CLIPSIBLINGS, scaled.left, scaled.top,
        scaled.right - scaled.left, scaled.bottom - scaled.top, options.parent, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        return std::unexpected(L"CreateWindowExW failed (error " +
                               std::to_wstring(::GetLastError()) + L")");
    }

    auto swapChain = deviceManager_->createSwapChain(hwnd_, width_, height_);
    if (!swapChain) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return std::unexpected(swapChain.error());
    }

    auto initResult = renderer_.init(deviceManager_->device(), swapChain->Get(), width_, height_);
    if (!initResult) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return initResult;
    }

    // M3: the shared checkerboard test texture (512x512, square -> any scaling
    // mode is identity); M4+ rebinds per video frame with real dimensions.
    if (options.textureSrv) {
        auto setResult = renderer_.setVideoTexture(options.textureSrv, 1.0f, // 512x512 square
                                                   gfx::D3D11Renderer::Scaling::Fill);
        if (!setResult) {
            ::DestroyWindow(hwnd_);
            hwnd_ = nullptr;
            return setResult;
        }
    }
    return {};
}

Result<void> WallpaperHost::render() {
    if (!deviceManager_ || !hwnd_) {
        return std::unexpected(L"WallpaperHost::render: not initialized");
    }
    if (!valid()) {
        return std::unexpected(L"WallpaperHost::render: window gone");
    }
    gfx::D3D11Renderer::FrameParams params{};
    params.tint[0] = params.tint[1] = params.tint[2] = params.tint[3] = 1.0f;
    auto result = renderer_.render(deviceManager_->context(), params);
    if (result && !shown_) {
        // First successful present: make the window visible (created hidden to
        // avoid a black flash before the swap chain has content).
        ::ShowWindow(hwnd_, SW_SHOWNA);
        shown_ = true;
    }
    return result;
}

Result<void> WallpaperHost::setVideoTexture(ID3D11ShaderResourceView* srv, float videoAspect,
                                           gfx::D3D11Renderer::Scaling scaling) {
    if (!deviceManager_ || !hwnd_) {
        return std::unexpected(L"WallpaperHost::setVideoTexture: not initialized");
    }
    return renderer_.setVideoTexture(srv, videoAspect, scaling);
}

Result<void> WallpaperHost::setVideoPlanes(ID3D11ShaderResourceView* ySrv,
                                           ID3D11ShaderResourceView* uvSrv, float videoAspect,
                                           gfx::D3D11Renderer::Scaling scaling) {
    if (!deviceManager_ || !hwnd_) {
        return std::unexpected(L"WallpaperHost::setVideoPlanes: not initialized");
    }
    return renderer_.setVideoPlanes(ySrv, uvSrv, videoAspect, scaling);
}

Result<void> WallpaperHost::setBounds(const RECT& bounds) {
    if (!deviceManager_ || !hwnd_) {
        return std::unexpected(L"WallpaperHost::setBounds: not initialized");
    }
    width_ = static_cast<UINT>(bounds.right - bounds.left);
    height_ = static_cast<UINT>(bounds.bottom - bounds.top);
    // Same DPI virtualization as init: position the window in the parent's
    // context, keep the swap chain at the physical size.
    const RECT scaled = scaleToParentDpi(::GetParent(hwnd_), bounds);
    ::SetWindowPos(hwnd_, nullptr, scaled.left, scaled.top,
                   scaled.right - scaled.left, scaled.bottom - scaled.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    auto resized = renderer_.resize(deviceManager_->device(), width_, height_);
    if (!resized) {
        return resized;
    }
    return render();
}

void WallpaperHost::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    // renderer_ ComPtrs release the swap chain/pipeline here (or on host
    // destruction) — safe once the window is gone.
}

} // namespace vw::wallpaper
