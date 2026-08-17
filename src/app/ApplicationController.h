#pragma once

#include <filesystem>
#include <memory>

#include "app/ControlWindow.h"
#include "config/ConfigurationManager.h"
#include "wallpaper/WallpaperManager.h"

namespace vw::video {
class VideoPlayer;
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
    void startPlayback();
    void onFrameTick();
    void shutdown();

    static constexpr UINT_PTR kWallpaperTimerId = 1; // 1 Hz Explorer-restart stub
    static constexpr UINT_PTR kFrameTimerId = 2;     // ~60 Hz frame pump (M4; M6 scheduler)

    HANDLE mutex_ = nullptr;
    std::filesystem::path appDataDir_;
    std::unique_ptr<config::ConfigurationManager> config_;
    std::unique_ptr<wallpaper::WallpaperManager> wallpaper_;
    std::unique_ptr<video::VideoPlayer> player_;
    bool mfStarted_ = false;
    ControlWindow control_;
};

} // namespace vw::app
