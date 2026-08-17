#include "app/ApplicationController.h"

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "detection/FullscreenDetector.h"
#include "graphics/D3D11DeviceManager.h"
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

// governor::Reason bitmask -> ui::PauseReason bitmask (the bit NUMBERING
// differs for MonitorHidden/SystemSuspended — spec §10.13 vs governor).
uint32_t uiPauseReasons(uint32_t governorReasons) {
    uint32_t out = 0;
    const auto map = [&](uint32_t governorBit, uint32_t uiBit) {
        if ((governorReasons & governorBit) != 0) {
            out |= uiBit;
        }
    };
    map(governor::Reason::User, ui::PauseReason::User);
    map(governor::Reason::Game, ui::PauseReason::Game);
    map(governor::Reason::Fullscreen, ui::PauseReason::Fullscreen);
    map(governor::Reason::HighCPU, ui::PauseReason::HighCPU);
    map(governor::Reason::HighGPU, ui::PauseReason::HighGPU);
    map(governor::Reason::HighMemory, ui::PauseReason::HighMemory);
    map(governor::Reason::Battery, ui::PauseReason::Battery);
    map(governor::Reason::Locked, ui::PauseReason::Locked);
    map(governor::Reason::DisplayOff, ui::PauseReason::DisplayOff);
    map(governor::Reason::MonitorHidden, ui::PauseReason::MonitorHidden);
    map(governor::Reason::SystemSuspended, ui::PauseReason::SystemSuspended);
    return out;
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

    control_.setHandler([this](UINT msg, WPARAM wParam, LPARAM lParam) {
        auto& log = log::Logger::instance();
        if (msg == ControlWindow::focusMessage()) {
            // Second instance: bring the UI forward (spec §10.1).
            log.info(L"second instance requested focus");
            if (ui_) {
                ui_->show();
            }
        } else if (msg == commandWakeMessage()) {
            drainCommands(); // M11: UI/tray commands run on the control thread
        } else if (msg == WM_TIMER && wParam == kWallpaperTimerId && wallpaper_) {
            wallpaper_->onTick(); // Explorer-restart validity stub (~1 Hz)
            // M10: long-pause release (PAUSED -> SUSPENDED). The 1 Hz tick
            // is also the governor's clock.
            if (governor_) {
                governor_->onTick();
            }
            // M11: library watch/probe results + config write-batching.
            if (library_) {
                library_->pollChangeEvents();
            }
            if (config_) {
                config_->maybeFlushDirty(std::chrono::steady_clock::now());
            }
        } else if (msg == WM_TIMER && wParam == kWorkloadTimerId) {
            onWorkloadTick(); // M9: ~2 s CPU/GPU/RAM sampling
        } else if (msg == WM_TIMER && wParam == kUiTelemetryTimerId) {
            onUiTelemetryTick(); // M11: 2 Hz pushes while the UI is open
        } else if (tray_ && msg == ui::TrayController::callbackMessage()) {
            // Tray icon events: left-click toggles the window; right-click
            // opens the menu (spec §10.8).
            switch (lParam) {
                case WM_LBUTTONUP:
                case NIN_SELECT: {
                    ui::Command c;
                    c.id = ui::CommandId::ToggleUi;
                    postCommand(std::move(c));
                    break;
                }
                case WM_CONTEXTMENU: {
                    const auto menuId = tray_->showMenu();
                    ui::Command c;
                    switch (menuId) {
                        case ui::TrayController::kMenuResume:
                            c.id = ui::CommandId::Resume;
                            postCommand(std::move(c));
                            break;
                        case ui::TrayController::kMenuPause:
                            c.id = ui::CommandId::Pause;
                            postCommand(std::move(c));
                            break;
                        case ui::TrayController::kMenuNext:
                            c.id = ui::CommandId::Next;
                            postCommand(std::move(c));
                            break;
                        case ui::TrayController::kMenuPrevious:
                            c.id = ui::CommandId::Previous;
                            postCommand(std::move(c));
                            break;
                        case ui::TrayController::kMenuOpen:
                            c.id = ui::CommandId::ShowUi;
                            postCommand(std::move(c));
                            break;
                        case ui::TrayController::kMenuSettings:
                            c.id = ui::CommandId::ShowUi;
                            c.i1 = 5; // Settings tab
                            postCommand(std::move(c));
                            break;
                        case ui::TrayController::kMenuExit:
                            c.id = ui::CommandId::Exit;
                            postCommand(std::move(c));
                            break;
                        default:
                            break; // current-wallpaper info item / none
                    }
                    break;
                }
            }
        } else if (msg == ControlWindow::pauseMessage()) {
            // Explicit user pause routes through the governor (sole authority).
            if (governor_) {
                governor_->setReason(governor::Reason::User, true);
            } else if (playback_) {
                playback_->pause();
            }
        } else if (msg == ControlWindow::resumeMessage()) {
            if (governor_) {
                governor_->setReason(governor::Reason::User, false);
            } else if (playback_) {
                if (auto r = playback_->resume(); !r) {
                    log.warn(L"resume failed: {}", r.error());
                }
            }
        } else if (msg == ControlWindow::stopMessage()) {
            if (governor_) {
                governor_->setReason(governor::Reason::User, true); // stop = pause-all
            }
            if (playback_) {
                playback_->stop();
            }
        } else if ((msg == WM_DISPLAYCHANGE || msg == WM_DEVICECHANGE) && wallpaper_) {
            wallpaper_->onDisplayChange();
            // Monitor bounds changed — a fullscreen window's classification
            // (rect vs monitor) may be stale. Re-classify the foreground.
            onForegroundChange(::GetForegroundWindow());
        } else if (systemMonitor_ && (msg == WM_WTSSESSION_CHANGE || msg == WM_POWERBROADCAST)) {
            // M10: lock/unlock, suspend/resume, monitor on/off -> governor.
            uint32_t reasons = governor_ ? governor_->reasons() : 0;
            const uint32_t changed = systemMonitor_->translate(msg, wParam, lParam, reasons);
            if (governor_ && changed != 0) {
                governor_->setReasons(reasons);
            }
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

    // M10: system-state notifications (lock/unlock, suspend/resume, monitor
    // power, battery) + the ResourceGovernor (sole authority over decode/
    // render start-stop). The governor is created AFTER startPlayback so
    // playback_ exists; the monitor uses the control-window handle so
    // notifications arrive as window messages.
    systemMonitor_ = std::make_unique<system::SystemStateMonitor>(control_.handle());
    systemMonitor_->start();
    systemMonitorStarted_ = true;
    if (playback_) {
        const auto& c = config_->config();
        governor_ = std::make_unique<governor::ResourceGovernor>(
            *playback_,
            governor::PausePolicy::Config{
                .longPauseReleaseSeconds = c.longPauseReleaseSeconds,
                .batteryPauses = c.batteryMode == config::BatteryMode::Pause,
            });
        // SUSPENDED released the decoder (stop()); reopening the current item
        // recreates it and seeks to the saved position.
        governor_->setResumeHandler([this]() {
            if (playlist_ && playlist_->currentIndex() < playlist_->size()) {
                startPlaylistItem(playlist_->currentIndex());
            }
        });
        // Initial battery state (never polled afterwards — only on changes).
        uint32_t reasons = governor_->reasons();
        systemMonitor_->updateBatteryReason(reasons);
        governor_->setReasons(reasons);
        // M11: every governor transition pushes the playback state to the UI
        // sink + tray tooltip (spec §10.12 — the Home panel and the tray both
        // read the PlaybackStateNotification).
        governor_->setActionObserver([this](governor::State, governor::State, uint32_t) {
            pushPlaybackState();
            updateTrayFromState();
        });
    }

    // M11: minimal library (spec §10.3) — notifications forward to the UI
    // sink; no subscribers = no-op (the engine never pushes to a closed UI).
    library_ = std::make_unique<library::LibraryManager>();
    library_->setChangeSink([this](const vw::ui::LibraryChangeNotification& n) {
        if (sink_) {
            sink_->onLibraryChange(n);
        }
    });

    // M11: system tray — created at startup, persists while the UI is closed
    // (spec §10.8: tray must not keep the UI alive; it holds only icon+menu).
    tray_ = std::make_unique<ui::TrayController>();
    tray_->create(control_.handle());
    trayCreated_ = true;

    // M11: the UI window is created LAZILY on first show (spec §10.1). The
    // object exists so commands/notifications have a target; the window is
    // built when the user opens it (tray left-click / second instance / …).
    ui_ = std::make_unique<ui::Win32UI>(
        [this](vw::ui::Command c) { postCommand(std::move(c)); },
        [this]() { // Playlists panel re-pull (read path, never a poll timer)
            if (ui_ && ui_->exists()) {
                ui_->refreshFromSnapshot(getUiSnapshot());
            }
        },
        [this](vw::ui::LibraryItemId id) { // lazy metadata probe request
            if (library_) {
                library_->requestMetadata(id);
            }
        });
    ui_->setOnClose([this]() {
        // Spec §10.1: close hides to tray per config; otherwise the window is
        // destroyed (engine + tray keep running either way).
        if (config_ && config_->config().minimizeToTray) {
            ui_->hide();
        } else {
            ui_->destroy();
            syncUiSubscription();
        }
    });

    // Home panel GPU adapter field (first hardware adapter — cosmetic).
    adapterName_ = adapterName();

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
    // M11: Home panel + tray tooltip reflect the new item/state immediately.
    pushPlaybackState();
    updateTrayFromState();
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
    feedDetectionReasons(); // M10: hysteresis-latched workload -> governor
}

void ApplicationController::feedDetectionReasons() {
    if (!governor_) {
        return;
    }
    const auto& c = config_->config();
    uint32_t reasons = governor_->reasons();

    // Game / fullscreen (immediate reasons). The foreground classification is
    // cached from the last WinEventHook event.
    const bool gamePause =
        c.pauseOnGame && gameDetector_ &&
        gameDetector_->state().classification == detection::GameClass::Game;
    const bool fsPause = c.pauseOnFullscreen && detection::isFullscreenState(fullscreenState_);
    const performance::WorkloadState wl =
        workloadMonitor_ ? workloadMonitor_->state() : performance::WorkloadState{};
    const bool cpuPause = c.pauseOnHighCPU && wl.cpuHigh;
    const bool gpuPause = c.pauseOnHighGPU && wl.gpuHigh;
    const bool memPause = c.pauseOnHighRAM && wl.memoryHigh;

    const uint32_t wanted = (gamePause ? governor::Reason::Game : 0) |
                            (fsPause ? governor::Reason::Fullscreen : 0) |
                            (cpuPause ? governor::Reason::HighCPU : 0) |
                            (gpuPause ? governor::Reason::HighGPU : 0) |
                            (memPause ? governor::Reason::HighMemory : 0);
    // Preserve the non-detection reasons (User, Battery, Locked, DisplayOff,
    // SystemSuspended) — the detection layer only owns these five bits.
    constexpr uint32_t kDetectionMask = governor::Reason::Game | governor::Reason::Fullscreen |
                                        governor::Reason::HighCPU | governor::Reason::HighGPU |
                                        governor::Reason::HighMemory;
    reasons = (reasons & ~kDetectionMask) | wanted;
    if (reasons != governor_->reasons()) {
        governor_->setReasons(reasons);
    }
}

void ApplicationController::onForegroundChange(HWND hwnd) {
    if (!gameDetector_) {
        return;
    }
    if (hwnd == lastForeground_) {
        return; // duplicate event — no rescans
    }
    lastForeground_ = hwnd;

    // M9: fullscreen classification (rect + styles vs the window's monitor).
    // Cached on the controller — M10's ResourceGovernor reads it for the
    // pause-on-fullscreen policy.
    fullscreenState_ = classifyForegroundFullscreen(hwnd);
    feedDetectionReasons(); // M10: game/fullscreen -> governor

    DWORD pid = 0;
    if (hwnd) {
        ::GetWindowThreadProcessId(hwnd, &pid);
    }
    const auto state = gameDetector_->updateForeground(pid);
    if (state.pid == 0) {
        return;
    }
    auto& log = log::Logger::instance();
    log.debug(L"foreground: pid {} {} ({}) | window: {}", state.pid, state.processPath,
              state.classification == detection::GameClass::Game
                  ? L"game [allow]"
                  : (state.classification == detection::GameClass::NotGame
                         ? L"not-game [deny]"
                         : L"unlisted"),
              detection::isFullscreenState(fullscreenState_)
                  ? (fullscreenState_ == detection::WindowState::Fullscreen ? L"fullscreen"
                                                                            : L"borderless-fullscreen")
                  : (fullscreenState_ == detection::WindowState::Maximized ? L"maximized"
                                                                           : L"windowed"));
}

detection::WindowState ApplicationController::classifyForegroundFullscreen(HWND hwnd) const {
    if (!hwnd || !::IsWindow(hwnd)) {
        return detection::WindowState::Windowed;
    }
    RECT winRect{};
    if (!::GetWindowRect(hwnd, &winRect)) {
        return detection::WindowState::Windowed;
    }
    // The monitor the window sits on (nearmost, matching how the user sees
    // it). Prefer the app's own monitor snapshot (physical bounds, stable
    // ids); fall back to a direct GetMonitorInfo probe.
    RECT monRect{};
    const HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (wallpaper_) {
        const auto& ms = wallpaper_->monitors();
        const auto it = std::find_if(ms.begin(), ms.end(),
                                     [&](const monitors::MonitorInfo& m) { return m.handle == mon; });
        if (it != ms.end()) {
            monRect = it->bounds;
        }
    }
    if (monRect.right == monRect.left || monRect.bottom == monRect.top) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(mon, &mi) != FALSE) {
            monRect = mi.rcMonitor;
        }
    }
    if (monRect.right == monRect.left || monRect.bottom == monRect.top) {
        return detection::WindowState::Windowed; // no monitor info — can't classify
    }
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return detection::classifyWindowState(winRect, monRect, style, exStyle);
}

void ApplicationController::shutdown() {
    // docs/03 §3.17: stop workers -> stop rendering -> release GPU resources
    // -> MFShutdown -> save state -> close window -> flush logs -> exit.
    if (systemMonitor_ && systemMonitorStarted_) {
        systemMonitor_->stop(); // unregister WTS/power notifications
        systemMonitorStarted_ = false;
    }
    systemMonitor_.reset();
    if (winEventHook_) {
        ::UnhookWinEvent(winEventHook_);
        winEventHook_ = nullptr;
    }
    g_controller = nullptr;
    ::KillTimer(control_.handle(), kWallpaperTimerId);
    ::KillTimer(control_.handle(), kWorkloadTimerId);
    ::KillTimer(control_.handle(), kUiTelemetryTimerId);
    uiTelemetryRunning_ = false;
    unsubscribe(); // no callbacks after the UI is gone (spec §10.12)
    if (ui_) {
        ui_->destroy();
        ui_.reset();
    }
    if (tray_ && trayCreated_) {
        tray_->destroy(); // remove the tray icon before the window dies
        trayCreated_ = false;
    }
    tray_.reset();
    if (library_) {
        library_.reset(); // joins the watch + probe threads
    }
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

// ---- M11: command queue ----------------------------------------------------

UINT ApplicationController::commandWakeMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"VideoWallpaper.CommandWake");
    return msg;
}

