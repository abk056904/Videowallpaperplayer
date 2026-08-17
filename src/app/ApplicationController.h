#pragma once

#include <filesystem>
#include <memory>

#include "app/ControlWindow.h"
#include "config/ConfigurationManager.h"
#include "detection/FullscreenDetector.h"
#include "detection/GameDetector.h"
#include "governor/ResourceGovernor.h"
#include "performance/StatsCollector.h"
#include "performance/WorkloadMonitor.h"
#include "playlist/PlaylistManager.h"
#include "system/SystemStateMonitor.h"
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
    void shutdown();

    static constexpr UINT_PTR kWallpaperTimerId = 1; // 1 Hz Explorer-restart stub
    static constexpr UINT_PTR kWorkloadTimerId = 2;  // M9: ~2 s workload sampling

    HANDLE mutex_ = nullptr;
    std::filesystem::path appDataDir_;
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
    HWINEVENTHOOK winEventHook_ = nullptr; // M9: EVENT_SYSTEM_FOREGROUND (out-of-context)
    HWND lastForeground_ = nullptr; // M9: change detection cache
    bool systemMonitorStarted_ = false; // M10: notification registration state
    std::filesystem::path playlistPath_;                 // M7: AppData/playlist.json
    std::wstring lastPlayedPath_;                        // M7: same-item loop detection
    bool mfStarted_ = false;
    ControlWindow control_;
};

} // namespace vw::app
