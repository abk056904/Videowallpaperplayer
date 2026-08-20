#include "wallpaper/WallpaperManager.h"

#include <algorithm>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <format>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>

#include "graphics/ScaleMath.h"
#include "graphics/TextureManager.h"
#include "logging/Logger.h"
#include "video/DecodedFrame.h"
#include "wallpaper/WallpaperHost.h"

namespace vw::wallpaper {

namespace {

// Message that asks Explorer to (re)spawn the wallpaper WorkerW (docs/02
// §2.6). Undocumented but stable across Win7..Win11.
constexpr UINT kSpawnWorkerW = 0x052C;

constexpr UINT kTestTextureSize = 512;  // checkerboard: 32 px cells -> 16x16
constexpr UINT kTestCellPx = 32;

// B3: plane-SRV cache cap. Hardware decoders recycle a small pool of
// surfaces, so the map stays at pool size; the cap bounds a pathological
// stream that never reuses a surface pointer (clear + rebuild on exceed).
constexpr size_t kMaxPlaneSrvCacheEntries = 64;

} // namespace

WallpaperManager::WallpaperManager() = default;

WallpaperManager::~WallpaperManager() {
    shutdown();
}

size_t WallpaperManager::hostCount() const {
    return hosts_.size();
}

ID3D11Device* WallpaperManager::device() {
    return deviceManager_.device();
}

Result<void> WallpaperManager::start(IDXGIAdapter1* adapter) {
    if (running_) {
        return {};
    }

#ifdef VW_DEBUG
    const bool wantDebug = true;
#else
    const bool wantDebug = false;
#endif
    auto deviceResult = deviceManager_.createDevice(adapter, wantDebug);
    if (!deviceResult) {
        return deviceResult;
    }

    auto monitors = monitorManager_.refresh(); // initial snapshot (no events wired yet)
    if (!monitors) {
        return std::unexpected(monitors.error());
    }
    monitors_ = *monitors;
    log::Logger::instance().info(L"wallpaper: {} monitor(s) detected", monitors_.size());

    // Events are wired AFTER the initial build so refresh() only fires for
    // real changes from here on.
    monitorManager_.setOnAdded([this](const std::wstring& id) {
        log::Logger::instance().info(L"monitor added: {}", id);
        addHostFor(id);
    });
    monitorManager_.setOnRemoved([this](const std::wstring& id) {
        log::Logger::instance().info(L"monitor removed: {}", id);
        removeHostFor(id);
    });
    monitorManager_.setOnChanged([this](const std::wstring& id) {
        log::Logger::instance().info(L"monitor changed: {}", id);
        repositionHost(id);
    });

    running_ = true;

    // Initial build; on failure the 1 Hz tick retries once Explorer is back.
    const bool built = discoverDesktop() && ensureTestTexture() && createHosts();
    if (!built) {
        teardownHosts();
        layer_ = {};
    } else if (auto rendered = renderAll(); !rendered) {
        log::Logger::instance().warn(L"initial render incomplete: {}", rendered.error());
    }
    return {};
}

void WallpaperManager::onDisplayChange() {
    if (!running_) {
        return;
    }
    // Enumerate FIRST, publish the snapshot to monitors_, THEN fire the diff
    // (refreshWithSnapshot). The onAdded/onChanged handlers run synchronously
    // inside refreshWithSnapshot and look their monitor up in monitors_ — a
    // hot-plugged monitor must already be visible to its own add/change
    // handler or the host is never created / repositioned with stale bounds
    // (ordering bug that survived since M3; M8's per-monitor routing made it
    // visible).
    auto current = monitors::MonitorManager::enumerateMonitors();
    if (!current) {
        log::Logger::instance().warn(L"monitor refresh failed: {}", current.error());
        return;
    }
    monitors_ = *current;
    if (auto diffed = monitorManager_.refreshWithSnapshot(std::move(*current)); !diffed) {
        log::Logger::instance().warn(L"monitor diff failed: {}", diffed.error());
    }
}

namespace {
// M12 device-recreate backoff: retry every tick (1 Hz) while failures are
// fresh, then every 30 ticks (30 s) so a long GPU outage doesn't spin.
constexpr unsigned kMaxFreshRecreateFailures = 10;
constexpr unsigned kSlowRecreateRetryTicks = 30;
} // namespace

void WallpaperManager::onTick() {
    if (!running_) {
        return;
    }

    // M12 device-loss recovery: a pending recreate request (or a retry due
    // after a previous failure) runs FIRST, before the Explorer check — a
    // lost device invalidates every host anyway.
    if (deviceManager_.consumeRecreateRequest() || recreateRetryDue()) {
        recreateDeviceResources();
        return;
    }

    // Explorer-restart check (docs/02 §2.6): ~1 Hz validity check that the
    // wallpaper layer and every host window still exist; on invalidation,
    // rediscover + rebuild hosts + re-apply the last frames + re-render
    // (playlist/config untouched).
    const bool layerInvalid = !layer_.wallpaperLayer || !::IsWindow(layer_.wallpaperLayer);

    bool anyDead = false;
    for (auto it = hosts_.begin(); it != hosts_.end();) {
        if (!(*it)->valid()) {
            anyDead = true;
            it = hosts_.erase(it);
        } else {
            ++it;
        }
    }

    if (layerInvalid || anyDead || hosts_.empty()) {
        if (layerInvalid) {
            log::Logger::instance().warn(L"wallpaper layer invalidated (Explorer restart?) — rebuilding");
        } else if (hosts_.empty()) {
            log::Logger::instance().warn(L"no hosts — retrying wallpaper build");
        }
        teardownHosts();
        layer_ = {};
        if (discoverDesktop() && ensureTestTexture() && createHosts()) {
            // M12: restore the last video frames (a PAUSED wallpaper must not
            // regress to the checkerboard), then render.
            if (auto bound = rebindLastFrames(); !bound) {
                log::Logger::instance().warn(L"rebuild frame rebind incomplete: {}",
                                             bound.error());
            }
            if (auto rendered = renderAll(); !rendered) {
                log::Logger::instance().warn(L"rebuild render incomplete: {}", rendered.error());
            }
        }
    }
}

bool WallpaperManager::recreateRetryDue() {
    if (recreateFailures_ == 0) {
        return false;
    }
    if (recreateFailures_ <= kMaxFreshRecreateFailures) {
        return true; // 1 Hz while the failure is fresh
    }
    if (++ticksSinceRecreate_ < kSlowRecreateRetryTicks) {
        return false;
    }
    ticksSinceRecreate_ = 0;
    return true; // then every 30 s — no tight loop, recovers when the GPU returns
}

void WallpaperManager::recreateDeviceResources() {
    auto& log = log::Logger::instance();
    log.warn(L"device lost — recreating D3D device + wallpaper resources");

    // 1) Stop rendering: hosts (swap chains + renderers) are device-bound.
    teardownHosts();
    // 2) Release every device-dependent resource (textures/SRVs/dims).
    testTexture_.Reset();
    testTextureSrv_.Reset();
    frameTexture_.Reset();
    frameTextureSrv_.Reset();
    nv12YSrv_.Reset();
    nv12UvSrv_.Reset();
    frameIsNv12_ = false;
    planeSrvCache_.clear(); // views are device-bound — all stale after recreate
    sharedFrameCache_.clear(); // shared handles are device-bound too
    frameWidth_ = 0;
    frameHeight_ = 0;
    frameDisplayAspect_ = 0.0f;
    perMonitorFrames_.clear();

    // 3) Recreate the device on the same adapter (GetDeviceRemovedReason
    //    logged inside).
    if (auto ok = deviceManager_.recreate(); !ok) {
        ++recreateFailures_;
        ticksSinceRecreate_ = 0;
        deviceLostLogged_ = false;
        log.error(L"device recreate failed ({} consecutive): {}", recreateFailures_,
                  ok.error());
        // Backoff applies on the next onTick (recreateRetryDue).
        return;
    }

    // 4) Rebuild the wallpaper layer + hosts + re-render. A DEVICE recreate
    //    destroys the last frame textures (the CPU-side frame is gone too —
    //    playback re-uploads on the next frame; a paused wallpaper shows the
    //    test texture until playback resumes — documented limitation).
    layer_ = {};
    const bool rebuilt = discoverDesktop() && ensureTestTexture() && createHosts();
    // The DEVICE is back regardless — clear the device-retry state (a hosts
    // failure is the Explorer-restart path's job, not a device issue).
    recreateFailures_ = 0;
    ticksSinceRecreate_ = 0;
    deviceLostLogged_ = false;
    if (rebuilt) {
        if (auto rendered = renderAll(); !rendered) {
            log.warn(L"post-recreate render incomplete: {}", rendered.error());
        }
        log.info(L"device recreate complete — wallpaper resumed");
    } else {
        // The device is healthy but the desktop isn't (Explorer dead/restarting)
        // — the Explorer-restart check rebuilds hosts when the shell returns.
        log.warn(L"device recreated but hosts not built (Explorer unavailable?) — will retry");
    }
}

Result<void> WallpaperManager::rebindLastFrames() {
    if (hosts_.empty()) {
        return {};
    }
    bool anyError = false;
    // Clone path: one shared frame texture on every host. NV12 software
    // frames rebind through the plane path (same as hardware); BGRA through
    // the texture path.
    if (frameIsNv12_ && nv12YSrv_ && nv12UvSrv_) {
        for (auto& host : hosts_) {
            auto result = host->setVideoPlanes(nv12YSrv_.Get(), nv12UvSrv_.Get(),
                                               frameDisplayAspect_, scaling_);
            if (!result) {
                anyError = true;
                log::Logger::instance().warn(L"rebind video planes failed for {}: {}",
                                             host->monitorId(), result.error());
            }
        }
    } else if (frameTextureSrv_) {
        for (auto& host : hosts_) {
            auto result =
                host->setVideoTexture(frameTextureSrv_.Get(), frameDisplayAspect_, scaling_);
            if (!result) {
                anyError = true;
                log::Logger::instance().warn(L"rebind video texture failed for {}: {}",
                                             host->monitorId(), result.error());
            }
        }
    }
    // Independent path: per-monitor textures on their own hosts, each with
    // ITS OWN aspect (different videos can have different aspects — using the
    // clone path's frameDisplayAspect_ here was wrong: 0 in Independent mode).
    for (auto& [monitorId, slot] : perMonitorFrames_) {
        auto hostIt = std::find_if(hosts_.begin(), hosts_.end(),
                                   [&](const auto& h) { return h->monitorId() == monitorId; });
        if (hostIt == hosts_.end()) {
            continue; // monitor gone — its host is recreated from monitors_
        }
        if (slot.nv12) {
            auto result = (*hostIt)->setVideoPlanes(slot.ySrv.Get(), slot.uvSrv.Get(),
                                                    slot.displayAspect, scaling_);
            if (!result) {
                anyError = true;
                log::Logger::instance().warn(L"rebind per-monitor planes failed for {}: {}",
                                             monitorId, result.error());
            }
        } else if (slot.srv) {
            auto result =
                (*hostIt)->setVideoTexture(slot.srv.Get(), slot.displayAspect, scaling_);
            if (!result) {
                anyError = true;
                log::Logger::instance().warn(L"rebind per-monitor texture failed for {}: {}",
                                             monitorId, result.error());
            }
        }
    }
    if (anyError) {
        return std::unexpected(L"one or more hosts failed to re-bind the video frame");
    }
    return {};
}

void WallpaperManager::shutdown() {
    if (!running_ && hosts_.empty()) {
        return;
    }
    running_ = false;
    teardownHosts();
    testTexture_.Reset();
    testTextureSrv_.Reset();
    frameTexture_.Reset();
    frameTextureSrv_.Reset();
    nv12YSrv_.Reset();
    nv12UvSrv_.Reset();
    frameIsNv12_ = false;
    planeSrvCache_.clear();
    frameWidth_ = 0;
    frameHeight_ = 0;
    layer_ = {};
    log::Logger::instance().debug(L"wallpaper shut down");
}

// ---- desktop discovery (docs/02 §2.6) -------------------------------------

Result<void> WallpaperManager::discoverDesktop() {
    layer_ = {};

    layer_.progman = ::FindWindowW(L"Progman", nullptr);
    if (!layer_.progman) {
        return std::unexpected(L"Progman not found — is Explorer running?");
    }

    // Ask Explorer to spawn the wallpaper WorkerW (arrangement-dependent; the
    // discovery below verifies what actually exists — never assume).
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(layer_.progman, kSpawnWorkerW, 0, 0,
                          SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &result);

    // Arrangement A (classic, most builds): SHELLDLL_DefView lives in a
    // top-level WorkerW; the wallpaper layer is the WorkerW below it.
    for (HWND w = ::FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); w;
         w = ::FindWindowExW(nullptr, w, L"WorkerW", nullptr)) {
        if (::FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
            layer_.iconLayer = w;
            break;
        }
    }
    if (layer_.iconLayer) {
        for (HWND w = ::FindWindowExW(nullptr, layer_.iconLayer, L"WorkerW", nullptr); w;
             w = ::FindWindowExW(nullptr, w, L"WorkerW", nullptr)) {
            if (!::FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
                layer_.wallpaperLayer = w;
                break;
            }
        }
    }