void ApplicationController::postCommand(vw::ui::Command c) {
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        commands_.push_back(std::move(c));
    }
    // Wake the control thread (it may be blocked in MsgWaitForMultipleObjects).
    if (control_.handle()) {
        ::PostMessageW(control_.handle(), commandWakeMessage(), 0, 0);
    }
}

void ApplicationController::drainCommands() {
    std::deque<vw::ui::Command> batch;
    {
        std::lock_guard<std::mutex> lk(cmdMu_);
        batch.swap(commands_);
    }
    for (const auto& c : batch) {
        dispatchCommand(c);
    }
}

void ApplicationController::dispatchCommand(const vw::ui::Command& c) {
    auto& log = log::Logger::instance();
    switch (c.id) {
        case vw::ui::CommandId::PlayPauseToggle: {
            if (governor_) {
                const bool paused = governor_->state() != governor::State::Active;
                governor_->setReason(governor::Reason::User, !paused);
            }
            break;
        }
        case vw::ui::CommandId::Pause:
            if (governor_) {
                governor_->setReason(governor::Reason::User, true);
            }
            break;
        case vw::ui::CommandId::Resume:
            if (governor_) {
                governor_->setReason(governor::Reason::User, false);
            }
            break;
        case vw::ui::CommandId::Next:
            playNext();
            break;
        case vw::ui::CommandId::Previous:
            playPrevious();
            break;
        case vw::ui::CommandId::SetWallpaperFile:
            if (!c.s2.empty()) {
                setWallpaperFile(c.s2);
            }
            break;
        case vw::ui::CommandId::SetWallpaperPlaylist:
            // v1: the engine has ONE playlist (shared-playlist mode is v2);
            // "apply playlist" is the current playback — nothing to change.
            log.info(L"command: SetWallpaperPlaylist (v1 single playlist — no-op)");
            break;
        case vw::ui::CommandId::SetGlobalMode: {
            auto& cfg = config_->config();
            cfg.wallpaperMode = c.b1 ? config::WallpaperMode::Clone
                                     : config::WallpaperMode::Independent;
            config_->markDirty();
            config_->markConfigChanged();
            log.info(L"command: wallpaper mode -> {}", c.b1 ? L"clone" : L"independent");
            pushWallpaperAssignment();
            break;
        }
        case vw::ui::CommandId::SetScaling: {
            auto& cfg = config_->config();
            cfg.scaling = static_cast<config::ScalingMode>(c.scaling);
            if (wallpaper_) {
                wallpaper_->setScaling(rendererScalingFrom(cfg.scaling));
            }
            config_->markDirty();
            config_->markConfigChanged();
            log.info(L"command: scaling -> {}", scalingNameForLog(c.scaling));
            pushWallpaperAssignment();
            break;
        }
        case vw::ui::CommandId::GrabFrameSnapshot:
            grabFrameSnapshotCommand();
            break;
        case vw::ui::CommandId::PlaylistCreate:
        case vw::ui::CommandId::PlaylistRename:
        case vw::ui::CommandId::PlaylistDelete:
        case vw::ui::CommandId::PlaylistDuplicate:
            // v1: single playlist (multi-playlist is v2 — shared-playlist
            // mode). Logged, no-op.
            log.info(L"command: playlist multi-manage is v2 (single playlist in v1)");
            break;
        case vw::ui::CommandId::PlaylistAddFiles: {
            if (!playlist_ || c.paths.empty()) {
                break;
            }
            for (const auto& p : c.paths) {
                playlist_->add(p);
            }
            savePlaylistAndNotify();
            break;
        }
        case vw::ui::CommandId::PlaylistAddLibraryItems: {
            if (!playlist_ || !library_) {
                break;
            }
            for (const auto id : c.itemIds) {
                if (const auto* item = library_->itemById(id)) {
                    playlist_->add(item->path);
                }
            }
            savePlaylistAndNotify();
            break;
        }
        case vw::ui::CommandId::PlaylistRemoveItem:
            if (playlist_ && c.i1 >= 0 &&
                c.i1 < static_cast<int32_t>(playlist_->size())) {
                playlist_->remove(static_cast<size_t>(c.i1));
                savePlaylistAndNotify();
            }
            break;
        case vw::ui::CommandId::PlaylistMoveItem:
            if (playlist_ && c.i1 >= 0) {
                const size_t from = static_cast<size_t>(c.i1);
                const int64_t to = static_cast<int64_t>(from) + c.i2;
                if (to >= 0 && to < static_cast<int64_t>(playlist_->size())) {
                    playlist_->move(from, static_cast<size_t>(to));
                    savePlaylistAndNotify();
                }
            }
            break;
        case vw::ui::CommandId::PlaylistToggleItem:
            if (playlist_ && c.i1 >= 0 &&
                c.i1 < static_cast<int32_t>(playlist_->size())) {
                playlist_->setEnabled(static_cast<size_t>(c.i1), c.b1);
                savePlaylistAndNotify();
            }
            break;
        case vw::ui::CommandId::PlaylistSetItemTimes:
            if (playlist_ && c.i1 >= 0) {
                playlist_->setItemTimes(static_cast<size_t>(c.i1),
                                        static_cast<int64_t>(c.d1 * 10'000'000.0),
                                        static_cast<int64_t>(c.d2 * 10'000'000.0));
                savePlaylistAndNotify();
            }
            break;
        case vw::ui::CommandId::PlaylistSetMode:
            if (playlist_) {
                playlist_->setMode(static_cast<playlist::Mode>(c.mode));
                savePlaylistAndNotify();
            }
            break;
        case vw::ui::CommandId::PlaylistSetLoop:
            if (playlist_) {
                playlist_->setLoop(c.b1);
                savePlaylistAndNotify();
            }
            break;
        case vw::ui::CommandId::LibraryAddFiles:
            if (library_) {
                library_->addFiles(c.paths);
            }
            break;
        case vw::ui::CommandId::LibraryAddFolder:
            if (library_ && !c.s1.empty()) {
                library_->addFolder(c.s1);
            }
            break;
        case vw::ui::CommandId::LibraryRemove:
            if (library_) {
                library_->remove(c.itemIds);
            }
            break;
        case vw::ui::CommandId::LibraryRefresh:
            if (library_) {
                library_->refresh();
            }
            break;
        case vw::ui::CommandId::ConfigSet:
            applyConfigSetLive(c.s1, c.s2);
            break;
        case vw::ui::CommandId::ShowUi:
            if (ui_) {
                ui_->selectTab(c.i1);
                syncUiSubscription();
            }
            break;
        case vw::ui::CommandId::ToggleUi:
            if (ui_) {
                ui_->toggle();
                syncUiSubscription();
            }
            break;
        case vw::ui::CommandId::Focus:
            if (ui_) {
                ui_->show();
                syncUiSubscription();
            }
            break;
        case vw::ui::CommandId::Exit:
            log.info(L"command: exit requested (tray)");
            ::PostMessageW(control_.handle(), WM_APP, 0, 0); // request shutdown
            break;
    }
}

