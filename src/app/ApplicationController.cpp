#include "app/ApplicationController.h"

#include <windows.h>
#include <shlobj.h>

#include <string>

#include "logging/Logger.h"
#include "util/clock.h"

namespace vw::app {

namespace {
const wchar_t* kSingleInstanceMutex = L"Local\\VideoWallpaper.SingleInstance";
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

    control_.setHandler([this](UINT msg, WPARAM, LPARAM) {
        auto& log = log::Logger::instance();
        if (msg == ControlWindow::focusMessage()) {
            log.info(L"second instance requested focus (UI arrives in M11)");
        } else if (msg == WM_TIMER && wallpaper_) {
            wallpaper_->onTick(); // Explorer-restart validity stub (~1 Hz)
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

void ApplicationController::shutdown() {
    // docs/03 §3.17 (M1 subset): stop accepting commands -> save state -> close
    // window -> flush logs -> release handles -> exit.
    if (config_) config_->save();
    if (wallpaper_) {
        ::KillTimer(control_.handle(), kWallpaperTimerId);
        wallpaper_->shutdown(); // tear down hosts BEFORE the pump dies
        wallpaper_.reset();
    }
    control_.destroy(); // destroys the window
    log::Logger::instance().flush();
    if (mutex_) {
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    log::Logger::instance().info(L"shutdown complete");
}

} // namespace vw::app