    // Arrangement B (this machine, Win11 24H2+ 26200): SHELLDLL_DefView stays
    // INSIDE Progman and 0x052C spawns a WorkerW as a CHILD of Progman at the
    // BOTTOM of its child z-order — that child is the wallpaper layer
    // (parenting to Progman itself would put the host ABOVE the icons).
    if (!layer_.wallpaperLayer) {
        // First WorkerW child of Progman (this build has exactly one — the
        // 0x052C spawn). Plain call: a one-iteration loop with an unconditional
        // break would trip C4702 (unreachable increment).
        layer_.wallpaperLayer =
            ::FindWindowExW(layer_.progman, nullptr, L"WorkerW", nullptr);
    }

    // Last resort: Progman directly (classic pre-8 layout with no WorkerW).
    if (!layer_.wallpaperLayer) {
        layer_.wallpaperLayer = layer_.progman;
    }

    if (layer_.iconLayer) {
        layer_.description =
            L"arrangement A: DefView in top-level WorkerW; wallpaper layer = WorkerW below it";
    } else if (layer_.wallpaperLayer != layer_.progman) {
        layer_.description =
            L"arrangement B: DefView inside Progman; wallpaper layer = Progman's child WorkerW";
    } else {
        layer_.description = L"no WorkerW — parenting to Progman directly (classic layout)";
    }