void ApplicationController::savePlaylistAndNotify() {
    if (!playlist_) {
        return;
    }
    if (auto saved = playlist::PlaylistStore::save(playlistPath_, playlist_->data()); !saved) {
        log::Logger::instance().warn(L"playlist save failed: {}", saved.error());
    }
    vw::ui::PlaylistChangeNotification n;
    n.kind = vw::ui::PlaylistChangeKind::ItemsChanged;
    if (sink_) {
        sink_->onPlaylistChange(n);
    }
}

void ApplicationController::pushWallpaperAssignment() {
    if (!sink_) {
        return;
    }
    vw::ui::WallpaperAssignmentNotification n;
    n.monitorId = primaryMonitorId();
    n.source = vw::ui::WallpaperSource::Playlist; // v1: the playlist is the source
    n.sourceId = L"default";
    n.scaling = static_cast<vw::ui::ScalingMode>(config_->config().scaling);
    n.clone = config_->config().wallpaperMode == config::WallpaperMode::Clone;
    sink_->onWallpaperAssignment(n);
}

// ---- M11: notification sink + telemetry -----------------------------------

void ApplicationController::subscribe(vw::ui::INotificationSink* sink) {
    sink_ = sink;
    if (sink_ && !uiTelemetryRunning_) {
        // Telemetry only while ≥1 subscriber (spec §10.12): the timer is
        // armed on subscribe and killed on unsubscribe.
        ::SetTimer(control_.handle(), kUiTelemetryTimerId, 500, nullptr);
        uiTelemetryRunning_ = true;
    } else if (!sink_ && uiTelemetryRunning_) {
        ::KillTimer(control_.handle(), kUiTelemetryTimerId);
        uiTelemetryRunning_ = false;
    }
}

