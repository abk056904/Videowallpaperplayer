#include "config/ConfigurationManager.h"

#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "logging/Logger.h"
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

template <typename F>
void readDouble(const util::Json& obj, const wchar_t* key, double def, double lo, double hi, F&& apply) {
    const auto& v = obj.get(key);
    if (v.isNumber()) {
        double n = v.asNumber(def);
        if (n < lo) n = lo;
        if (n > hi) n = hi;
        apply(n);
    } else {
        apply(def);
    }
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
    readBool(general, L"startMinimized", cfg.startMinimized, [&](bool v) { cfg.startMinimized = v; });
    readBool(general, L"fileAssociations", cfg.fileAssociations, [&](bool v) { cfg.fileAssociations = v; });
    const auto& logLevelStr = general.get(L"logLevel");
    if (logLevelStr.isString()) {
        const auto s = logLevelStr.asString();
        if (s == L"debug" || s == L"info" || s == L"warn" || s == L"error") {
            cfg.logLevel = s;
        }
    }

    readDouble(playback, L"playbackSpeed", cfg.playbackSpeed, 0.25, 4.0, [&](double v) { cfg.playbackSpeed = v; });
    readBool(playback, L"shuffle", cfg.shuffle, [&](bool v) { cfg.shuffle = v; });
    readBool(playback, L"loop", cfg.loop, [&](bool v) { cfg.loop = v; });
    readBool(playback, L"audio", cfg.audio, [&](bool v) { cfg.audio = v; });
    readInt(playback, L"volume", cfg.volume, 0, 100, [&](int v) { cfg.volume = v; });
    // #18: per-monitor volume overrides (map of monitorId -> volume 0-100).
    const auto& pmvObj = playback.get(L"perMonitorVolume");
    if (pmvObj.isObject()) {
        for (const auto& [key, val] : pmvObj.asObject()) {
            if (val.isNumber()) {
                int v = static_cast<int>(val.asInt(80));
                cfg.perMonitorVolume[key] = v < 0 ? 0 : (v > 100 ? 100 : v);
            }
        }
    }
    // #23: per-monitor scaling overrides.
    const auto& pmsObj = playback.get(L"perMonitorScaling");
    if (pmsObj.isObject()) {
        for (const auto& [key, val] : pmsObj.asObject()) {
            if (val.isString()) {
                cfg.perMonitorScaling[key] = scalingFrom(val.asString());
            }
        }
    }
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
    // M10 (spec §9): threshold-pair cross-validation at load — pause >= resume
    // for cpu/gpu/memory; violations clamped + logged.
    validateThresholdPairs(config_);
    lastError_.clear();
    return true;
}

bool ConfigurationManager::save() {
    dirty_ = false; // a successful (or attempted) flush clears the flag; a
                    // failed write keeps the error in lastError_ for logging
    util::Json::Object general{
        {L"startWithWindows", util::Json::boolean(config_.startWithWindows)},
        {L"minimizeToTray", util::Json::boolean(config_.minimizeToTray)},
        {L"startMinimized", util::Json::boolean(config_.startMinimized)},
        {L"fileAssociations", util::Json::boolean(config_.fileAssociations)},
        {L"logLevel", util::Json::string(config_.logLevel)},
    };
    util::Json::Object playback{
        {L"mode", util::Json::string(playbackModeName(config_.mode))},
        {L"shuffle", util::Json::boolean(config_.shuffle)},
        {L"loop", util::Json::boolean(config_.loop)},
        {L"scaling", util::Json::string(scalingName(config_.scaling))},
        {L"playbackSpeed", util::Json::number(config_.playbackSpeed)},
        {L"frameQueue", util::Json::number(static_cast<double>(config_.frameQueue))},
        {L"audio", util::Json::boolean(config_.audio)},
        {L"volume", util::Json::number(static_cast<double>(config_.volume))},
        {L"videoPath", util::Json::string(config_.videoPath)},
    };
    // #18: per-monitor volume overrides.
    if (!config_.perMonitorVolume.empty()) {
        util::Json::Object pmvObj;
        for (const auto& [id, vol] : config_.perMonitorVolume) {
            pmvObj[id] = util::Json::number(static_cast<double>(vol));
        }
        playback[L"perMonitorVolume"] = util::Json::object(std::move(pmvObj));
    }
    // #23: per-monitor scaling overrides.
    if (!config_.perMonitorScaling.empty()) {
        util::Json::Object pmsObj;
        for (const auto& [id, mode] : config_.perMonitorScaling) {
            pmsObj[id] = util::Json::string(scalingName(mode));
        }
        playback[L"perMonitorScaling"] = util::Json::object(std::move(pmsObj));
    }
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
            dirty_ = true; // the change is still unsaved — retry next flush
            return false;
        }
        const std::string utf8 = util::wideToUtf8(text);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    std::filesystem::rename(tmp, opts_.configPath, ec);
    if (ec) {
        lastError_ = L"cannot rename config into place";
        dirty_ = true; // the change is still unsaved — retry next flush
        return false;
    }
    return true;
}

