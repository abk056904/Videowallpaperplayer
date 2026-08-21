#pragma once

#include <chrono>
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
// M8: how the playlist maps onto monitors. Clone = one decoder + one timeline
// presented on every display; Independent = one decoder per display (only for
// N distinct videos; shared device/factory/shaders). Default = Independent
// (spec §125). On this single-display machine both reduce to one session.
enum class WallpaperMode { Independent, Clone };

// Validated runtime configuration (docs/02 §2.9 schema, v1 subset).
struct Config {
    // general
    bool startWithWindows = false;
    bool minimizeToTray = true;
    bool startMinimized = false; // #13: start minimized to tray (no UI flash)
    std::wstring logLevel = L"info"; // M11: Logger level (info|debug), persisted
    // playback
    PlaybackMode mode = PlaybackMode::Loop;
    bool shuffle = false;
    bool loop = true;
    ScalingMode scaling = ScalingMode::Fill;
    double playbackSpeed = 1.0; // playback speed multiplier (0.25–4.0)
    // Frame queue depth. Default 1: the decode worker is then consumer-paced
    // (blocks when the queue is full), so the queue can never hold two frames
    // that became due between two consumer wakes — stale-frame drops become
    // impossible by construction (measured 0.00/s vs ~3/s at depth 3; the
    // depth-3 decode-ahead of ~65 ms was the drop source). Decode-bound
    // playback behaves identically at any depth (the queue stays empty).
    // Raised (2..16) only when a jittery source needs buffering at the cost
    // of drops + latency.
    int frameQueue = 2;
    bool audio = true;    // §1.8: audio enabled by default (was false pre-v1.1)
    int volume = 80;      // 0–100 (mapped to WASAPI 0.0–10.0 range)
    std::wstring videoPath; // M4: single clip to play (empty = none, wallpaper shows checkerboard)
    // wallpaper (M8)
    WallpaperMode wallpaperMode = WallpaperMode::Independent;
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
    int longPauseReleaseSeconds = 2; // M14 P4: faster memory release on pause
    // battery
    BatteryMode batteryMode = BatteryMode::Pause;
    // file associations
    bool fileAssociations = false; // register .mp4 etc. to open with this app
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
    bool save(); // atomic: write temp, then rename over target

    const Config& config() const { return config_; }
    Config& config() { return config_; }
    const std::filesystem::path& configPath() const { return opts_.configPath; }

    const std::wstring& lastError() const { return lastError_; }

    // M10 (spec §9): the revision counter — bumped on every ACCEPTED config
    // change (thresholds/delays/modes) so the ResourceGovernor reacts to live
    // changes without polling. The UI calls markConfigChanged() after a
    // CONFIG_SET; the governor compares revision() to its cached value.
    uint64_t revision() const { return revision_; }
    void markConfigChanged() { ++revision_; }

    // M10 (spec §9): threshold-pair cross-validation — pause >= resume for
    // cpu/gpu/memory. Violations are clamped (resume pulled UP to pause) and
    // logged. Called at load and on every accepted CONFIG_SET.
    static void validateThresholdPairs(Config& cfg);

    // M11 (spec §10.10): pure key -> Config mapping for CONFIG_SET commands
    // (the UI/tray never touch Config directly). Keys: pauseOnGame /
    // pauseOnFullscreen / pauseOnHighCPU / pauseOnHighGPU / pauseOnHighRAM /
    // cpu|gpu|memory{Pause,Resume}Threshold / pauseDelaySeconds /
    // resumeDelaySeconds / longPauseReleaseSeconds / frameQueue /
    // batteryMode (continue|reduce|pause) / perfMode (performance|balanced|
    // quality|ultra-low-resource) / scaling (fill|fit|stretch|center) /
    // wallpaperMode (independent|clone) / playbackMode (single|sequential|
    // loop|shuffle) / loop / startWithWindows / minimizeToTray /
    // logLevel (info|debug). Booleans "true"/"false"/"1"/"0"; numbers are
    // decimal strings. Clamped like load; threshold pairs re-validated.
    // Returns false + a human-readable error for unknown keys/bad values.
    static bool applyConfigSet(Config& cfg, const std::wstring& key,
                               const std::wstring& value, std::wstring& error);

    // M11 (spec §9 / §10.10): debounced write-batching. CONFIG_SETs call
    // markDirty(); the app's low-frequency tick calls maybeFlushDirty(now),
    // which persists once the debounce window (kSaveDebounce = 5 s) has
    // passed since the LAST change. shutdown()'s save() is the final flush.
    // No per-click disk writes; UI edits survive a crash up to the debounce.
    // now = the change time (injectable for tests; defaults to the real clock).
    void markDirty(std::chrono::steady_clock::time_point now =
                   std::chrono::steady_clock::now());
    void maybeFlushDirty(std::chrono::steady_clock::time_point now);
    bool dirty() const { return dirty_; }
    static constexpr auto kSaveDebounce = std::chrono::milliseconds(5000); // M14 P5.2: 5 s debounce (was 1.5 s)

private:
    void applyDefaults();
    static void readInto(Config& cfg, const util::Json& root);

    Options opts_;
    Config config_;
    std::wstring lastError_; // written from save()
    uint64_t revision_ = 0; // M10: config revision counter (spec §9)
    bool dirty_ = false; // M11: unsaved change pending (write-batching)
    std::chrono::steady_clock::time_point lastChange_{}; // M11: debounce anchor
};

} // namespace vw::config
