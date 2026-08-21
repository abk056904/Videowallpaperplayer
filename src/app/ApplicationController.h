#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

#include "app/ControlWindow.h"
#include "app/UiContract.h"
#include "config/ConfigurationManager.h"
#include "detection/FullscreenDetector.h"
#include "detection/GameDetector.h"
#include "governor/ResourceGovernor.h"
#include "library/LibraryManager.h"
#include "library/ThumbnailExtractor.h"
#include "performance/StatsCollector.h"
#include "performance/WorkloadMonitor.h"
#include "playlist/PlaylistManager.h"
#include "system/SystemStateMonitor.h"
#include "ui/TrayController.h"
#include "ui/Win32UI.h"
#include "util/UpdateChecker.h"
#include "wallpaper/WallpaperManager.h"

namespace vw::playback {
class PlaybackController;
}

namespace vw::app {

// Owns the process: single-instance guard, paths, control window, config,
// logger wiring, wallpaper hosting (M3), message loop, and the deterministic
// shutdown sequence (docs/03 §3.17 — more subsystems attach in later
// milestones).
class ApplicationController {
public:
    ApplicationController();
    ~ApplicationController();

    ApplicationController(const ApplicationController&) = delete;
    ApplicationController& operator=(const ApplicationController&) = delete;

    // Process entry point; returns the process exit code.
    int run();

    // Set command line (from wWinMain's PWSTR) for file association launch.
    void setCommandLine(const std::wstring& cmd) { commandLine_ = cmd; }

private:
    bool acquireSingleInstance();
    void notifyExistingInstance() const;
    void initPaths();
    void loadPlaylist(); // M7: load from AppData; seed from config on first run
    void startPlayback();
    // M7: plays playlist item `index` (open + start + metadata cache). Marks
    // the item unavailable and returns false when it cannot be played.
    bool startPlaylistItem(size_t index);
    void handleEndOfStream(); // M7: advance per playlist mode (loop/next/stop)
    std::wstring primaryMonitorId() const; // M8: independent-mode session target
    void onFrameWake(); // M6: deadline or new-frame event -> schedule + present
    void onWorkloadTick(); // M9: ~2 s CPU/GPU/RAM sampling + hysteresis
    void onForegroundChange(HWND hwnd); // M9: WinEventHook foreground event
    // M9: pure fullscreen classification of a window vs its monitor (wired
    // from onForegroundChange + WM_DISPLAYCHANGE; the governor consumes it).
    detection::WindowState classifyForegroundFullscreen(HWND hwnd) const;
    void feedDetectionReasons(); // M10: game/fullscreen/workload -> governor

    // ---- M11: UI/tray/library command surface (spec §10.10–10.11) ----
    // postCommand is thread-safe (mutex + wake message); drainCommands runs
    // on the control thread. The UI never calls engine methods directly.
    void postCommand(vw::ui::Command c);
    void drainCommands();
    void dispatchCommand(const vw::ui::Command& c);
    static UINT commandWakeMessage();

    // Notification sink: the UI window is the single subscriber; telemetry is
    // pushed ONLY while subscribed (timer armed on subscribe, killed on
    // unsubscribe — spec §10.12).
    void subscribe(vw::ui::INotificationSink* sink);
    void unsubscribe();
    void syncUiSubscription();

    // Pull-on-open (spec §10.12): current telemetry, playback states,
    // monitors, library items, playlist (v1 single), assignments + config.
    vw::ui::UiSnapshot getUiSnapshot() const;

    void onUiTelemetryTick(); // 500 ms: telemetry push + library drain + config flush
    void pushPlaybackState(); // PlaybackStateNotification to the sink + tray tooltip
    void updateTrayFromState();
    std::wstring currentVideoName() const;

    // Command helpers.
    void playNext();
    void playPrevious();
    void setWallpaperFile(const std::wstring& path); // v1: single-item playlist
    void grabFrameSnapshotCommand();
    void applyConfigSetLive(const std::wstring& key, const std::wstring& value);
    void setStartWithWindows(bool on); // HKCU Run (no admin)
    std::wstring adapterName() const;
    void savePlaylistAndNotify();
    void pushWallpaperAssignment();
    void pushWallpaperAssignmentInto(
        std::vector<vw::ui::WallpaperAssignmentNotification>& out) const;
    static const wchar_t* scalingNameForLog(vw::ui::ScalingMode m);

    void shutdown();

    static constexpr UINT_PTR kWallpaperTimerId = 1; // 1 Hz Explorer-restart stub
    static constexpr UINT_PTR kWorkloadTimerId = 2;  // M9: ~2 s workload sampling
    static constexpr UINT_PTR kUiTelemetryTimerId = 3; // M11: 2 Hz while UI open

    HANDLE mutex_ = nullptr;
    std::filesystem::path appDataDir_;
    bool portableMode_ = false; // portable.ini detected next to exe
    std::unique_ptr<config::ConfigurationManager> config_;
    std::unique_ptr<wallpaper::WallpaperManager> wallpaper_;
    std::unique_ptr<playback::PlaybackController> playback_;
    std::unique_ptr<performance::StatsCollector> statsCollector_; // M6→M9 telemetry
    std::unique_ptr<performance::WorkloadMonitor> workloadMonitor_; // M9
    std::unique_ptr<detection::GameDetector> gameDetector_;        // M9
    detection::WindowState fullscreenState_ = detection::WindowState::Windowed; // M9
    std::unique_ptr<governor::ResourceGovernor> governor_;        // M10
    std::unique_ptr<system::SystemStateMonitor> systemMonitor_;    // M10
    std::unique_ptr<playlist::PlaylistManager> playlist_; // M7
    std::unique_ptr<ui::Win32UI> ui_;                     // M11: main window (lazy)
    std::unique_ptr<ui::TrayController> tray_;            // M11: tray (persists)
    std::unique_ptr<library::LibraryManager> library_;    // M11: minimal library
    std::unique_ptr<library::ThumbnailExtractor> thumbnails_; // #9: video frame previews
    std::mutex cmdMu_;                                    // M11: command queue
    std::deque<vw::ui::Command> commands_;
    vw::ui::INotificationSink* sink_ = nullptr;           // M11: single subscriber
    bool uiTelemetryRunning_ = false;                     // M11: telemetry timer state
    bool trayCreated_ = false;                            // M11: Shell_NotifyIcon added
    std::wstring adapterName_;                            // M11: Home panel field
    HWINEVENTHOOK winEventHook_ = nullptr; // M9: EVENT_SYSTEM_FOREGROUND (out-of-context)
    HWND lastForeground_ = nullptr; // M9: change detection cache
    bool systemMonitorStarted_ = false; // M10: notification registration state
    std::filesystem::path playlistPath_;                 // M7: AppData/playlist.json
    std::wstring lastPlayedPath_;                        // M7: same-item loop detection
    bool mfStarted_ = false;
    util::UpdateChecker updateChecker_;
    std::wstring commandLine_;
    ControlWindow control_;
};

} // namespace vw::app
