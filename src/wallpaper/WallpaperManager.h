#pragma once

#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include "graphics/D3D11DeviceManager.h"
#include "monitors/MonitorManager.h"
#include "util/Result.h"

namespace vw::video {
struct DecodedFrame;
}

namespace vw::wallpaper {

class WallpaperHost;

// Owns the wallpaper layer (docs/02 §2.6): runtime desktop discovery
// (Progman/WorkerW/SHELLDLL_DefView), one WallpaperHost per monitor, the
// shared M3 test texture, monitor-event wiring (WM_DISPLAYCHANGE /
// WM_DEVICECHANGE), and the Explorer-restart validity stub (~1 Hz; full
// resilience logic lands in M12). UI-thread only, zero busy loops.
class WallpaperManager {
public:
    // The desktop hierarchy found at discovery (logged + kept for tests).
    struct DesktopLayer {
        HWND progman = nullptr;      // icon host (bottom of the desktop stack)
        HWND iconLayer = nullptr;    // WorkerW containing SHELLDLL_DefView
        HWND wallpaperLayer = nullptr; // host parent (WorkerW or Progman)
        std::wstring description;    // human-readable arrangement
    };

    WallpaperManager();
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;

    // NOTE: both special members are out-of-line because hosts_ holds
    // unique_ptr<WallpaperHost> (forward-declared here); MSVC instantiates
    // the vector's element destructor from an inline defaulted ctor/dtor.

    // Creates the device (on the given adapter, nullptr = default), snapshots
    // monitors, and attempts the initial build. If Explorer is unavailable
    // the wallpaper appears on the next onTick() once the shell is back
    // (Explorer-restart stub).
    Result<void> start(IDXGIAdapter1* adapter = nullptr);

    // Re-renders every host (each present is vsync-blocked). The app calls
    // this when new frames arrive (M4+); the harness uses it to drive
    // sustained rendering.
    Result<void> renderAll();

    // Uploads one decoded frame (tightly-packed B8G8R8A8) to a persistent
    // texture and rebinds it on every host (M4 software path; M5 swaps to GPU
    // surfaces). EOS/empty frames keep the last presented frame. Recreates the
    // texture if the frame size changes (loop across resolutions).
    Result<void> setVideoFrame(const video::DecodedFrame& frame);

    // Monitor changes (WM_DISPLAYCHANGE / WM_DEVICECHANGE): refresh + sync
    // hosts via the add/remove/change events.
    void onDisplayChange();

    // Low-frequency validity check (~1 Hz, only while running — docs/02
    // §2.8): rebuilds hosts when the wallpaper layer or any host died
    // (Explorer restart). Full logic in M12.
    void onTick();

    // Tears down hosts (windows + swap chains) — deterministic shutdown.
    void shutdown();

    bool running() const { return running_; }
    // Out-of-line: hosts_ holds unique_ptr<WallpaperHost> (forward-declared
    // here), and MSVC instantiates the element's destructor when an inline
    // member touches the vector.
    size_t hostCount() const;
    const std::vector<monitors::MonitorInfo>& monitors() const { return monitors_; }
    const DesktopLayer& layer() const { return layer_; }

private:
    Result<void> discoverDesktop();
    Result<void> ensureTestTexture();
    Result<void> createHosts();
    Result<void> bindFrameTexture();
    void teardownHosts();
    void addHostFor(const std::wstring& monitorId);
    void removeHostFor(const std::wstring& monitorId);
    void repositionHost(const std::wstring& monitorId);
    void logHierarchy() const;

    gfx::D3D11DeviceManager deviceManager_;
    monitors::MonitorManager monitorManager_;
    std::vector<monitors::MonitorInfo> monitors_;
    std::vector<std::unique_ptr<WallpaperHost>> hosts_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> testTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> testTextureSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture_; // M4: video upload
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frameTextureSrv_;
    UINT frameWidth_ = 0;
    UINT frameHeight_ = 0;
    DesktopLayer layer_;
    bool running_ = false;
};

} // namespace vw::wallpaper