    logHierarchy();
    return {};
}

Result<WallpaperManager::FrameSnapshot> WallpaperManager::grabFrameSnapshot() const {
    // The bound frame: Independent mode keeps per-monitor upload textures;
    // Clone/the software path use the shared frameTexture_. Prefer the
    // primary monitor's texture (matches what the user sees on the main
    // display), then the shared one.
    ID3D11Texture2D* source = frameTexture_.Get();
    UINT width = frameWidth_;
    UINT height = frameHeight_;
    // The bound texture may be NV12 (B1 software path) or BGRA. NV12 readback
    // yields YUV bytes — converted to BGRA below with the same BT.709
    // limited->full-range math as the GPU shader (user-initiated, ONE frame;
    // the conversion cost is fine on this path). The hardware path's decoder
    // surfaces are transient (never retained) — no CPU copy exists there and
    // the caller sees "no preview" (documented in the header).
    bool nv12 = frameIsNv12_;
    if (!monitors_.empty()) {
        auto it = std::find_if(monitors_.begin(), monitors_.end(),
                               [](const monitors::MonitorInfo& m) { return m.primary; });
        const auto& id = it != monitors_.end() ? it->id : monitors_.front().id;
        const auto f = perMonitorFrames_.find(id);
        if (f != perMonitorFrames_.end() && f->second.texture) {
            source = f->second.texture.Get();
            width = f->second.width;
            height = f->second.height;
            nv12 = f->second.nv12;
        }
    }
    if (!source || width == 0 || height == 0) {
        return std::unexpected(L"no video frame has been presented yet");
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    source->GetDesc(&srcDesc);
    D3D11_TEXTURE2D_DESC stagingDesc = srcDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(deviceManager_.device()->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        return std::unexpected(L"preview staging texture creation failed");
    }
    ID3D11DeviceContext* ctx = deviceManager_.context();
    ctx->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return std::unexpected(L"preview readback failed");
    }
    FrameSnapshot out;
    out.width = width;
    out.height = height;
    out.bgra.resize(static_cast<size_t>(height) * static_cast<size_t>(width) * 4);
    const auto* src = static_cast<const BYTE*>(mapped.pData);
    if (nv12) {
        // NV12 layout in subresource 0: the Y plane (height rows of RowPitch)
        // followed immediately by the interleaved UV plane (height/2 rows of
        // RowPitch, one U/V pair per 2x2 luma block). Same math as PSMainYuv:
        // limited->full range (luma 16-235, chroma 16-240) then BT.709.
        const auto* uv = src + static_cast<size_t>(height) * mapped.RowPitch;
        const size_t rowPitch = static_cast<size_t>(mapped.RowPitch);
        for (UINT y = 0; y < height; ++y) {
            auto* dst = out.bgra.data() + static_cast<size_t>(y) * width * 4;
            const auto* yRow = src + static_cast<size_t>(y) * rowPitch;
            const auto* uvRow = uv + (static_cast<size_t>(y) / 2) * rowPitch;
            for (UINT x = 0; x < width; ++x) {
                const float yv = (static_cast<float>(yRow[x]) - 16.0f) * (255.0f / 219.0f);
                const float cb =
                    (static_cast<float>(uvRow[static_cast<size_t>(x / 2) * 2]) - 128.0f) *
                    (255.0f / 224.0f);
                const float cr =
                    (static_cast<float>(uvRow[static_cast<size_t>(x / 2) * 2 + 1]) - 128.0f) *
                    (255.0f / 224.0f);
                const float r = yv + 1.5748f * cr;
                const float g = yv - 0.1873f * cb - 0.4681f * cr;
                const float b = yv + 1.8556f * cb;
                dst[x * 4 + 0] = static_cast<BYTE>(std::clamp(b, 0.0f, 255.0f) + 0.5f);
                dst[x * 4 + 1] = static_cast<BYTE>(std::clamp(g, 0.0f, 255.0f) + 0.5f);
                dst[x * 4 + 2] = static_cast<BYTE>(std::clamp(r, 0.0f, 255.0f) + 0.5f);
                dst[x * 4 + 3] = 0xFF;
            }
        }
    } else {
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(out.bgra.data() + static_cast<size_t>(y) * width * 4,
                        src + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(width) * 4);
        }
    }
    ctx->Unmap(staging.Get(), 0);
    return out;
}

