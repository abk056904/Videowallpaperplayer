#include "app/ApplicationController.h"

#include <windows.h>
#include <mfapi.h>
#include <shlobj.h>

#include <string>

#include "logging/Logger.h"
#include "playback/PlaybackController.h"
#include "util/clock.h"
#include "video/DecodedFrame.h"

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
        } else if (msg == ControlWindow::pauseMessage()) {
            // PlaybackController logs the pause position + cancels the timer.
            if (playback_ && playback_->state() == playback::PlaybackController::State::Playing) {
                playback_->pause();
            }
        } else if (msg == ControlWindow::resumeMessage()) {
            if (playback_ && playback_->state() == playback::PlaybackController::State::Paused) {
                if (auto r = playback_->resume(); !r) {
                    log.warn(L"resume failed: {}", r.error());
                }
            }
        } else if (msg == ControlWindow::stopMessage()) {
            if (playback_) {
                playback_->stop();
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

    // Message loop: blocks when idle (zero busy-wait, docs/02 §2.8). While
    // playback runs, wait on the FrameScheduler's waitable timer + the
    // decoder's new-frame event alongside messages (M6); anything else waits
    // on messages alone.
    MSG msg{};
    bool running = true;
    while (running) {
        HANDLE waits[2] = {};
        DWORD waitCount = 0;
        if (playback_ && playback_->state() == playback::PlaybackController::State::Playing) {
            // Both handles are valid while Playing; if one were ever null, fall
            // back to messages-only rather than WAIT_FAILED busy-spinning on a
            // null handle in the array.
            const HANDLE timer = playback_->timerHandle();
            const HANDLE event = playback_->newFrameEvent();
            if (timer && event) {
                waits[0] = timer;
                waits[1] = event;
                waitCount = 2;
            }
        }
        const DWORD waitResult = ::MsgWaitForMultipleObjects(waitCount, waits, FALSE, INFINITE,
                                                             QS_ALLINPUT);
        if (waitCount > 0 && (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_OBJECT_0 + 1)) {
            onFrameWake(); // deadline fired or a new frame arrived
        }
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
        }
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
    playback_ = std::make_unique<playback::PlaybackController>();
    // M5: hardware decode on the wallpaper's D3D device (same adapter);
    // scaling per config.playback.scaling (Fill default).
    if (wallpaper_) {
        wallpaper_->setScaling(rendererScalingFrom(config_->config().scaling));
    }
    auto opened =
        playback_->open(path, wallpaper_ ? wallpaper_->device() : nullptr,
                        static_cast<size_t>(config_->config().frameQueue));
    if (!opened) {
        log.warn(L"cannot open video '{}': {}", path, opened.error());
        playback_.reset();
        return;
    }
    // PlaybackController logs the full open + start diagnostics.
    if (auto started = playback_->start(); !started) {
        log.warn(L"playback start failed: {}", started.error());
        playback_.reset();
        return;
    }
    // M6: no frame timer — the message loop waits on the controller's
    // waitable timer + new-frame event (source-FPS pacing, zero busy-wait).
}

void ApplicationController::onFrameWake() {
    auto& log = log::Logger::instance();
    if (!playback_ || !wallpaper_) {
        return;
    }
    // Scheduler-gated drain: only present when a new frame is actually due
    // (no redraw of static frames — the idle wake re-arms the timer only).
    auto frame = playback_->onWake();
    if (!frame) {
        return;
    }
    if (frame->endOfStream) {
        log.info(L"video stream ended — stopping playback");
        playback_->stop(); // last frame stays on screen (loop/next is M7)
        return;
    }
    if (auto set = wallpaper_->setVideoFrame(*frame); !set) {
        log.warn(L"video frame upload failed: {}", set.error());
    }
    const LONGLONG renderStart = util::Clock::instance().now100ns();
    if (auto rendered = wallpaper_->renderAll(); !rendered) {
        log.warn(L"frame render failed: {}", rendered.error());
    }
    playback_->noteRenderTime(
        static_cast<double>(util::Clock::instance().now100ns() - renderStart) / 10000.0);
}

void ApplicationController::shutdown() {
    // docs/03 §3.17: stop workers -> stop rendering -> release GPU resources
    // -> MFShutdown -> save state -> close window -> flush logs -> exit.
    ::KillTimer(control_.handle(), kWallpaperTimerId);
    if (playback_) {
        playback_->stop(); // joins the decode worker (no MF use afterwards)
        playback_.reset();
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