void ApplicationController::unsubscribe() {
    subscribe(nullptr);
}

void ApplicationController::syncUiSubscription() {
    if (!ui_) {
        return;
    }
    const bool uiExists = ui_->exists();
    if (uiExists && sink_ != ui_.get()) {
        subscribe(ui_.get());
        if (sink_) {
            sink_->onTelemetry(statsCollector_ ? statsCollector_->snapshot()
                                               : vw::ui::TelemetrySnapshot{});
        }
    } else if (!uiExists && sink_) {
        unsubscribe();
    }
}

void ApplicationController::onUiTelemetryTick() {
    if (!sink_) {
        return;
    }
    if (library_) {
        library_->pollChangeEvents();
    }
    if (config_) {
        config_->maybeFlushDirty(std::chrono::steady_clock::now());
    }
    sink_->onTelemetry(statsCollector_ ? statsCollector_->snapshot()
                                       : vw::ui::TelemetrySnapshot{});
    updateTrayFromState();
}

std::wstring ApplicationController::currentVideoName() const {
    if (playlist_ && playlist_->currentItem()) {
        return std::filesystem::path(playlist_->currentItem()->path).filename().wstring();
    }
    return {};
}

void ApplicationController::pushPlaybackState() {
    if (!sink_) {
        return;
    }
    vw::ui::PlaybackStateNotification n;
    n.monitorId = primaryMonitorId();
    n.videoName = currentVideoName();
    n.playlistName = L"Playlist";
    n.pauseReasons = governor_ ? uiPauseReasons(governor_->reasons()) : 0;
    if (governor_) {
        switch (governor_->state()) {
            case governor::State::Active: n.state = vw::ui::PlaybackState::Playing; break;
            case governor::State::Paused: n.state = vw::ui::PlaybackState::Paused; break;
            case governor::State::Suspended: n.state = vw::ui::PlaybackState::Suspended; break;
        }
    } else {
        n.state = vw::ui::PlaybackState::NoWallpaper;
    }
    if (playback_ && playback_->isOpen()) {
        if (playback_->hardwareDecoding()) {
            n.decoderMode = playback_->decoderName().empty() ? L"hardware"
                                                             : playback_->decoderName();
        } else {
            n.decoderMode = L"Software fallback";
        }
    }
    n.adapterName = adapterName_;
    sink_->onPlaybackState(n);
}