void WallpaperManager::logHierarchy() const {
    auto& log = log::Logger::instance();
    log.info(L"desktop hierarchy: Progman=0x{:X}, {}",
             reinterpret_cast<uintptr_t>(layer_.progman), layer_.description);
    log.debug(L"  iconLayer=0x{:X} wallpaperLayer=0x{:X}",
              reinterpret_cast<uintptr_t>(layer_.iconLayer),
              reinterpret_cast<uintptr_t>(layer_.wallpaperLayer));
}

// ---- test texture + hosts -------------------------------------------------

Result<void> WallpaperManager::ensureTestTexture() {
    if (testTextureSrv_) {
        return {};
    }
    auto texture = gfx::TextureManager::createTexture(
        deviceManager_.device(), DXGI_FORMAT_B8G8R8A8_UNORM, kTestTextureSize, kTestTextureSize,
        true /* dynamic */);
    if (!texture) {
        return std::unexpected(texture.error());
    }

    // Fill a black/white checkerboard (32 px cells) on the CPU.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(deviceManager_.context()->Map(texture->Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                             &mapped))) {
        return std::unexpected(L"checkerboard Map failed");
    }
    for (UINT y = 0; y < kTestTextureSize; ++y) {
        auto* row = static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < kTestTextureSize; ++x) {
            const bool black = ((x / kTestCellPx) + (y / kTestCellPx)) % 2 == 0;
            const BYTE v = black ? 0x00 : 0xFF;
            row[x * 4 + 0] = v; // B
            row[x * 4 + 1] = v; // G
            row[x * 4 + 2] = v; // R
            row[x * 4 + 3] = 0xFF;
        }
    }
    deviceManager_.context()->Unmap(texture->Get(), 0);

    auto srv = gfx::TextureManager::createSrv(deviceManager_.device(), texture->Get());
    if (!srv) {
        return std::unexpected(srv.error());
    }
    testTexture_ = std::move(*texture);
    testTextureSrv_ = std::move(*srv);
    log::Logger::instance().debug(L"checkerboard test texture created ({}x{})", kTestTextureSize,
                                  kTestTextureSize);
    return {};
}

Result<void> WallpaperManager::createHosts() {
    if (hosts_.empty() && monitors_.empty()) {
        return std::unexpected(L"no monitors to host");
    }
    std::wstring firstError;
    for (const auto& m : monitors_) {
        const bool alreadyHosted =
            std::any_of(hosts_.begin(), hosts_.end(),
                        [&](const auto& h) { return h->monitorId() == m.id; });
        if (alreadyHosted) {
            continue;
        }
        auto host = std::make_unique<WallpaperHost>();
        WallpaperHost::Options options{};
        options.parent = layer_.wallpaperLayer;
        options.bounds = m.bounds;
        options.monitorId = m.id;
        options.textureSrv = testTextureSrv_.Get();
        auto result = host->init(&deviceManager_, options);
        if (!result) {
            if (firstError.empty()) {
                firstError = result.error();
            }
            log::Logger::instance().warn(L"host creation failed for {}: {}", m.id, result.error());
            continue;
        }
        log::Logger::instance().info(L"host created for {} at {}x{} ({},{} - {},{})", m.id, m.width,
                                     m.height, m.bounds.left, m.bounds.top, m.bounds.right,
                                     m.bounds.bottom);
        hosts_.push_back(std::move(host));
    }
    if (hosts_.empty()) {
        return std::unexpected(firstError.empty() ? L"no hosts could be created" : firstError);
    }
    return {};
}

