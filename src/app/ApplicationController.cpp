#include "app/ApplicationController.h"

#include <windows.h>
#include <mfapi.h>
#include <shlobj.h>

#include <string>

#include "logging/Logger.h"
#include "util/clock.h"
#include "video/VideoPlayer.h"

namespace vw::app {

namespace {
const wchar_t* kSingleInstanceMutex = L"Local\\VideoWallpaper.SingleInstance";

// config.playback.scaling -> renderer scaling (Fill default).
gfx::D3D11Renderer::Scaling rendererScalingFrom(config::ScalingMode m) {
    switch (m) {
        case config::ScalingMode::Fit: return gfx::D3D11Renderer::Scaling::Fit;
        case config::ScalingMode::Stretch: return gfx::D3D11Renderer::Scaling::Stretch;
        case config::ScalingMode::Center: return gfx::D3D11Renderer::Scaling::Center;
        case config::ScalingMode::Fill: return gfx::D3D11Renderer::Scaling::Fill;
    }
    return gfx::D3D11Renderer::Scaling::Fill;
}
} // namespace

ApplicationController::ApplicationController() = default;
ApplicationController::~ApplicationController() = default;

bool ApplicationController::acquireSingleInstance() {
    mutex_ = ::CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    return mutex_ != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS;
}

void ApplicationController::notifyExistingInstance() const {
    // Ask the running instance to bring itself forward, then exit silently.
    const HWND existing = ::FindWindowW(ControlWindow::kClassName, nullptr);
    if (existing) {
        ::PostMessageW(existing, ControlWindow::focusMessage(), 0, 0);
    }
}

void ApplicationController::initPaths() {
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        appDataDir_ = std::filesystem::path(buf) / L"VideoWallpaper";
    } else {
        // Never write next to the executable (plan rule): use the temp dir as
        // the last-resort fallback instead of the working directory.
        appDataDir_ = std::filesystem::temp_directory_path() / L"VideoWallpaper";
    }
}

int ApplicationController::run() {
    // Per-monitor DPI awareness so monitor bounds are physical pixels.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!acquireSingleInstance()) {
        notifyExistingInstance();
        return 0;
    }

    initPaths();
    auto& log = log::Logger::instance();
    log.init(log::Logger::Options{
        .logDir = appDataDir_ / L"logs",
#ifdef VW_DEBUG
        .level = log::Level::Debug,
#else
        .level = log::Level::Info,
#endif
        .maxFileBytes = 5ull * 1024 * 1024,
    });

    config_ = std::make_unique<config::ConfigurationManager>(
        config::ConfigurationManager::Options{appDataDir_ / L"config.json"});
    const bool configLoaded = config_->load();
    if (!configLoaded) {
        log.warn(L"config load failed: {}", config_->lastError());
    } else if (!config_->lastError().empty()) {
        log.warn(L"config: {}", config_->lastError());
    }

    control_.setHandler([this](UINT msg, WPARAM wParam, LPARAM) {
        auto& log = log::Logger::instance();
        if (msg == ControlWindow::focusMessage()) {
            log.info(L"second instance requested focus (UI arrives in M11)");
        } else if (msg == WM_TIMER && wParam == kWallpaperTimerId && wallpaper_) {
            wallpaper_->onTick(); // Explorer-restart validity stub (~1 Hz)
        } else if (msg == WM_TIMER && wParam == kFrameTimerId) {
            onFrameTick(); // M4: decode -> upload -> present at ~vsync cadence
        } else if (msg == ControlWindow::pauseMessage()) {
            // VideoPlayer logs the pause position.
            if (player_ && player_->state() == video::VideoPlayer::State::Playing) {
                player_->pause();
                ::KillTimer(control_.handle(), kFrameTimerId);
            }
        } else if (msg == ControlWindow::resumeMessage()) {
            if (player_ && player_->state() == video::VideoPlayer::State::Paused) {
                if (auto r = player_->resume(); r) {
                    ::SetTimer(control_.handle(), kFrameTimerId, 16, nullptr);
                } else {
                    log.warn(L"resume failed: {}", r.error());
                }
            }
        } else if (msg == ControlWindow::stopMessage()) {
            if (player_) {
                player_->stop();
                ::KillTimer(control_.handle(), kFrameTimerId);
            }
        } else if ((msg == WM_DISPLAYCHANGE || msg == WM_DEVICECHANGE) && wallpaper_) {
            wallpaper_->onDisplayChange();
        }
    });

    if (!control_.create()) {
        log.error(L"failed to create control window");
        shutdown();
        return 1;
    }

    // M3: wallpaper behind desktop icons.
    wallpaper_ = std::make_unique<wallpaper::WallpaperManager>();
    const auto wallpaperResult = wallpaper_->start();
    if (!wallpaperResult) {
        log.error(L"wallpaper start failed: {}", wallpaperResult.error());
    } else {
        // 1 Hz validity check — only while the wallpaper runs (docs/02 §2.8).
        ::SetTimer(control_.handle(), kWallpaperTimerId, 1000, nullptr);
    }

    // M4: Media Foundation single-video playback (docs/03 §3.6).
    const HRESULT mf = ::MFStartup(MF_VERSION);
    if (FAILED(mf)) {
        log.error(L"MFStartup failed: 0x{:08X}", static_cast<unsigned>(mf));
    } else {
        mfStarted_ = true;
        startPlayback();
    }

    log.info(L"Video Wallpaper v{} starting", L"0.1.0");
    log.info(L"appdata dir: {}", appDataDir_.wstring());
    log.info(L"config: {} (loaded={})", config_->lastError().empty() ? L"ok" : config_->lastError(),
             configLoaded ? L"yes" : L"no");
    log.debug(L"clock frequency: {:.0f} Hz", 1.0 / util::Clock::instance().ticksToSeconds(1));

    // Message loop: blocks when idle (zero busy-wait).
    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    shutdown();
    return 0;
}