void ApplicationController::updateTrayFromState() {
    if (!tray_ || !trayCreated_) {
        return;
    }
    const wchar_t* state = L"Stopped";
    if (governor_) {
        switch (governor_->state()) {
            case governor::State::Active: state = L"Playing"; break;
            case governor::State::Paused: state = L"Paused"; break;
            case governor::State::Suspended: state = L"Suspended"; break;
        }
    }
    tray_->setTooltip(std::wstring(L"Video Wallpaper — ") + state);
    tray_->setCurrentVideo(currentVideoName());
}

// ---- M11: command helpers --------------------------------------------------

void ApplicationController::playNext() {
    if (!playlist_ || !playback_) {
        return;
    }
    const size_t next = playlist_->nextIndex();
    if (next == playlist::PlaylistManager::kNoIndex) {
        log::Logger::instance().info(L"playlist ended — holding the last frame");
        playback_->stop();
        return;
    }
    playlist_->setCurrent(next);
    if (!startPlaylistItem(next)) {
        log::Logger::instance().warn(L"next item failed to open — holding the last frame");
    }
    pushPlaybackState();
}

void ApplicationController::playPrevious() {
    if (!playlist_ || !playback_) {
        return;
    }
    const size_t prev = playlist_->previousIndex();
    if (prev == playlist::PlaylistManager::kNoIndex) {
        return;
    }
    playlist_->setCurrent(prev);
    if (!startPlaylistItem(prev)) {
        log::Logger::instance().warn(L"previous item failed to open — holding the last frame");
    }
    pushPlaybackState();
}

