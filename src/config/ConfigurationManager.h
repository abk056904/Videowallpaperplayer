#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "util/json.h"

namespace vw::config {

enum class PlaybackMode { Single, Sequential, Loop, Shuffle };
enum class ScalingMode { Fill, Fit, Stretch, Center };
enum class BatteryMode { Continue, ReduceQuality, Pause };
enum class PerfMode { Performance, Balanced, Quality, UltraLowResource };

// Validated runtime configuration (docs/02 §2.9 schema, v1 subset).
struct Config {
    // general
    bool startWithWindows = false;
    bool minimizeToTray = true;
    // playback
    PlaybackMode mode = PlaybackMode::Loop;
    bool shuffle = false;
    bool loop = true;
    ScalingMode scaling = ScalingMode::Fill;
    int frameQueue = 3;
    bool audio = false;
    std::wstring videoPath; // M4: single clip to play (empty = none, wallpaper shows checkerboard)
    // performance
    bool pauseOnGame = true;
    bool pauseOnFullscreen = true;
    bool pauseOnHighCPU = true;
    bool pauseOnHighGPU = true;
    bool pauseOnHighRAM = false;
    int cpuPauseThreshold = 85, cpuResumeThreshold = 65;
    int gpuPauseThreshold = 90, gpuResumeThreshold = 70;
    int memoryPauseThreshold = 90, memoryResumeThreshold = 75;
    int pauseDelaySeconds = 3, resumeDelaySeconds = 5;
    PerfMode perfMode = PerfMode::Balanced;
    bool ultraLowResource = false;
    int longPauseReleaseSeconds = 5;
    // battery
    BatteryMode batteryMode = BatteryMode::Pause;
    // detection
    std::vector<std::wstring> alwaysPause;
    std::vector<std::wstring> neverPause;
};

// Loads/validates/persists config as JSON in the per-user AppData folder.
// Never writes next to the executable. Corrupt file => .bak backup + defaults.
class ConfigurationManager {
public:
    struct Options {
        std::filesystem::path configPath; // full path to config.json
    };

    explicit ConfigurationManager(Options opts);

    // Returns false only on I/O failure (e.g. cannot create dir). Missing or
    // corrupt files are handled per policy and startup continues.
    bool load();
    bool save() const; // atomic: write temp, then rename over target

    const Config& config() const { return config_; }
    Config& config() { return config_; }

    const std::wstring& lastError() const { return lastError_; }

private:
    void applyDefaults();
    static void readInto(Config& cfg, const util::Json& root);

    Options opts_;
    Config config_;
    mutable std::wstring lastError_; // written from const save()
};

} // namespace vw::config
