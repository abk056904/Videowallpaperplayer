#pragma once

#include <filesystem>
#include <memory>

#include "app/ControlWindow.h"
#include "config/ConfigurationManager.h"
#include "wallpaper/WallpaperManager.h"

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
    void shutdown();

    static constexpr UINT_PTR kWallpaperTimerId = 1;

    HANDLE mutex_ = nullptr;
    std::filesystem::path appDataDir_;
    std::unique_ptr<config::ConfigurationManager> config_;
    std::unique_ptr<wallpaper::WallpaperManager> wallpaper_;
    ControlWindow control_;
};

} // namespace vw::app