void ApplicationController::setWallpaperFile(const std::wstring& path) {
    if (!playlist_ || !playback_) {
        return;
    }
    // v1: setting a wallpaper file replaces the playlist with that single
    // item and plays it (the Monitors/Library panels both route here).
    playlist_->clear();
    playlist_->add(path);
    playlist_->setCurrent(0);
    if (auto saved = playlist::PlaylistStore::save(playlistPath_, playlist_->data()); !saved) {
        log::Logger::instance().warn(L"playlist save failed: {}", saved.error());
    }
    if (!startPlaylistItem(0)) {
        log::Logger::instance().warn(L"set wallpaper failed to open '{}'", path);
    }
    pushPlaybackState();
    pushWallpaperAssignment();
}

void ApplicationController::grabFrameSnapshotCommand() {
    if (!wallpaper_) {
        return;
    }
    auto snap = wallpaper_->grabFrameSnapshot();
    if (!snap) {
        log::Logger::instance().warn(L"preview grab failed: {}", snap.error());
        if (ui_ && ui_->exists()) {
            ::MessageBoxW(ui_->hwnd(), L"No preview available.\n\nThe current frame "
                                        L"cannot be read back (hardware path or no frame yet).",
                          L"Video Wallpaper", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    // BGRA8 rows (top-down) -> top-down DIB section -> HBITMAP.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(snap->width);
    bi.bmiHeader.biHeight = -static_cast<LONG>(snap->height); // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!bmp || !bits) {
        log::Logger::instance().error(L"preview DIB creation failed");
        if (bmp) {
            ::DeleteObject(bmp);
        }
        return;
    }
    std::memcpy(bits, snap->bgra.data(), snap->bgra.size());
    if (ui_) {
        ui_->showFrameSnapshot(bmp); // ownership transfers to the Monitors panel
    } else {
        ::DeleteObject(bmp);
    }
}

void ApplicationController::applyConfigSetLive(const std::wstring& key, const std::wstring& value) {
    auto& log = log::Logger::instance();
    if (!config_) {
        return;
    }
    std::wstring error;
    if (!config::ConfigurationManager::applyConfigSet(config_->config(), key, value, error)) {
        log.warn(L"CONFIG_SET rejected ({}={}): {}", key, value, error);
        return;
    }
    config_->markDirty();
    config_->markConfigChanged(); // revision bump — governor reacts to live changes
    log.info(L"CONFIG_SET: {}={}", key, value);

    // Live apply (spec §10.10: validated/clamped, live effect, no restart).
    const auto& c = config_->config();
    if (key == L"cpuPauseThreshold" || key == L"cpuResumeThreshold" ||
        key == L"gpuPauseThreshold" || key == L"gpuResumeThreshold" ||
        key == L"memoryPauseThreshold" || key == L"memoryResumeThreshold" ||
        key == L"pauseDelaySeconds" || key == L"resumeDelaySeconds") {
        if (workloadMonitor_) {
            workloadMonitor_->reconfigure(performance::WorkloadMonitor::Config{
                .cpuPause = static_cast<double>(c.cpuPauseThreshold),
                .cpuResume = static_cast<double>(c.cpuResumeThreshold),
                .gpuPause = static_cast<double>(c.gpuPauseThreshold),
                .gpuResume = static_cast<double>(c.gpuResumeThreshold),
                .memoryPause = static_cast<double>(c.memoryPauseThreshold),
                .memoryResume = static_cast<double>(c.memoryResumeThreshold),
                .pauseDelay = std::chrono::seconds(c.pauseDelaySeconds),
                .resumeDelay = std::chrono::seconds(c.resumeDelaySeconds),
            });
        }
    } else if (key == L"scaling") {
        if (wallpaper_) {
            wallpaper_->setScaling(rendererScalingFrom(c.scaling));
        }
        pushWallpaperAssignment();
    } else if (key == L"wallpaperMode") {
        pushWallpaperAssignment();
    } else if (key == L"batteryMode") {
        if (governor_) {
            governor_->setBatteryPauses(c.batteryMode == config::BatteryMode::Pause);
        }
        // Re-read the battery state with the new policy.
        if (systemMonitor_ && governor_) {
            uint32_t reasons = governor_->reasons();
            systemMonitor_->updateBatteryReason(reasons);
            governor_->setReasons(reasons);
        }
    } else if (key == L"longPauseReleaseSeconds") {
        if (governor_) {
            governor_->setLongPauseReleaseSeconds(c.longPauseReleaseSeconds);
        }
    } else if (key == L"startWithWindows") {
        setStartWithWindows(c.startWithWindows);
    } else if (key == L"logLevel") {
        log::Logger::instance().setLevel(
            c.logLevel == L"debug" ? log::Level::Debug
                                    : (c.logLevel == L"warn" ? log::Level::Warn
                                                              : (c.logLevel == L"error"
                                                                     ? log::Level::Error
                                                                     : log::Level::Info)));
    }
}

void ApplicationController::setStartWithWindows(bool on) {
    // HKCU Run value (no admin, per spec §10.7).
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        if (on) {
            wchar_t exe[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
            ::RegSetValueExW(key, L"VideoWallpaper", 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(exe),
                             static_cast<DWORD>((std::wcslen(exe) + 1) * sizeof(wchar_t)));
        } else {
            ::RegDeleteValueW(key, L"VideoWallpaper");
        }
        ::RegCloseKey(key);
        log::Logger::instance().info(L"start-with-Windows {}", on ? L"enabled" : L"disabled");
    } else {
        log::Logger::instance().warn(L"cannot open HKCU Run key ({})", ::GetLastError());
    }
}

std::wstring ApplicationController::adapterName() const {
    const auto adapters = gfx::D3D11DeviceManager::enumerateAdapters();
    if (adapters) {
        for (const auto& a : *adapters) {
            // Skip the Basic Render Driver (software adapter).
            if (a.vendor != 0x1414) { // 0x1414 = Microsoft
                return a.description;
            }
        }
        if (!adapters->empty()) {
            return adapters->front().description;
        }
    }
    return {};
}

vw::ui::UiSnapshot ApplicationController::getUiSnapshot() const {
    vw::ui::UiSnapshot s;
    if (statsCollector_) {
        s.telemetry = statsCollector_->snapshot();
    }
    if (wallpaper_) {
        for (const auto& m : wallpaper_->monitors()) {
            vw::ui::MonitorInfo ui;
            ui.id = m.id;
            ui.handle = reinterpret_cast<uintptr_t>(m.handle);
            ui.x = m.bounds.left;
            ui.y = m.bounds.top;
            ui.width = m.bounds.right - m.bounds.left;
            ui.height = m.bounds.bottom - m.bounds.top;
            ui.workX = m.workArea.left;
            ui.workY = m.workArea.top;
            ui.workW = m.workArea.right - m.workArea.left;
            ui.workH = m.workArea.bottom - m.workArea.top;
            ui.refreshNum = m.refreshRateNumerator;
            ui.refreshDen = m.refreshRateDenominator;
            ui.primary = m.primary;
            ui.active = m.active;
            s.monitors.push_back(std::move(ui));
        }
    }
    if (library_) {
        s.libraryItems = library_->items();
    }
    if (playlist_) {
        vw::ui::PlaylistSummary ps;
        ps.id = L"default";
        ps.name = L"Playlist";
        ps.itemCount = playlist_->size();
        ps.mode = static_cast<vw::ui::PlaylistMode>(playlist_->mode());
        ps.loop = playlist_->loop();
        ps.shuffle = playlist_->mode() == playlist::Mode::Shuffle;
        s.playlists.push_back(std::move(ps));
        for (const auto& item : playlist_->items()) {
            vw::ui::PlaylistItemView view;
            view.path = item.path;
            view.start100ns = item.start100ns;
            view.end100ns = item.end100ns;
            view.enabled = item.enabled;
            view.duration100ns = item.duration100ns;
            view.width = item.width;
            view.height = item.height;
            view.codec = item.codec;
            s.playlistItems.push_back(std::move(view));
        }
    }
    pushWallpaperAssignmentInto(s.assignments);
    const auto& c = config_->config();
    s.config.pauseOnGame = c.pauseOnGame;
    s.config.pauseOnFullscreen = c.pauseOnFullscreen;
    s.config.pauseOnHighCPU = c.pauseOnHighCPU;
    s.config.pauseOnHighGPU = c.pauseOnHighGPU;
    s.config.pauseOnHighRAM = c.pauseOnHighRAM;
    s.config.cpuPauseThreshold = c.cpuPauseThreshold;
    s.config.cpuResumeThreshold = c.cpuResumeThreshold;
    s.config.gpuPauseThreshold = c.gpuPauseThreshold;
    s.config.gpuResumeThreshold = c.gpuResumeThreshold;
    s.config.memoryPauseThreshold = c.memoryPauseThreshold;
    s.config.memoryResumeThreshold = c.memoryResumeThreshold;
    s.config.pauseDelaySeconds = c.pauseDelaySeconds;
    s.config.resumeDelaySeconds = c.resumeDelaySeconds;
    s.config.longPauseReleaseSeconds = c.longPauseReleaseSeconds;
    s.config.frameQueue = c.frameQueue;
    s.config.batteryMode = static_cast<vw::ui::BatteryMode>(c.batteryMode);
    s.config.playbackMode = static_cast<vw::ui::PlaylistMode>(c.mode);
    s.config.loop = c.loop;
    s.config.scaling = static_cast<vw::ui::ScalingMode>(c.scaling);
    s.config.clone = c.wallpaperMode == config::WallpaperMode::Clone;
    s.config.startWithWindows = c.startWithWindows;
    s.config.minimizeToTray = c.minimizeToTray;
    s.config.logLevel = c.logLevel;
    return s;
}

void ApplicationController::pushWallpaperAssignmentInto(
    std::vector<vw::ui::WallpaperAssignmentNotification>& out) const {
    if (!config_) {
        return;
    }
    vw::ui::WallpaperAssignmentNotification n;
    n.monitorId = primaryMonitorId();
    n.source = vw::ui::WallpaperSource::Playlist; // v1: the playlist is the source
    n.sourceId = L"default";
    n.scaling = static_cast<vw::ui::ScalingMode>(config_->config().scaling);
    n.clone = config_->config().wallpaperMode == config::WallpaperMode::Clone;
    out.push_back(std::move(n));
}

const wchar_t* ApplicationController::scalingNameForLog(vw::ui::ScalingMode m) {
    switch (m) {
        case vw::ui::ScalingMode::Fill: return L"fill";
        case vw::ui::ScalingMode::Fit: return L"fit";
        case vw::ui::ScalingMode::Stretch: return L"stretch";
        case vw::ui::ScalingMode::Center: return L"center";
    }
    return L"fill";
}

} // namespace vw::app