void ApplicationController::startPlayback() {
    auto& log = log::Logger::instance();
    if (!config_ || config_->config().videoPath.empty()) {
        log.info(L"no playback.videoPath configured — wallpaper shows the test texture");
        return;
    }
    const std::wstring path = config_->config().videoPath;
    player_ = std::make_unique<video::VideoPlayer>();
    // M5: hardware decode on the wallpaper's D3D device (same adapter);
    // scaling per config.playback.scaling (Fill default).
    if (wallpaper_) {
        wallpaper_->setScaling(rendererScalingFrom(config_->config().scaling));
        player_->setD3DDevice(wallpaper_->device());
    }
    auto opened = player_->open(path);
    if (!opened) {
        log.warn(L"cannot open video '{}': {}", path, opened.error());
        player_.reset();
        return;
    }
    // VideoPlayer logs the full open + start diagnostics.
    if (auto started = player_->start(); !started) {
        log.warn(L"playback start failed: {}", started.error());
        player_.reset();
        return;
    }
    // ~60 Hz frame pump (docs/03 §3.6): each tick pulls the newest decoded
    // frame, uploads it, and presents (vsync-blocked). M6 replaces this with
    // the FrameScheduler.
    ::SetTimer(control_.handle(), kFrameTimerId, 16, nullptr);
}

void ApplicationController::onFrameTick() {
    auto& log = log::Logger::instance();
    if (!player_ || !wallpaper_) {
        return;
    }
    if (player_->state() != video::VideoPlayer::State::Playing) {
        return; // paused/stopped: nothing to pump (timer is killed on pause)
    }

    // Drain whatever the worker decoded (usually 0-1 frames at source rate),
    // upload the newest, and present once per tick (vsync-paced).
    video::DecodedFrame frame;
    while (player_->pollFrame(frame)) {
        if (frame.endOfStream) {
            log.info(L"video stream ended — stopping playback");
            ::KillTimer(control_.handle(), kFrameTimerId);
            player_->stop(); // last frame stays on screen (loop/next is M7)
            break;
        }
        if (auto set = wallpaper_->setVideoFrame(frame); !set) {
            log.warn(L"video frame upload failed: {}", set.error());
        }
    }

    if (auto rendered = wallpaper_->renderAll(); !rendered) {
        log.warn(L"frame render failed: {}", rendered.error());
    }
}

void ApplicationController::shutdown() {
    // docs/03 §3.17: stop workers -> stop rendering -> release GPU resources
    // -> MFShutdown -> save state -> close window -> flush logs -> exit.
    ::KillTimer(control_.handle(), kWallpaperTimerId);
    ::KillTimer(control_.handle(), kFrameTimerId);
    if (player_) {
        player_->stop(); // joins the decode worker (no MF use afterwards)
        player_.reset();
    }
    if (wallpaper_) {
        wallpaper_->shutdown(); // tear down hosts + release GPU resources
        wallpaper_.reset();
    }
    if (mfStarted_) {
        ::MFShutdown();
        mfStarted_ = false;
    }
    if (config_) config_->save();
    control_.destroy(); // destroys the window
    log::Logger::instance().flush();
    if (mutex_) {
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    log::Logger::instance().info(L"shutdown complete");
}

} // namespace vw::app
