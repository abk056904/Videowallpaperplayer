#include "app/ApplicationController.h"

#include <windows.h>
#include <mfapi.h>
#include <shlobj.h>

#include <chrono>
#include <string>

#include "detection/FullscreenDetector.h"
#include "logging/Logger.h"
#include "playback/PlaybackController.h"
#include "playlist/PlaylistStore.h"
#include "util/clock.h"
#include "video/DecodedFrame.h"

namespace vw::app {

namespace {
// M9: the WinEventHook callback (out-of-context, dispatched on the UI thread)
// needs the instance; the app is single-instance so one pointer is enough.
ApplicationController* g_controller = nullptr;

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

// config.playback.mode -> playlist mode (M7).
playlist::Mode playlistModeFrom(config::PlaybackMode m) {
    switch (m) {
        case config::PlaybackMode::Single: return playlist::Mode::Single;
        case config::PlaybackMode::Sequential: return playlist::Mode::Sequential;
        case config::PlaybackMode::Loop: return playlist::Mode::Loop;
        case config::PlaybackMode::Shuffle: return playlist::Mode::Shuffle;
    }
    return playlist::Mode::Loop;
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

    // Telemetry aggregator (spec §10.12): holds the latest snapshot; the
    // playback session feeds it ~1 Hz; WorkloadMonitor completes it at M9.
    statsCollector_ = std::make_unique<performance::StatsCollector>();

    // M9: workload monitor (CPU/GPU/RAM + hysteresis) + game/fullscreen
    // detection. Configured from the performance/detection sections; M10's
    // ResourceGovernor will consume the latched states.
    {
        const auto& c = config_->config();
        workloadMonitor_ = std::make_unique<performance::WorkloadMonitor>(
            performance::WorkloadMonitor::Config{
                .cpuPause = static_cast<double>(c.cpuPauseThreshold),
                .cpuResume = static_cast<double>(c.cpuResumeThreshold),
                .gpuPause = static_cast<double>(c.gpuPauseThreshold),
                .gpuResume = static_cast<double>(c.gpuResumeThreshold),
                .memoryPause = static_cast<double>(c.memoryPauseThreshold),
                .memoryResume = static_cast<double>(c.memoryResumeThreshold),
                .pauseDelay = std::chrono::seconds(c.pauseDelaySeconds),
                .resumeDelay = std::chrono::seconds(c.resumeDelaySeconds),
            });
        gameDetector_ = std::make_unique<detection::GameDetector>();
        gameDetector_->setLists(c.alwaysPause, c.neverPause);
        log.info(L"M9 detection: {} allow / {} deny entries, workload sampling @{}s/{}",
                 c.alwaysPause.size(), c.neverPause.size(), c.pauseDelaySeconds,
                 c.resumeDelaySeconds);
    }

    // M7: playlist (items/modes/persistence). Loaded from AppData; seeded
    // from config.playback on first run.
    loadPlaylist();

    control_.setHandler([this](UINT msg, WPARAM wParam, LPARAM) {
        auto& log = log::Logger::instance();
        if (msg == ControlWindow::focusMessage()) {
            log.info(L"second instance requested focus (UI arrives in M11)");
        } else if (msg == WM_TIMER && wParam == kWallpaperTimerId && wallpaper_) {
            wallpaper_->onTick(); // Explorer-restart validity stub (~1 Hz)
        } else if (msg == WM_TIMER && wParam == kWorkloadTimerId) {
            onWorkloadTick(); // M9: ~2 s CPU/GPU/RAM sampling
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

    // M9: foreground-window change events (EVENT_SYSTEM_FOREGROUND — no
    // polling). Out-of-context so the callback is dispatched by THIS thread's
    // message loop (the hook must live as long as the loop). The lambda needs
    // the instance: a file-scope pointer is set here (single instance).
    g_controller = this;
    winEventHook_ = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                      [](HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD,
                                         DWORD) {
                                          if (g_controller) {
                                              g_controller->onForegroundChange(hwnd);
                                          }
                                      },
                                      0, 0, WINEVENT_OUTOFCONTEXT);
    if (!winEventHook_) {
        log.warn(L"SetWinEventHook failed (game/fullscreen detection disabled)");
    }
    // Initial classification of the current foreground window.
    if (gameDetector_) {
        DWORD pid = 0;
        if (const HWND fg = ::GetForegroundWindow()) {
            ::GetWindowThreadProcessId(fg, &pid);
        }
        gameDetector_->updateForeground(pid);
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

    // M9: ~2 s workload sampling (CPU/GPU/RAM + hysteresis) — only while the
    // app runs; sampling is cheap and the collector holds the latest values.
    ::SetTimer(control_.handle(), kWorkloadTimerId, 2000, nullptr);

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
    // M8 decoder-count diagnostic: Clone decodes ONCE for all monitors (one
    // session); Independent is one decoder per display. On this single-display
    // machine both report 1; the multi-monitor case is NOT MEASURED (spec §6).
    {
        const bool clone = config_->config().wallpaperMode == config::WallpaperMode::Clone;
        log.info(L"wallpaper mode: {} ({} monitor(s), decoder count: 1 {})",
                 clone ? L"clone" : L"independent",
                 wallpaper_ ? wallpaper_->monitors().size() : 0,
                 clone ? L"— decode-once for all displays" : L"— one per display");
    }
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
    if (!playlist_ || playlist_->empty()) {
        log.info(L"playlist is empty — wallpaper shows the test texture");
        return;
    }
    if (!playback_) {
        playback_ = std::make_unique<playback::PlaybackController>();
    }
    if (statsCollector_) {
        statsCollector_->reset(); // fresh session: no stale telemetry
        // Telemetry stream (spec §10.12): each per-second stats recompute is
        // aggregated into the collector's snapshot; the wallpaper's monitor
        // feeds the per-monitor detail. Never blocks (copy-only observer).
        playback_->setStatsObserver([this](const playback::PlaybackStats& s) {
            if (!statsCollector_ || !wallpaper_) {
                return;
            }
            statsCollector_->updatePlayback(s.decodedFps, s.presentedFps, s.droppedFrames,
                                            s.decodeLatencyMs, s.renderTimeMs,
                                            playback_->hardwareDecoding());
            const auto& monitors = wallpaper_->monitors();
            if (!monitors.empty()) {
                statsCollector_->updatePerMonitor(monitors.front().id, s.presentedFps,
                                                  s.droppedFrames);
            }
        });
    }
    // M5: hardware decode on the wallpaper's D3D device (same adapter);
    // scaling per config.playback.scaling (Fill default).
    if (wallpaper_) {
        wallpaper_->setScaling(rendererScalingFrom(config_->config().scaling));
    }
    if (playlist_->currentIndex() == playlist::PlaylistManager::kNoIndex) {
        playlist_->setCurrent(0);
    }
    if (!startPlaylistItem(playlist_->currentIndex())) {
        log.warn(L"initial playlist item could not be played — wallpaper shows the test texture");
    }
    // M6: no frame timer — the message loop waits on the controller's
    // waitable timer + new-frame event (source-FPS pacing, zero busy-wait).
}

void ApplicationController::loadPlaylist() {
    auto& log = log::Logger::instance();
    playlistPath_ = appDataDir_ / L"playlist.json";
    auto loaded = playlist::PlaylistStore::load(playlistPath_);
    if (loaded) {
        playlist_ = std::make_unique<playlist::PlaylistManager>(std::move(*loaded));
        log.info(L"playlist: {} item(s) loaded, current={}, mode={}, loop={}",
                 playlist_->size(), playlist_->currentIndex(),
                 playlist::PlaylistStore::modeName(playlist_->mode()),
                 playlist_->loop() ? L"on" : L"off");
        return;
    }
    // First run (or corrupt store — recovery = defaults): seed from config
    // (docs/03 §3.9). mode/loop come from config.playback on first run and
    // are persisted from then on.
    playlist::PlaylistData base;
    base.mode = playlistModeFrom(config_->config().mode);
    base.loop = config_->config().loop;
    playlist_ = std::make_unique<playlist::PlaylistManager>(std::move(base));
    const std::wstring& path = config_->config().videoPath;
    if (!path.empty()) {
        playlist_->add(path);
        playlist_->setCurrent(0);
    }
    if (auto saved = playlist::PlaylistStore::save(playlistPath_, playlist_->data()); !saved) {
        log.warn(L"playlist seed save failed: {}", saved.error());
    }
    log.info(L"playlist: initialized from config (videoPath={}), mode={}, loop={}",
             path.empty() ? L"(none)" : path,
             playlist::PlaylistStore::modeName(playlist_->mode()),
             playlist_->loop() ? L"on" : L"off");
}

bool ApplicationController::startPlaylistItem(size_t index) {
    auto& log = log::Logger::instance();
    const playlist::PlaylistItem* item = playlist_->itemAt(index);
    if (!item || playlist_->isUnavailable(index)) {
        return false;
    }
    if (wallpaper_) {
        wallpaper_->setScaling(rendererScalingFrom(config_->config().scaling));
    }
    const LONGLONG t0 = util::Clock::instance().now100ns();
    auto opened =
        playback_->open(item->path, wallpaper_ ? wallpaper_->device() : nullptr,
                        static_cast<size_t>(config_->config().frameQueue));
    if (!opened) {
        log.warn(L"playlist item {} '{}' cannot be opened: {} — marking unavailable", index,
                 item->path, opened.error());
        playlist_->markUnavailable(index);
        return false;
    }
    // Cache real metadata back into the item (lightweight prep, docs/03
    // §3.9: persisted so a large playlist starts instantly on later runs;
    // never decoded frames). Transition = meaningful change — save now.
    const auto& m = playback_->metadata();
    playlist_->updateCachedMetadata(index, m.duration100ns, m.width, m.height, m.codec);
    if (auto saved = playlist::PlaylistStore::save(playlistPath_, playlist_->data()); !saved) {
        log.warn(L"playlist save failed: {}", saved.error());
    }
    if (auto started = playback_->start(); !started) {
        log.warn(L"playback start failed for '{}': {} — marking unavailable", item->path,
                 started.error());
        playlist_->markUnavailable(index);
        return false;
    }
    const double openMs = static_cast<double>(util::Clock::instance().now100ns() - t0) / 10000.0;
    lastPlayedPath_ = item->path;
    log.info(L"transition to playlist item {} ('{}') in {:.1f} ms", index, item->path, openMs);
    return true;
}

// M8: the monitor id the (single) playback session targets in Independent
// mode. On this machine there is exactly one display; with real multi-monitor
// this becomes per-session routing (NOT MEASURED — single display, spec §6).
std::wstring ApplicationController::primaryMonitorId() const {
    if (wallpaper_) {
        const auto& monitors = wallpaper_->monitors();
        const auto it = std::find_if(monitors.begin(), monitors.end(),
                                     [](const monitors::MonitorInfo& m) { return m.primary; });
        if (it != monitors.end()) {
            return it->id;
        }
        if (!monitors.empty()) {
            return monitors.front().id;
        }
    }
    return {};
}

void ApplicationController::handleEndOfStream() {
    auto& log = log::Logger::instance();
    if (!playlist_ || !playback_) {
        return;
    }
    // A session that presented zero frames is broken (opens but decodes
    // nothing) — mark it unavailable (M7; M12 hardens with attempt tracking).
    if (playback_->stats().presentedFrames == 0) {
        if (const auto* item = playlist_->currentItem()) {
            log.warn(L"playlist item produced no frames — marking unavailable: {}", item->path);
        }
        playlist_->markUnavailable(playlist_->currentIndex());
    }
    const size_t prev = playlist_->currentIndex();
    size_t next = playlist_->nextIndex();
    // Bounded: every non-returning iteration marks one item unavailable, so
    // this can never loop forever on a playlist of broken files.
    for (size_t tries = 0; tries <= playlist_->size(); ++tries) {
        if (next == playlist::PlaylistManager::kNoIndex) {
            log.info(L"playlist ended — holding the last frame");
            playback_->stop();
            return;
        }
        const playlist::PlaylistItem* nextItem = playlist_->itemAt(next);
        if (next == prev && nextItem && nextItem->path == lastPlayedPath_ &&
            playback_->isOpen()) {
            // Same item (Single self-loop / 1-item loop): reuse the open
            // reader + decoder — no reopen, no hardware re-probe (docs §34).
            if (auto replayed = playback_->replay(); replayed) {
                return;
            }
            log.warn(L"loop replay failed — marking item unavailable: {}", nextItem->path);
            playlist_->markUnavailable(next);
        }
        playlist_->setCurrent(next);
        if (startPlaylistItem(next)) {
            return;
        }
        next = playlist_->nextIndex(); // failed open marked it unavailable — advance
    }
    log.warn(L"no playable playlist items — holding the last frame");
    playback_->stop();
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
        handleEndOfStream(); // M7: loop / next / stop per the playlist mode
        return;
    }
    // M8: route per wallpaper mode. Clone = one frame broadcast to every
    // host; Independent = the frame binds to the (single, on this machine)
    // display's host. N-session independent fan-out is exercised by the
    // simulated-topology tests; real multi-monitor is NOT MEASURED here.
    const bool clone = config_->config().wallpaperMode == config::WallpaperMode::Clone;
    if (auto set = clone ? wallpaper_->setVideoFrame(*frame)
                         : wallpaper_->setVideoFrameFor(primaryMonitorId(), *frame);
        !set) {
        log.warn(L"video frame upload failed: {}", set.error());
    }
    const LONGLONG renderStart = util::Clock::instance().now100ns();
    if (auto rendered = wallpaper_->renderAll(); !rendered) {
        log.warn(L"frame render failed: {}", rendered.error());
    }
    playback_->noteRenderTime(
        static_cast<double>(util::Clock::instance().now100ns() - renderStart) / 10000.0);
}

void ApplicationController::onWorkloadTick() {
    if (!workloadMonitor_ || !statsCollector_) {
        return;
    }
    const auto state = workloadMonitor_->sample(std::chrono::steady_clock::now());
    performance::WorkloadMonitor::pushToCollector(*statsCollector_, state);
    // DEBUG telemetry (~2 s, Debug builds only — no per-second log spam in
    // Release; the UI (M11) reads the snapshot instead).
    auto& log = log::Logger::instance();
    log.debug(L"workload: cpu {:.0f}%, ram {:.0f}% ({} MB used), gpu mem {:.0f}/{:.0f} MB"
              L" | high: cpu={} gpu={} mem={}",
              state.cpuUsage, state.memoryUsage, state.systemMemoryUsed / (1024 * 1024),
              state.gpuMemoryUsed / (1024.0 * 1024.0), state.gpuMemoryBudget / (1024.0 * 1024.0),
              state.cpuHigh ? L"yes" : L"no", state.gpuHigh ? L"yes" : L"no",
              state.memoryHigh ? L"yes" : L"no");
}

void ApplicationController::onForegroundChange(HWND hwnd) {
    if (!gameDetector_) {
        return;
    }
    if (hwnd == lastForeground_) {
        return; // duplicate event — no rescans
    }
    lastForeground_ = hwnd;
    DWORD pid = 0;
    if (hwnd) {
        ::GetWindowThreadProcessId(hwnd, &pid);
    }
    const auto state = gameDetector_->updateForeground(pid);
    if (state.pid == 0) {
        return;
    }
    auto& log = log::Logger::instance();
    log.debug(L"foreground: pid {} {} ({})", state.pid, state.processPath,
              state.classification == detection::GameClass::Game
                  ? L"game [allow]"
                  : (state.classification == detection::GameClass::NotGame
                         ? L"not-game [deny]"
                         : L"unlisted"));
}

void ApplicationController::shutdown() {
    // docs/03 §3.17: stop workers -> stop rendering -> release GPU resources
    // -> MFShutdown -> save state -> close window -> flush logs -> exit.
    if (winEventHook_) {
        ::UnhookWinEvent(winEventHook_);
        winEventHook_ = nullptr;
    }
    g_controller = nullptr;
    ::KillTimer(control_.handle(), kWallpaperTimerId);
    ::KillTimer(control_.handle(), kWorkloadTimerId);
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
    if (playlist_) {
        if (auto saved = playlist::PlaylistStore::save(playlistPath_, playlist_->data());
            !saved) {
            log::Logger::instance().warn(L"playlist save failed at shutdown: {}", saved.error());
        }
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
