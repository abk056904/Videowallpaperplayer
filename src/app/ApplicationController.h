#pragma once

#include <filesystem>
#include <memory>

#include "app/ControlWindow.h"
#include "config/ConfigurationManager.h"

namespace vw::app {

// Owns the process: single-instance guard, paths, control window, config,
// logger wiring, message loop, and the deterministic shutdown sequence
// (docs/03 §3.17, M1 subset — more subsystems attach in later milestones).
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

    HANDLE mutex_ = nullptr;
    std::filesystem::path appDataDir_;
    std::unique_ptr<config::ConfigurationManager> config_;
    ControlWindow control_;
};

} // namespace vw::app