Result<void> WallpaperManager::renderAll() {
    bool anyError = false;
    for (auto& host : hosts_) {
        auto result = host->render();
        if (!result) {
            anyError = true;
            // M12: a device-lost failure is reported ONCE per loss event (the
            // recreate logs the sequence); per-frame spam while the 1 Hz tick
            // hasn't recovered the device yet is suppressed.
            if (host->lastRenderDeviceLost()) {
                if (!deviceLostLogged_) {
                    deviceLostLogged_ = true;
                    log::Logger::instance().warn(L"render failed for {}: {}", host->monitorId(),
                                                 result.error());
                }
            } else {
                log::Logger::instance().warn(L"render failed for {}: {}", host->monitorId(),
                                             result.error());
            }
        }
    }
    if (anyError) {
        return std::unexpected(L"one or more hosts failed to render");
    }
    return {};
}

namespace {
// M8: creates a B8G8R8A8 upload texture + SRV at the given size.
Result<std::pair<Microsoft::WRL::ComPtr<ID3D11Texture2D>,
                 Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>>
createUploadTexture(ID3D11Device* device, UINT width, UINT height) {
    auto texture = gfx::TextureManager::createTexture(device, DXGI_FORMAT_B8G8R8A8_UNORM, width,
                                                      height, true /* dynamic */);
    if (!texture) {
        return std::unexpected(texture.error());
    }
    auto srv = gfx::TextureManager::createSrv(device, texture->Get());
    if (!srv) {
        return std::unexpected(srv.error());
    }
    return std::make_pair(std::move(*texture), std::move(*srv));
}

// M8: copies tightly-packed B8G8R8A8 frame bytes into a dynamic texture.
Result<void> uploadFrameBytes(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                              const video::DecodedFrame& frame) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return std::unexpected(L"frame texture Map failed");
    }
    const auto* src = frame.bytes.data();
    const size_t rowBytes = static_cast<size_t>(frame.width) * 4;
    for (UINT y = 0; y < frame.height; ++y) {
        auto* dst = static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        std::memcpy(dst, src + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    context->Unmap(texture, 0);
    return {};
}

// B1: creates a dynamic NV12 upload texture + the Y/UV plane SRVs (R8 + R8G8
// views of the same texture — the GPU YUV shader converts + scales, no CPU
// color conversion).
Result<std::tuple<Microsoft::WRL::ComPtr<ID3D11Texture2D>,
                  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>,
                  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>>>
createNv12Upload(ID3D11Device* device, UINT width, UINT height) {
    auto texture = gfx::TextureManager::createTexture(device, DXGI_FORMAT_NV12, width, height,
                                                      true /* dynamic */);
    if (!texture) {
        return std::unexpected(texture.error());
    }
    auto ySrv = gfx::TextureManager::createPlaneSrv(device, texture->Get(), DXGI_FORMAT_R8_UNORM);
    if (!ySrv) {
        return std::unexpected(ySrv.error());
    }
    auto uvSrv =
        gfx::TextureManager::createPlaneSrv(device, texture->Get(), DXGI_FORMAT_R8G8_UNORM);
    if (!uvSrv) {
        return std::unexpected(uvSrv.error());
    }
    return std::make_tuple(std::move(*texture), std::move(*ySrv), std::move(*uvSrv));
}

// B1: copies tightly-packed NV12 bytes (Y plane w*h, then interleaved UV
// plane w*h/2) into a dynamic NV12 texture. Subresource 0 covers BOTH planes:
// the Y rows first, then the UV rows, each at the same RowPitch.
Result<void> uploadNv12Bytes(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                             const video::DecodedFrame& frame) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return std::unexpected(L"NV12 texture Map failed");
    }
    const auto* src = frame.bytes.data();
    const size_t rowBytes = static_cast<size_t>(frame.width);
    const size_t ySize = rowBytes * static_cast<size_t>(frame.height);
    auto* dst = static_cast<BYTE*>(mapped.pData);
    for (UINT y = 0; y < frame.height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                    src + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    // UV plane: right after the Y plane, height/2 rows of interleaved U/V.
    auto* uvDst = dst + static_cast<size_t>(frame.height) * mapped.RowPitch;
    const auto* uv = src + ySize;
    for (UINT y = 0; y < frame.height / 2; ++y) {
        std::memcpy(uvDst + static_cast<size_t>(y) * mapped.RowPitch,
                    uv + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    context->Unmap(texture, 0);
    return {};
}
} // namespace

Result<void> WallpaperManager::setVideoFrame(const video::DecodedFrame& frame) {
    if (!running_) {
        return {};
    }
    // DXGI shared handle path (D3D11VA zero-copy): the frame is a shared
    // texture from FFmpeg's private D3D11 device. Open it on the render
    // device and bind directly — no CPU upload.
    if (frame.hardware && frame.sharedHandle) {
        return bindSharedFrame(frame);
    }
    // Hardware path (M5): the frame IS a GPU surface — view its two planes
    // (Y + interleaved UV) and rebind the hosts. No CPU upload.
    // FFmpeg HW decode: hardware=true but texture=null (CPU-transferred NV12).
    if (frame.hardware && frame.texture) {
        return bindGpuFrame(frame);
    }
    if (frame.endOfStream || frame.bytes.empty()) {
        // EOS/empty sentinel: keep the last presented frame on screen (the
        // player loops by reopening; nothing to upload here).
        return {};
    }
    if (frame.width == 0 || frame.height == 0) {
        return std::unexpected(L"setVideoFrame: invalid frame size");
    }

    // NV12 software path (B1): one NV12 texture + plane SRVs; the GPU YUV
    // shader converts + scales (1.5 vs 4 B/px upload, no CPU color
    // conversion). Same bind path as hardware frames.
    if (frame.nv12) {
        if (!frameTexture_ || !frameIsNv12_ || frameWidth_ != frame.width ||
            frameHeight_ != frame.height) {
            auto created = createNv12Upload(deviceManager_.device(), frame.width, frame.height);
            if (!created) {
                return std::unexpected(created.error());
            }
            frameTexture_ = std::move(std::get<0>(*created));
            nv12YSrv_ = std::move(std::get<1>(*created));
            nv12UvSrv_ = std::move(std::get<2>(*created));
            frameTextureSrv_.Reset(); // BGRA view is stale for an NV12 texture
            frameIsNv12_ = true;
            frameWidth_ = frame.width;
            frameHeight_ = frame.height;
            log::Logger::instance().debug(L"video NV12 upload texture (re)created: {}x{}",
                                          frame.width, frame.height);
        }
        auto uploaded = uploadNv12Bytes(deviceManager_.context(), frameTexture_.Get(), frame);
        if (!uploaded) {
            return uploaded;
        }
        // Display aspect (SAR-corrected) for the scaling math — the clone
        // path binds one frame on every host, so remember it for the rebind.
        frameDisplayAspect_ = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
        return bindFramePlanes(nv12YSrv_.Get(), nv12UvSrv_.Get(), frameDisplayAspect_);
    }

    // (Re)create the upload texture + SRV when the frame size changes (loop
    // across different-resolution clips) OR the slot switched from NV12
    // (same-resolution NV12 -> RGB32 clip must not reuse the NV12 texture).
    if (!frameTexture_ || frameIsNv12_ || frameWidth_ != frame.width ||
        frameHeight_ != frame.height) {
        auto created = createUploadTexture(deviceManager_.device(), frame.width, frame.height);
        if (!created) {
            return std::unexpected(created.error());
        }
        frameTexture_ = std::move(created->first);
        frameTextureSrv_ = std::move(created->second);
        nv12YSrv_.Reset();
        nv12UvSrv_.Reset();
        frameIsNv12_ = false;
        frameWidth_ = frame.width;
        frameHeight_ = frame.height;
        log::Logger::instance().debug(L"video upload texture (re)created: {}x{}", frame.width,
                                      frame.height);
    }

    auto uploaded = uploadFrameBytes(deviceManager_.context(), frameTexture_.Get(), frame);
    if (!uploaded) {
        return uploaded;
    }
    // Display aspect (SAR-corrected) for the scaling math — the clone path
    // binds one frame on every host, so remember it for the rebind.
    frameDisplayAspect_ = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return bindFrameTexture();
}

Result<void> WallpaperManager::setVideoFrameFor(const std::wstring& monitorId,
                                                const video::DecodedFrame& frame) {
    if (!running_) {
        return {};
    }
    auto hostIt = std::find_if(hosts_.begin(), hosts_.end(),
                               [&](const auto& h) { return h->monitorId() == monitorId; });
    if (hostIt == hosts_.end()) {
        return {}; // unknown monitor (e.g. unplugged between events) — no-op
    }
    // DXGI shared handle path (D3D11VA zero-copy).
    if (frame.hardware && frame.sharedHandle) {
        return bindSharedFrameFor(monitorId, frame);
    }
    // Hardware path (M5): GPU surface — bind planes on THAT host only.
    // But FFmpeg HW decode transfers to CPU NV12 (hardware=true, texture=null)
    // so fall through to the NV12 software upload path in that case.
    if (frame.hardware && frame.texture) {
        return bindGpuFrameFor(monitorId, frame);
    }
    if (frame.endOfStream || frame.bytes.empty()) {
        return {}; // EOS sentinel: keep the last presented frame
    }
    if (frame.width == 0 || frame.height == 0) {
        return std::unexpected(L"setVideoFrameFor: invalid frame size");
    }

    // NV12 software path (B1): per-monitor NV12 texture + plane SRVs, bound
    // through the same plane path as hardware frames.
    if (frame.nv12) {
        auto& slot = perMonitorFrames_[monitorId];
        if (!slot.texture || !slot.nv12 || slot.width != frame.width ||
            slot.height != frame.height) {
            auto created = createNv12Upload(deviceManager_.device(), frame.width, frame.height);
            if (!created) {
                return std::unexpected(created.error());
            }
            slot.texture = std::move(std::get<0>(*created));
            slot.ySrv = std::move(std::get<1>(*created));
            slot.uvSrv = std::move(std::get<2>(*created));
            slot.srv.Reset(); // BGRA view is stale for an NV12 texture
            slot.nv12 = true;
            slot.width = frame.width;
            slot.height = frame.height;
            log::Logger::instance().debug(L"per-monitor NV12 texture (re)created: {} ({}x{})",
                                          monitorId, frame.width, frame.height);
        }
        auto uploaded = uploadNv12Bytes(deviceManager_.context(), slot.texture.Get(), frame);
        if (!uploaded) {
            return uploaded;
        }
        slot.displayAspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
        return (*hostIt)->setVideoPlanes(slot.ySrv.Get(), slot.uvSrv.Get(), slot.displayAspect,
                                         scaling_);
    }

    // Per-monitor upload texture, recreated on size change OR format switch.
    auto& slot = perMonitorFrames_[monitorId];
    if (!slot.texture || slot.nv12 || slot.width != frame.width || slot.height != frame.height) {
        auto created = createUploadTexture(deviceManager_.device(), frame.width, frame.height);
        if (!created) {
            return std::unexpected(created.error());
        }
        slot.texture = std::move(created->first);
        slot.srv = std::move(created->second);
        slot.ySrv.Reset();
        slot.uvSrv.Reset();
        slot.nv12 = false;
        slot.width = frame.width;
        slot.height = frame.height;
        log::Logger::instance().debug(L"per-monitor video texture (re)created: {} ({}x{})",
                                      monitorId, frame.width, frame.height);
    }
    auto uploaded = uploadFrameBytes(deviceManager_.context(), slot.texture.Get(), frame);
    if (!uploaded) {
        return uploaded;
    }
    // Remember THIS frame's display aspect for the M12 rebind path (each
    // monitor can run a different-resolution video with its own aspect).
    slot.displayAspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return (*hostIt)->setVideoTexture(slot.srv.Get(), slot.displayAspect, scaling_);
}

Result<void> WallpaperManager::bindFrameTexture() {
    bool anyError = false;
    for (auto& host : hosts_) {
        auto result = host->setVideoTexture(frameTextureSrv_.Get(), frameDisplayAspect_, scaling_);
        if (!result) {
            anyError = true;
            log::Logger::instance().warn(L"bind video texture failed for {}: {}",
                                         host->monitorId(), result.error());
        }
    }
    if (anyError) {
        return std::unexpected(L"one or more hosts failed to bind the video texture");
    }
    return {};
}

Result<void> WallpaperManager::bindGpuFrame(const video::DecodedFrame& frame) {
    if (!frame.texture) {
        return std::unexpected(L"bindGpuFrame: no texture");
    }
    // Plane views for the decoder's planar surface (NV12 -> R8/R8G8,
    // P010 -> R16/R16G16), per the documented two-views pattern.
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    DXGI_FORMAT yFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uvFormat = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
        case DXGI_FORMAT_NV12:
            yFormat = DXGI_FORMAT_R8_UNORM;
            uvFormat = DXGI_FORMAT_R8G8_UNORM;
            break;
        case DXGI_FORMAT_P010:
            yFormat = DXGI_FORMAT_R16_UNORM;
            uvFormat = DXGI_FORMAT_R16G16_UNORM;
            break;
        default:
            return std::unexpected(L"bindGpuFrame: unsupported surface format " +
                                   std::to_wstring(static_cast<int>(desc.Format)));
    }
    // B3: cache the plane views per (texture, slice) pair. D3D11VA textures
    // are arrays — the same texture pointer recurs across frames with
    // different array slices. Key by (ptr, slice) so each slice gets its own
    // cached views. Bounded: pooled surfaces keep the map tiny.
    const PlaneSrvKey key{frame.texture.Get(), frame.textureSlice};
    auto& entry = planeSrvCache_[key];
    if (!entry.y) {
        if (planeSrvCache_.size() > kMaxPlaneSrvCacheEntries) {
            planeSrvCache_.clear();
            entry = planeSrvCache_[key];
        }
        auto ySrv = gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                        yFormat, frame.textureSlice);
        if (!ySrv) {
            return std::unexpected(ySrv.error());
        }
        auto uvSrv =
            gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                uvFormat, frame.textureSlice);
        if (!uvSrv) {
            return std::unexpected(uvSrv.error());
        }
        entry.y = std::move(*ySrv);
        entry.uv = std::move(*uvSrv);
    }
    const float aspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return bindFramePlanes(entry.y.Get(), entry.uv.Get(), aspect);
}

