#pragma once

#include <filesystem>
#include <memory>

#include "app/ControlWindow.h"
#include "config/ConfigurationManager.h"
#include "performance/StatsCollector.h"
#include "playlist/PlaylistManager.h"
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
    void shutdown();

    static constexpr UINT_PTR kWallpaperTimerId = 1; // 1 Hz Explorer-restart stub

    HANDLE mutex_ = nullptr;
    std::filesystem::path appDataDir_;
    std::unique_ptr<config::ConfigurationManager> config_;
    std::unique_ptr<wallpaper::WallpaperManager> wallpaper_;
    std::unique_ptr<playback::PlaybackController> playback_;
    std::unique_ptr<performance::StatsCollector> statsCollector_; // M6→M9 telemetry
    std::unique_ptr<playlist::PlaylistManager> playlist_; // M7
    std::filesystem::path playlistPath_;                 // M7: AppData/playlist.json
    std::wstring lastPlayedPath_;                        // M7: same-item loop detection
    bool mfStarted_ = false;
    ControlWindow control_;
};

} // namespace vw::app
