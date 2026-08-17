#pragma once

#include <string>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include "graphics/D3D11DeviceManager.h"
#include "graphics/D3D11Renderer.h"
#include "util/Result.h"

namespace vw::wallpaper {

// One per-monitor child window in the wallpaper layer (docs/02 §2.6): owns its
// window, flip-model swap chain, and D3D11Renderer. Renders the shared test
// texture (M3); M4+ renders decoded video frames instead. All methods must be
// called from the thread that owns the window (the app's UI thread).
class WallpaperHost {
public:
    struct Options {
        HWND parent = nullptr;                 // wallpaper-layer window (WorkerW or Progman)
        RECT bounds{};                         // monitor bounds in physical pixels
        std::wstring monitorId;                // stable id for lookup
        ID3D11ShaderResourceView* textureSrv = nullptr; // M3 test texture (checkerboard)
    };

    WallpaperHost() = default;
    ~WallpaperHost();

    WallpaperHost(const WallpaperHost&) = delete;
    WallpaperHost& operator=(const WallpaperHost&) = delete;

    // Creates the child window, swap chain, and renderer pipeline. The
    // deviceManager must outlive this host (WallpaperManager owns both).
    Result<void> init(gfx::D3D11DeviceManager* deviceManager, const Options& options);

    // Draws the bound texture + presents (vsync). No-op-safe only while valid.
    Result<void> render();

    // Repositions/resizes the window + swap chain (monitor change).
    Result<void> setBounds(const RECT& bounds);

    // Destroys the window and releases the swap chain/renderer (idempotent).
    void destroy();

    HWND hwnd() const { return hwnd_; }
    const std::wstring& monitorId() const { return monitorId_; }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    bool valid() const { return hwnd_ != nullptr && ::IsWindow(hwnd_) != FALSE; }

    // Window class name for host windows.
    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static bool registerClass();
    // Physical monitor rect -> parent-DPI-context window rect (child-window
    // DPI virtualization, see WallpaperHost.cpp).
    RECT scaleToParentDpi(HWND parent, const RECT& physical) const;

    gfx::D3D11DeviceManager* deviceManager_ = nullptr;
    HWND hwnd_ = nullptr;
    std::wstring monitorId_;
    UINT width_ = 0;
    UINT height_ = 0;
    bool shown_ = false; // window created hidden; shown after first present
    gfx::D3D11Renderer renderer_;
};

} // namespace vw::wallpaper