Result<void> WallpaperManager::bindGpuFrameFor(const std::wstring& monitorId,
                                               const video::DecodedFrame& frame) {
    if (!frame.texture) {
        return std::unexpected(L"bindGpuFrameFor: no texture");
    }
    auto hostIt = std::find_if(hosts_.begin(), hosts_.end(),
                               [&](const auto& h) { return h->monitorId() == monitorId; });
    if (hostIt == hosts_.end()) {
        return {}; // unknown monitor — no-op
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    DXGI_FORMAT yFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT uvFormat = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
        case DXGI_FORMAT_NV12:
            yFormat = DXGI_FORMAT_R8_UNORM;
            uvFormat = DXGI_FORMAT_R8G8_UNORM;
            break;
        case DXGI_FORMAT_P010:
            yFormat = DXGI_FORMAT_R16_UNORM;
            uvFormat = DXGI_FORMAT_R16G16_UNORM;
            break;
        default:
            return std::unexpected(L"bindGpuFrameFor: unsupported surface format " +
                                   std::to_wstring(static_cast<int>(desc.Format)));
    }
    // B3: cached plane views per (texture, slice) — see bindGpuFrame.
    const PlaneSrvKey key{frame.texture.Get(), frame.textureSlice};
    auto& entry = planeSrvCache_[key];
    if (!entry.y) {
        if (planeSrvCache_.size() > kMaxPlaneSrvCacheEntries) {
            planeSrvCache_.clear();
            entry = planeSrvCache_[key];
        }
        auto ySrv = gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                        yFormat, frame.textureSlice);
        if (!ySrv) {
            return std::unexpected(ySrv.error());
        }
        auto uvSrv =
            gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                uvFormat, frame.textureSlice);
        if (!uvSrv) {
            return std::unexpected(uvSrv.error());
        }
        entry.y = std::move(*ySrv);
        entry.uv = std::move(*uvSrv);
    }
    const float aspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return (*hostIt)->setVideoPlanes(entry.y.Get(), entry.uv.Get(), aspect, scaling_);
}