void ConfigurationManager::markDirty(std::chrono::steady_clock::time_point now) {
    dirty_ = true;
    lastChange_ = now; // the LAST change anchors the debounce window
}

void ConfigurationManager::maybeFlushDirty(std::chrono::steady_clock::time_point now) {
    if (!dirty_) {
        return;
    }
    if (now - lastChange_ < kSaveDebounce) {
        return; // debounce: wait 1.5 s after the LAST change
    }
    if (!save()) {
        log::Logger::instance().warn(L"config debounced save failed: {}", lastError_);
    }
}

void ConfigurationManager::validateThresholdPairs(Config& cfg) {
    auto& log = log::Logger::instance();
    const auto clampPair = [&](int& pause, int& resume, const wchar_t* name) {
        if (pause < resume) {
            log.warn(L"config: {} pause threshold ({}) < resume threshold ({}) — "
                     L"resume clamped up to pause",
                     name, pause, resume);
            resume = pause; // resume pulled UP to pause (never pause > resume)
        }
    };
    clampPair(cfg.cpuPauseThreshold, cfg.cpuResumeThreshold, L"cpu");
    clampPair(cfg.gpuPauseThreshold, cfg.gpuResumeThreshold, L"gpu");
    clampPair(cfg.memoryPauseThreshold, cfg.memoryResumeThreshold, L"memory");
}

