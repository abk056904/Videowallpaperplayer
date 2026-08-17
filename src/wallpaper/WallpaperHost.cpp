#include "wallpaper/WallpaperHost.h"

#include <string>

namespace vw::wallpaper {

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

    // DPI: a WS_CHILD window whose parent belongs to another process is
    // virtualized into the PARENT's DPI context, regardless of this thread's
    // context (verified empirically: physical rect = requested x 96/parentDpi
    // on this machine — Explorer is 120 DPI / 125% scaling). The monitor
    // bounds are physical pixels (the app is per-monitor v2), so request
    // parentDpi/96 x the physical size and the window lands at the full
    // physical monitor. The swap chain keeps the physical size (crisp
    // back buffer); DWM maps the window's physical area 1:1.
    const UINT parentDpi = ::GetDpiForWindow(options.parent);
    const int px = ::MulDiv(options.bounds.left, static_cast<int>(parentDpi), 96);
    const int py = ::MulDiv(options.bounds.top, static_cast<int>(parentDpi), 96);
    const int pw = ::MulDiv(static_cast<int>(width_), static_cast<int>(parentDpi), 96);
    const int ph = ::MulDiv(static_cast<int>(height_), static_cast<int>(parentDpi), 96);

    // Child of the wallpaper layer: stays behind desktop icons, never appears
    // in the taskbar, never takes focus. Created without WS_VISIBLE; render()
    // shows it after the first present (no black flash).
    hwnd_ = ::CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY, kClassName, L"VideoWallpaperHost",
        WS_CHILD | WS_CLIPSIBLINGS, px, py, pw, ph, options.parent, nullptr,
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

    // M3: the shared checkerboard test texture; M4+ rebinds per video frame.
    if (options.textureSrv) {
        auto setResult = renderer_.setVideoTexture(options.textureSrv);
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

Result<void> WallpaperHost::setBounds(const RECT& bounds) {
    if (!deviceManager_ || !hwnd_) {
        return std::unexpected(L"WallpaperHost::setBounds: not initialized");
    }
    width_ = static_cast<UINT>(bounds.right - bounds.left);
    height_ = static_cast<UINT>(bounds.bottom - bounds.top);
    ::SetWindowPos(hwnd_, nullptr, bounds.left, bounds.top, width_, height_,
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