// ---- D3D11VA shared-handle zero-copy path ----
// D3D11VA shared-handle zero-copy path.
// Opens a DXGI shared texture on the render device, caching it for reuse.
Microsoft::WRL::ComPtr<ID3D11Texture2D> WallpaperManager::openSharedHandle(HANDLE sharedHandle) {
    auto& entry = sharedFrameCache_[sharedHandle];
    if (entry.texture) return entry.texture;
    Microsoft::WRL::ComPtr<ID3D11Device1> dev1;
    HRESULT hr = deviceManager_.device()->QueryInterface(IID_PPV_ARGS(&dev1));
    if (FAILED(hr) || !dev1) return nullptr;
    ID3D11Texture2D* rawTex = nullptr;
    hr = dev1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&rawTex));
    if (FAILED(hr) || !rawTex) return nullptr;
    entry.texture.Attach(rawTex);
    entry.handle = sharedHandle;
    return entry.texture;
}

Result<void> WallpaperManager::bindSharedFrame(const video::DecodedFrame& frame) {
    auto tex = openSharedHandle(frame.sharedHandle);
    if (!tex) return std::unexpected(L"bindSharedFrame: OpenSharedResource1 failed");
    video::DecodedFrame proxy;
    proxy.texture = tex;
    proxy.hardware = true;
    proxy.width = frame.width;
    proxy.height = frame.height;
    proxy.displayAspect = frame.displayAspect;
    proxy.textureSlice = frame.textureSlice;
    return bindGpuFrame(proxy);
}

