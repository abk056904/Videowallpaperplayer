#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include "graphics/D3D11DeviceManager.h"
#include "graphics/D3D11Renderer.h"
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
    // texture if the frame size changes (loop across resolutions). This is the
    // CLONE path: one decoded stream presented on every monitor.
    Result<void> setVideoFrame(const video::DecodedFrame& frame);

    // M8 INDEPENDENT path: uploads the frame to a per-monitor texture and
    // rebinds it on THAT host only, so each display can run its own video
    // (N decoders for N distinct videos; shared device/factory/shaders —
    // docs/03 §3.10). No-op for an unknown monitor id. EOS/empty frames keep
    // the last presented frame.
    Result<void> setVideoFrameFor(const std::wstring& monitorId,
                                  const video::DecodedFrame& frame);

    // Monitor changes (WM_DISPLAYCHANGE / WM_DEVICECHANGE): refresh + sync
    // hosts via the add/remove/change events.
    void onDisplayChange();

    // Low-frequency validity check (~1 Hz, only while running — docs/02
    // §2.8): processes a pending device-loss recreate, then rebuilds hosts
    // when the wallpaper layer or any host died (Explorer restart).
    void onTick();

    // M12 device-loss recovery. `requestDeviceRecreate` is the fault-injection
    // entry point (the harness uses it to simulate a lost device); the 1 Hz
    // onTick consumes the request and runs the full teardown -> device
    // recreate -> rebuild -> re-render sequence with controlled retry/backoff
    // (1 Hz while fresh, every 30 s after consecutive failures) — no tight
    // loops, self-recovers when the GPU returns. True while the recreate is
    // pending or retrying (the app uses it to silence per-frame render-failure
    // log spam during the gap).
    void requestDeviceRecreate() { deviceManager_.scheduleRecreate(); }
    bool isDeviceLost() const {
        return deviceManager_.recreatePending() || recreateFailures_ > 0;
    }

    // Tears down hosts (windows + swap chains) — deterministic shutdown.
    void shutdown();

    bool running() const { return running_; }
    // Out-of-line: hosts_ holds unique_ptr<WallpaperHost> (forward-declared
    // here), and MSVC instantiates the element's destructor when an inline
    // member touches the vector.
    size_t hostCount() const;
    const std::vector<monitors::MonitorInfo>& monitors() const { return monitors_; }
    const DesktopLayer& layer() const { return layer_; }

    // The D3D device backing the wallpaper — handed to the player so the
    // decoder produces GPU surfaces on the same device (M5).
    ID3D11Device* device();

    // Scaling mode for video frames (config.playback.scaling, Fill default).
    void setScaling(gfx::D3D11Renderer::Scaling scaling) { scaling_ = scaling; }

    // M11 Monitors-panel preview: one-time CPU readback of the CURRENT video
    // frame (the software-path upload texture). Returns tightly-packed BGRA8
    // rows (row 0 = top, ready for a top-down DIB) + size. On the hardware/
    // NV12 path there is no CPU copy — returns an error and the UI shows
    // "no preview" (honest fallback per spec §10.5; on this machine the
    // software path is active so preview works).
    struct FrameSnapshot {
        std::vector<uint8_t> bgra;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    Result<FrameSnapshot> grabFrameSnapshot() const;

private:
    Result<void> discoverDesktop();
    Result<void> ensureTestTexture();
    Result<void> createHosts();
    // M12: the device-loss recovery sequence (teardown -> device recreate ->
    // rebuild -> re-render), with consecutive-failure backoff.
    void recreateDeviceResources();
    // M12: backoff pacing for recreate retries (see recreateDeviceResources).
    bool recreateRetryDue();
    // M12: re-applies the last video frames (clone texture / per-monitor
    // textures) to freshly rebuilt hosts so a PAUSED wallpaper doesn't regress
    // to the checkerboard after Explorer restart or device recreate.
    Result<void> rebindLastFrames();
    Result<void> bindFrameTexture();
    Result<void> bindGpuFrame(const video::DecodedFrame& frame);
    // M8: hardware-frame bind on ONE host (Independent mode).
    Result<void> bindGpuFrameFor(const std::wstring& monitorId,
                                 const video::DecodedFrame& frame);
    Result<void> bindFramePlanes(ID3D11ShaderResourceView* ySrv, ID3D11ShaderResourceView* uvSrv,
                                 float videoAspect);
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
    float frameDisplayAspect_ = 0.0f; // SAR-corrected aspect of the bound frame
    // M8: per-monitor upload textures for the INDEPENDENT path (each display
    // runs its own video). Keyed by stable monitor id.
    struct PerMonitorFrame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT width = 0;
        UINT height = 0;
    };
    std::map<std::wstring, PerMonitorFrame> perMonitorFrames_;
    gfx::D3D11Renderer::Scaling scaling_ = gfx::D3D11Renderer::Scaling::Fill;
    DesktopLayer layer_;
    bool running_ = false;
    // M12: consecutive device-recreate failures (drives the retry backoff).
    unsigned recreateFailures_ = 0;
    unsigned ticksSinceRecreate_ = 0;
    bool deviceLostLogged_ = false; // one warn per loss event (no per-frame spam)
};

} // namespace vw::wallpaper
