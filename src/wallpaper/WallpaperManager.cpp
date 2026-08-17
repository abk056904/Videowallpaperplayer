#include "wallpaper/WallpaperManager.h"

#include <algorithm>
#include <cstring>
#include <string>
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

void WallpaperManager::onTick() {
    if (!running_) {
        return;
    }

    // Explorer-restart stub (docs/02 §2.6, full logic M12): ~1 Hz check that
    // the wallpaper layer and every host window still exist.
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
            if (auto rendered = renderAll(); !rendered) {
                log::Logger::instance().warn(L"rebuild render incomplete: {}", rendered.error());
            }
        }
    }
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
            log::Logger::instance().warn(L"render failed for {}: {}", host->monitorId(),
                                         result.error());
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
} // namespace

Result<void> WallpaperManager::setVideoFrame(const video::DecodedFrame& frame) {
    if (!running_) {
        return {};
    }
    // Hardware path (M5): the frame IS a GPU surface — view its two planes
    // (Y + interleaved UV) and rebind the hosts. No CPU upload.
    if (frame.hardware) {
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

    // (Re)create the upload texture + SRV when the frame size changes (loop
    // across different-resolution clips).
    if (!frameTexture_ || frameWidth_ != frame.width || frameHeight_ != frame.height) {
        auto created = createUploadTexture(deviceManager_.device(), frame.width, frame.height);
        if (!created) {
            return std::unexpected(created.error());
        }
        frameTexture_ = std::move(created->first);
        frameTextureSrv_ = std::move(created->second);
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
    // Hardware path (M5): GPU surface — bind planes on THAT host only.
    if (frame.hardware) {
        return bindGpuFrameFor(monitorId, frame);
    }
    if (frame.endOfStream || frame.bytes.empty()) {
        return {}; // EOS sentinel: keep the last presented frame
    }
    if (frame.width == 0 || frame.height == 0) {
        return std::unexpected(L"setVideoFrameFor: invalid frame size");
    }

    // Per-monitor upload texture, recreated on size change.
    auto& slot = perMonitorFrames_[monitorId];
    if (!slot.texture || slot.width != frame.width || slot.height != frame.height) {
        auto created = createUploadTexture(deviceManager_.device(), frame.width, frame.height);
        if (!created) {
            return std::unexpected(created.error());
        }
        slot.texture = std::move(created->first);
        slot.srv = std::move(created->second);
        slot.width = frame.width;
        slot.height = frame.height;
        log::Logger::instance().debug(L"per-monitor video texture (re)created: {} ({}x{})",
                                      monitorId, frame.width, frame.height);
    }
    auto uploaded = uploadFrameBytes(deviceManager_.context(), slot.texture.Get(), frame);
    if (!uploaded) {
        return uploaded;
    }
    const float aspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return (*hostIt)->setVideoTexture(slot.srv.Get(), aspect, scaling_);
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
    auto ySrv = gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                    yFormat);
    if (!ySrv) {
        return std::unexpected(ySrv.error());
    }
    auto uvSrv = gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                     uvFormat);
    if (!uvSrv) {
        return std::unexpected(uvSrv.error());
    }
    const float aspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return bindFramePlanes(ySrv->Get(), uvSrv->Get(), aspect);
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
    auto ySrv = gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                    yFormat);
    if (!ySrv) {
        return std::unexpected(ySrv.error());
    }
    auto uvSrv = gfx::TextureManager::createPlaneSrv(deviceManager_.device(), frame.texture.Get(),
                                                     uvFormat);
    if (!uvSrv) {
        return std::unexpected(uvSrv.error());
    }
    const float aspect = gfx::videoAspectFor(frame.width, frame.height, frame.displayAspect);
    return (*hostIt)->setVideoPlanes(ySrv->Get(), uvSrv->Get(), aspect, scaling_);
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