Result<void> WallpaperManager::bindSharedFrameFor(const std::wstring& monitorId,
                                                  const video::DecodedFrame& frame) {
    auto tex = openSharedHandle(frame.sharedHandle);
    if (!tex) return std::unexpected(L"bindSharedFrameFor: OpenSharedResource1 failed");
    video::DecodedFrame proxy;
    proxy.texture = tex;
    proxy.hardware = true;
    proxy.width = frame.width;
    proxy.height = frame.height;
    proxy.displayAspect = frame.displayAspect;
    proxy.textureSlice = frame.textureSlice;
    return bindGpuFrameFor(monitorId, proxy);
}

Result<void> WallpaperManager::bindFramePlanes(ID3D11ShaderResourceView* ySrv,
                                               ID3D11ShaderResourceView* uvSrv,
                                               float videoAspect) {
    bool anyError = false;
    for (auto& host : hosts_) {
        auto result = host->setVideoPlanes(ySrv, uvSrv, videoAspect, scaling_);
        if (!result) {
            anyError = true;
            log::Logger::instance().warn(L"bind video planes failed for {}: {}",
                                         host->monitorId(), result.error());
        }
    }
    if (anyError) {
        return std::unexpected(L"one or more hosts failed to bind the video planes");
    }
    return {};
}

void WallpaperManager::teardownHosts() {
    for (auto& host : hosts_) {
        host->destroy();
    }
    hosts_.clear();
    perMonitorFrames_.clear(); // M8: independent-path textures die with hosts
}

// ---- monitor event handlers -----------------------------------------------

void WallpaperManager::addHostFor(const std::wstring& monitorId) {
    if (!running_ || !layer_.wallpaperLayer || !::IsWindow(layer_.wallpaperLayer)) {
        return; // tick rebuild handles it once the layer is valid
    }
    const auto it = std::find_if(monitors_.begin(), monitors_.end(),
                                 [&](const monitors::MonitorInfo& m) { return m.id == monitorId; });
    if (it == monitors_.end()) {
        return;
    }
    auto host = std::make_unique<WallpaperHost>();
    WallpaperHost::Options options{};
    options.parent = layer_.wallpaperLayer;
    options.bounds = it->bounds;
    options.monitorId = it->id;
    options.textureSrv = testTextureSrv_.Get();
    auto result = host->init(&deviceManager_, options);
    if (!result) {
        log::Logger::instance().warn(L"host creation failed for {}: {}", monitorId, result.error());
        return;
    }
    if (auto rendered = host->render(); !rendered) {
        log::Logger::instance().warn(L"render failed for {}: {}", monitorId, rendered.error());
    }
    hosts_.push_back(std::move(host));
}

void WallpaperManager::removeHostFor(const std::wstring& monitorId) {
    auto it = std::find_if(hosts_.begin(), hosts_.end(),
                           [&](const auto& h) { return h->monitorId() == monitorId; });
    if (it != hosts_.end()) {
        (*it)->destroy();
        hosts_.erase(it);
    }
    // M8: drop the per-monitor upload texture on disconnect (no leak — the
    // host's swap chain + textures are released with it).
    perMonitorFrames_.erase(monitorId);
}

void WallpaperManager::repositionHost(const std::wstring& monitorId) {
    auto hostIt = std::find_if(hosts_.begin(), hosts_.end(),
                               [&](const auto& h) { return h->monitorId() == monitorId; });
    if (hostIt == hosts_.end()) {
        addHostFor(monitorId);
        return;
    }
    const auto monIt = std::find_if(monitors_.begin(), monitors_.end(),
                                    [&](const monitors::MonitorInfo& m) { return m.id == monitorId; });
    if (monIt == monitors_.end()) {
        return;
    }
    auto result = (*hostIt)->setBounds(monIt->bounds);
    if (!result) {
        log::Logger::instance().warn(L"reposition failed for {}: {}", monitorId, result.error());
    }
}

} // namespace vw::wallpaper
