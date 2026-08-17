#include "config/ConfigurationManager.h"

#include <fstream>
#include <iterator>
#include <sstream>

#include "util/utf8.h"

namespace vw::config {

namespace {

// Per-field read helpers: type-checked, clamped, enum whitelisted.
template <typename F>
void readInt(const util::Json& obj, const wchar_t* key, int def, int lo, int hi, F&& apply) {
    const auto& v = obj.get(key);
    if (v.isNumber()) {
        const int n = static_cast<int>(v.asInt(def));
        apply(n < lo ? lo : (n > hi ? hi : n));
    } else {
        apply(def);
    }
}

template <typename F>
void readBool(const util::Json& obj, const wchar_t* key, bool def, F&& apply) {
    const auto& v = obj.get(key);
    apply(v.isBool() ? v.asBool(def) : def);
}

void readStrings(const util::Json& obj, const wchar_t* key, std::vector<std::wstring>& out) {
    const auto& v = obj.get(key);
    if (!v.isArray()) return;
    for (const auto& e : v.asArray()) {
        if (e.isString()) out.push_back(e.asString());
    }
}

PlaybackMode playbackModeFrom(const std::wstring& s) {
    if (s == L"single") return PlaybackMode::Single;
    if (s == L"sequential") return PlaybackMode::Sequential;
    if (s == L"shuffle") return PlaybackMode::Shuffle;
    return PlaybackMode::Loop; // "loop" (and anything unknown) => loop playlist
}

ScalingMode scalingFrom(const std::wstring& s) {
    if (s == L"fit") return ScalingMode::Fit;
    if (s == L"stretch") return ScalingMode::Stretch;
    if (s == L"center") return ScalingMode::Center;
    return ScalingMode::Fill;
}

BatteryMode batteryFrom(const std::wstring& s) {
    if (s == L"continue") return BatteryMode::Continue;
    if (s == L"reduce") return BatteryMode::ReduceQuality;
    return BatteryMode::Pause;
}

WallpaperMode wallpaperModeFrom(const std::wstring& s) {
    if (s == L"clone") return WallpaperMode::Clone;
    return WallpaperMode::Independent; // "independent" (and anything unknown)
}

std::wstring wallpaperModeName(WallpaperMode m) {
    switch (m) {
        case WallpaperMode::Clone: return L"clone";
        case WallpaperMode::Independent: return L"independent";
    }
    return L"independent";
}

PerfMode perfFrom(const std::wstring& s) {
    if (s == L"performance") return PerfMode::Performance;
    if (s == L"quality") return PerfMode::Quality;
    if (s == L"ultra-low-resource" || s == L"ultralowresource") return PerfMode::UltraLowResource;
    return PerfMode::Balanced;
}

std::wstring playbackModeName(PlaybackMode m) {
    switch (m) {
        case PlaybackMode::Single: return L"single";
        case PlaybackMode::Sequential: return L"sequential";
        case PlaybackMode::Shuffle: return L"shuffle";
        case PlaybackMode::Loop: return L"loop";
    }
    return L"loop";
}
std::wstring scalingName(ScalingMode m) {
    switch (m) {
        case ScalingMode::Fit: return L"fit";
        case ScalingMode::Stretch: return L"stretch";
        case ScalingMode::Center: return L"center";
        case ScalingMode::Fill: return L"fill";
    }
    return L"fill";
}
std::wstring batteryName(BatteryMode m) {
    switch (m) {
        case BatteryMode::Continue: return L"continue";
        case BatteryMode::ReduceQuality: return L"reduce";
        case BatteryMode::Pause: return L"pause";
    }
    return L"pause";
}
std::wstring perfName(PerfMode m) {
    switch (m) {
        case PerfMode::Performance: return L"performance";
        case PerfMode::Quality: return L"quality";
        case PerfMode::UltraLowResource: return L"ultra-low-resource";
        case PerfMode::Balanced: return L"balanced";
    }
    return L"balanced";
}

} // namespace

ConfigurationManager::ConfigurationManager(Options opts) : opts_(std::move(opts)) {
    applyDefaults();
}

void ConfigurationManager::applyDefaults() {
    config_ = Config{};
}

void ConfigurationManager::readInto(Config& cfg, const util::Json& root) {
    const auto& general = root.get(L"general");
    const auto& playback = root.get(L"playback");
    const auto& wallpaper = root.get(L"wallpaper");
    const auto& perf = root.get(L"performance");
    const auto& battery = root.get(L"battery");
    const auto& detection = root.get(L"detection");

    readBool(general, L"startWithWindows", cfg.startWithWindows, [&](bool v) { cfg.startWithWindows = v; });
    readBool(general, L"minimizeToTray", cfg.minimizeToTray, [&](bool v) { cfg.minimizeToTray = v; });

    readBool(playback, L"shuffle", cfg.shuffle, [&](bool v) { cfg.shuffle = v; });
    readBool(playback, L"loop", cfg.loop, [&](bool v) { cfg.loop = v; });
    readBool(playback, L"audio", cfg.audio, [&](bool v) { cfg.audio = v; });
    readInt(playback, L"frameQueue", cfg.frameQueue, 1, 16, [&](int v) { cfg.frameQueue = v; });
    const auto& pathStr = playback.get(L"videoPath");
    if (pathStr.isString()) cfg.videoPath = pathStr.asString();
    const auto& modeStr = playback.get(L"mode");
    if (modeStr.isString()) cfg.mode = playbackModeFrom(modeStr.asString());
    const auto& scaleStr = playback.get(L"scaling");
    if (scaleStr.isString()) cfg.scaling = scalingFrom(scaleStr.asString());

    // M8: wallpaper mode (clone/independent).
    const auto& wmStr = wallpaper.get(L"mode");
    if (wmStr.isString()) cfg.wallpaperMode = wallpaperModeFrom(wmStr.asString());

    readBool(perf, L"pauseOnGame", cfg.pauseOnGame, [&](bool v) { cfg.pauseOnGame = v; });
    readBool(perf, L"pauseOnFullscreen", cfg.pauseOnFullscreen, [&](bool v) { cfg.pauseOnFullscreen = v; });
    readBool(perf, L"pauseOnHighCPU", cfg.pauseOnHighCPU, [&](bool v) { cfg.pauseOnHighCPU = v; });
    readBool(perf, L"pauseOnHighGPU", cfg.pauseOnHighGPU, [&](bool v) { cfg.pauseOnHighGPU = v; });
    readBool(perf, L"pauseOnHighRAM", cfg.pauseOnHighRAM, [&](bool v) { cfg.pauseOnHighRAM = v; });
    readInt(perf, L"cpuPauseThreshold", cfg.cpuPauseThreshold, 1, 99, [&](int v) { cfg.cpuPauseThreshold = v; });
    readInt(perf, L"cpuResumeThreshold", cfg.cpuResumeThreshold, 1, 99, [&](int v) { cfg.cpuResumeThreshold = v; });
    readInt(perf, L"gpuPauseThreshold", cfg.gpuPauseThreshold, 1, 99, [&](int v) { cfg.gpuPauseThreshold = v; });
    readInt(perf, L"gpuResumeThreshold", cfg.gpuResumeThreshold, 1, 99, [&](int v) { cfg.gpuResumeThreshold = v; });
    readInt(perf, L"memoryPauseThreshold", cfg.memoryPauseThreshold, 1, 99, [&](int v) { cfg.memoryPauseThreshold = v; });
    readInt(perf, L"memoryResumeThreshold", cfg.memoryResumeThreshold, 1, 99, [&](int v) { cfg.memoryResumeThreshold = v; });
    readInt(perf, L"pauseDelaySeconds", cfg.pauseDelaySeconds, 0, 60, [&](int v) { cfg.pauseDelaySeconds = v; });
    readInt(perf, L"resumeDelaySeconds", cfg.resumeDelaySeconds, 0, 60, [&](int v) { cfg.resumeDelaySeconds = v; });
    readInt(perf, L"longPauseReleaseSeconds", cfg.longPauseReleaseSeconds, 1, 3600, [&](int v) { cfg.longPauseReleaseSeconds = v; });
    readBool(perf, L"ultraLowResource", cfg.ultraLowResource, [&](bool v) { cfg.ultraLowResource = v; });
    const auto& perfStr = perf.get(L"perfMode");
    if (perfStr.isString()) cfg.perfMode = perfFrom(perfStr.asString());

    const auto& batteryStr = battery.get(L"mode");
    if (batteryStr.isString()) cfg.batteryMode = batteryFrom(batteryStr.asString());

    readStrings(detection, L"alwaysPause", cfg.alwaysPause);
    readStrings(detection, L"neverPause", cfg.neverPause);
}

bool ConfigurationManager::load() {
    std::error_code ec;
    if (!std::filesystem::exists(opts_.configPath, ec)) {
        // First run: write defaults so the user has a real config file.
        return save();
    }

    // Corrupt config (bad UTF-8 or bad JSON): back it up, fall back to defaults.
    auto backupAndReset = [&]() {
        auto bak = opts_.configPath;
        bak += L".bak"; // config.json -> config.json.bak
        std::error_code ec2;
        std::filesystem::remove(bak, ec2);
        std::filesystem::rename(opts_.configPath, bak, ec2);
        lastError_ = L"corrupt config backed up to " + bak.filename().wstring();
        applyDefaults();
        save();
    };

    std::string bytes;
    {
        std::ifstream in(opts_.configPath, std::ios::binary);
        if (!in) {
            lastError_ = L"cannot open config file";
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } // stream closed here so the rename below cannot hit a sharing violation

    auto decoded = util::utf8ToWide(bytes);
    if (!decoded) {
        // Invalid UTF-8 (e.g. ANSI file) — treated as corrupt.
        backupAndReset();
        return true;
    }
    auto parsed = util::Json::parse(*decoded);
    if (!parsed || !parsed->isObject()) {
        backupAndReset();
        return true;
    }

    applyDefaults();
    readInto(config_, *parsed);
    lastError_.clear();
    return true;
}

bool ConfigurationManager::save() const {
    util::Json::Object general{
        {L"startWithWindows", util::Json::boolean(config_.startWithWindows)},
        {L"minimizeToTray", util::Json::boolean(config_.minimizeToTray)},
    };
    util::Json::Object playback{
        {L"mode", util::Json::string(playbackModeName(config_.mode))},
        {L"shuffle", util::Json::boolean(config_.shuffle)},
        {L"loop", util::Json::boolean(config_.loop)},
        {L"scaling", util::Json::string(scalingName(config_.scaling))},
        {L"frameQueue", util::Json::number(static_cast<double>(config_.frameQueue))},
        {L"audio", util::Json::boolean(config_.audio)},
        {L"videoPath", util::Json::string(config_.videoPath)},
    };
    util::Json::Object wallpaper{
        {L"mode", util::Json::string(wallpaperModeName(config_.wallpaperMode))},
    };
    util::Json::Object perf{
        {L"pauseOnGame", util::Json::boolean(config_.pauseOnGame)},
        {L"pauseOnFullscreen", util::Json::boolean(config_.pauseOnFullscreen)},
        {L"pauseOnHighCPU", util::Json::boolean(config_.pauseOnHighCPU)},
        {L"pauseOnHighGPU", util::Json::boolean(config_.pauseOnHighGPU)},
        {L"pauseOnHighRAM", util::Json::boolean(config_.pauseOnHighRAM)},
        {L"cpuPauseThreshold", util::Json::number(static_cast<double>(config_.cpuPauseThreshold))},
        {L"cpuResumeThreshold", util::Json::number(static_cast<double>(config_.cpuResumeThreshold))},
        {L"gpuPauseThreshold", util::Json::number(static_cast<double>(config_.gpuPauseThreshold))},
        {L"gpuResumeThreshold", util::Json::number(static_cast<double>(config_.gpuResumeThreshold))},
        {L"memoryPauseThreshold", util::Json::number(static_cast<double>(config_.memoryPauseThreshold))},
        {L"memoryResumeThreshold", util::Json::number(static_cast<double>(config_.memoryResumeThreshold))},
        {L"pauseDelaySeconds", util::Json::number(static_cast<double>(config_.pauseDelaySeconds))},
        {L"resumeDelaySeconds", util::Json::number(static_cast<double>(config_.resumeDelaySeconds))},
        {L"perfMode", util::Json::string(perfName(config_.perfMode))},
        {L"ultraLowResource", util::Json::boolean(config_.ultraLowResource)},
        {L"longPauseReleaseSeconds", util::Json::number(static_cast<double>(config_.longPauseReleaseSeconds))},
    };
    util::Json::Array always;
    for (const auto& s : config_.alwaysPause) always.push_back(util::Json::string(s));
    util::Json::Array never;
    for (const auto& s : config_.neverPause) never.push_back(util::Json::string(s));
    util::Json::Object detection{
        {L"alwaysPause", util::Json::array(std::move(always))},
        {L"neverPause", util::Json::array(std::move(never))},
    };
    util::Json::Object root{
        {L"general", util::Json::object(std::move(general))},
        {L"playback", util::Json::object(std::move(playback))},
        {L"wallpaper", util::Json::object(std::move(wallpaper))},
        {L"performance", util::Json::object(std::move(perf))},
        {L"battery", util::Json::object({{L"mode", util::Json::string(batteryName(config_.batteryMode))}})},
        {L"detection", util::Json::object(std::move(detection))},
    };

    const std::wstring text = util::Json::object(std::move(root)).serialize();

    std::error_code ec;
    std::filesystem::create_directories(opts_.configPath.parent_path(), ec);
    auto tmp = opts_.configPath;
    tmp += L".tmp";
    {
        // UTF-8 storage via explicit conversion (wfstream is ANSI-codepage
        // dependent on write and would corrupt non-ASCII values — see BUILD_NOTES).
        std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out) {
            lastError_ = L"cannot write config";
            return false;
        }
        const std::string utf8 = util::wideToUtf8(text);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    std::filesystem::rename(tmp, opts_.configPath, ec);
    if (ec) {
        lastError_ = L"cannot rename config into place";
        return false;
    }
    return true;
}

} // namespace vw::config