bool ConfigurationManager::applyConfigSet(Config& cfg, const std::wstring& key,
                                          const std::wstring& value, std::wstring& error) {
    // Normalized (lowercased) key; values are matched case-insensitively too.
    auto lower = [](std::wstring s) {
        for (wchar_t& c : s) {
            c = static_cast<wchar_t>(::towlower(c));
        }
        return s;
    };
    const std::wstring k = lower(key);
    const std::wstring v = lower(value);

    auto parseBool = [&](bool& out) -> bool {
        if (v == L"true" || v == L"1" || v == L"yes") {
            out = true;
            return true;
        }
        if (v == L"false" || v == L"0" || v == L"no") {
            out = false;
            return true;
        }
        error = L"expected true/false for '" + key + L"', got '" + value + L"'";
        return false;
    };
    auto parseInt = [&](int& out, int lo, int hi) -> bool {
        try {
            const int n = std::stoi(value);
            out = n < lo ? lo : (n > hi ? hi : n); // clamped like load
            return true;
        } catch (...) {
            error = L"expected an integer for '" + key + L"', got '" + value + L"'";
            return false;
        }
    };
    auto parseEnum = [&](const std::wstring& a, const std::wstring& b, const std::wstring& c,
                         const std::wstring& d, int& out) -> bool {
        if (v == a) { out = 0; return true; }
        if (v == b) { out = 1; return true; }
        if (v == c) { out = 2; return true; }
        if (v == d) { out = 3; return true; }
        error = L"unknown value '" + value + L"' for '" + key + L"'";
        return false;
    };

    bool ok = true;
    if (k == L"pauseongame") { ok = parseBool(cfg.pauseOnGame); }
    else if (k == L"pauseonfullscreen") { ok = parseBool(cfg.pauseOnFullscreen); }
    else if (k == L"pauseonhighcpu") { ok = parseBool(cfg.pauseOnHighCPU); }
    else if (k == L"pauseonhighgpu") { ok = parseBool(cfg.pauseOnHighGPU); }
    else if (k == L"pauseonhighram") { ok = parseBool(cfg.pauseOnHighRAM); }
    else if (k == L"cpupausethreshold") { ok = parseInt(cfg.cpuPauseThreshold, 1, 99); }
    else if (k == L"cpuresumethreshold") { ok = parseInt(cfg.cpuResumeThreshold, 1, 99); }
    else if (k == L"gpupausethreshold") { ok = parseInt(cfg.gpuPauseThreshold, 1, 99); }
    else if (k == L"gpuresumethreshold") { ok = parseInt(cfg.gpuResumeThreshold, 1, 99); }
    else if (k == L"memorypausethreshold") { ok = parseInt(cfg.memoryPauseThreshold, 1, 99); }
    else if (k == L"memoryresumethreshold") { ok = parseInt(cfg.memoryResumeThreshold, 1, 99); }
    else if (k == L"pausedelayseconds") { ok = parseInt(cfg.pauseDelaySeconds, 0, 60); }
    else if (k == L"resumedelayseconds") { ok = parseInt(cfg.resumeDelaySeconds, 0, 60); }
    else if (k == L"longpausereleaseseconds") { ok = parseInt(cfg.longPauseReleaseSeconds, 1, 3600); }
    else if (k == L"framequeue") { ok = parseInt(cfg.frameQueue, 1, 16); }
    else if (k == L"batterymode") {
        int m = 0;
        ok = parseEnum(L"continue", L"reduce", L"pause", L"", m);
        if (ok) { cfg.batteryMode = static_cast<BatteryMode>(m); }
    }
    else if (k == L"perfmode") {
        int m = 0;
        if (v == L"performance") { m = 0; }
        else if (v == L"balanced") { m = 1; }
        else if (v == L"quality") { m = 2; }
        else if (v == L"ultra-low-resource" || v == L"ultralowresource") { m = 3; }
        else { error = L"unknown value '" + value + L"' for '" + key + L"'"; ok = false; }
        if (ok) { cfg.perfMode = static_cast<PerfMode>(m); }
    }
    else if (k == L"scaling") {
        int m = 0;
        if (v == L"fill") { m = 0; }
        else if (v == L"fit") { m = 1; }
        else if (v == L"stretch") { m = 2; }
        else if (v == L"center") { m = 3; }
        else { error = L"unknown value '" + value + L"' for '" + key + L"'"; ok = false; }
        if (ok) { cfg.scaling = static_cast<ScalingMode>(m); }
    }
    else if (k == L"playbackspeed") {
        try {
            double spd = std::stod(value);
            if (spd < 0.25) spd = 0.25;
            if (spd > 4.0) spd = 4.0;
            cfg.playbackSpeed = spd;
        } catch (...) {
            error = L"expected a number for '" + key + L"', got '" + value + L"'";
            ok = false;
        }
    }
    else if (k == L"wallpapermode") {
        if (v == L"clone") { cfg.wallpaperMode = WallpaperMode::Clone; }
        else if (v == L"independent") { cfg.wallpaperMode = WallpaperMode::Independent; }
        else { error = L"unknown value '" + value + L"' for '" + key + L"'"; ok = false; }
    }
    else if (k == L"playbackmode" || k == L"mode") {
        int m = 0;
        ok = parseEnum(L"single", L"sequential", L"loop", L"shuffle", m);
        if (ok) { cfg.mode = static_cast<PlaybackMode>(m); }
    }
    else if (k == L"loop") { ok = parseBool(cfg.loop); }
    else if (k == L"audio") { ok = parseBool(cfg.audio); }
    else if (k == L"volume") { ok = parseInt(cfg.volume, 0, 100); }
    else if (k.rfind(L"pmvolume:", 0) == 0) {
        // #18: perMonitorVolume:<monitorId>:<volume>
        // The key after 'pmvolume:' is the monitor id, the value is the volume.
        std::wstring monitorId = key.substr(9); // skip "pmvolume:"
        int vol = 0;
        if (parseInt(vol, 0, 100)) {
            cfg.perMonitorVolume[monitorId] = vol;
        }
    }
    else if (k.rfind(L"pmscaling:", 0) == 0) {
        // #23: perMonitorScaling:<monitorId>:<fill|fit|stretch|center>
        std::wstring monitorId = key.substr(10); // skip "pmscaling:"
        int m = -1;
        if (v == L"fill") { m = 0; }
        else if (v == L"fit") { m = 1; }
        else if (v == L"stretch") { m = 2; }
        else if (v == L"center") { m = 3; }
        if (m >= 0) {
            cfg.perMonitorScaling[monitorId] = static_cast<ScalingMode>(m);
        } else {
            error = L"unknown scaling '" + value + L"' for '" + key + L"'";
            ok = false;
        }
    }
    else if (k == L"startwithwindows") { ok = parseBool(cfg.startWithWindows); }
    else if (k == L"minimizetotray") { ok = parseBool(cfg.minimizeToTray); }
    else if (k == L"startminimized") { ok = parseBool(cfg.startMinimized); }
    else if (k == L"fileassociations") { ok = parseBool(cfg.fileAssociations); }
    else if (k == L"loglevel") {
        if (v == L"info") { cfg.logLevel = L"info"; }
        else if (v == L"debug") { cfg.logLevel = L"debug"; }
        else if (v == L"warn") { cfg.logLevel = L"warn"; }
        else if (v == L"error") { cfg.logLevel = L"error"; }
        else { error = L"unknown log level '" + value + L"'"; ok = false; }
    }
    else {
        error = L"unknown config key '" + key + L"'";
        ok = false;
    }
    if (!ok) {
        return false;
    }
    // Threshold pairs re-validated on every accepted change (spec §9).
    validateThresholdPairs(cfg);
    return true;
}

} // namespace vw::config
