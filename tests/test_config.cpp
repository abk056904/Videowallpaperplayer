#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <random>

#include "config/ConfigurationManager.h"

#include "util/utf8.h"

using vw::config::ConfigurationManager;
using vw::config::PerfMode;

namespace {

std::filesystem::path uniqueTempDir() {
    std::random_device rd;
    const auto base = std::filesystem::temp_directory_path() / L"vw_test";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    for (int i = 0; i < 1000; ++i) {
        const auto dir = base / (L"cfg_" + std::to_wstring(rd()));
        if (std::filesystem::create_directories(dir, ec)) return dir;
    }
    return base;
}

} // namespace

TEST_CASE("config: first run writes defaults") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    CHECK(mgr.load());

    CHECK(mgr.config().loop == true);
    CHECK(mgr.config().minimizeToTray == true);
    CHECK(mgr.config().gpuPauseThreshold == 90);
    CHECK(mgr.config().gpuResumeThreshold == 70);
    CHECK(mgr.config().batteryMode == vw::config::BatteryMode::Pause);
    CHECK(mgr.config().audio);  // §1.8: audio enabled by default

    // Defaults were persisted on first run.
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("config: round-trip load/save") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        ConfigurationManager mgr(ConfigurationManager::Options{path});
        REQUIRE(mgr.load());
        mgr.config().pauseOnHighGPU = false;
        mgr.config().cpuPauseThreshold = 70;
        mgr.config().frameQueue = 8;
        mgr.config().perfMode = PerfMode::Quality;
        mgr.config().alwaysPause = {L"game.exe", L"bench.exe"};
        CHECK(mgr.save());
    }
    {
        ConfigurationManager mgr(ConfigurationManager::Options{path});
        REQUIRE(mgr.load());
        CHECK(mgr.config().pauseOnHighGPU == false);
        CHECK(mgr.config().cpuPauseThreshold == 70);
        CHECK(mgr.config().frameQueue == 8);
        CHECK(mgr.config().perfMode == PerfMode::Quality);
        CHECK(mgr.config().alwaysPause.size() == 2);
        CHECK(mgr.config().alwaysPause[0] == L"game.exe");
    }
}

TEST_CASE("config: corrupt file backed up and defaults restored") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << "{ this is not json";
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    CHECK(mgr.load()); // must not fail; continue with defaults
    CHECK(std::filesystem::exists(dir / L"config.json.bak"));
    CHECK(mgr.config().loop == true);
    CHECK_FALSE(mgr.lastError().empty());
    // Defaults written again so the app has a usable file.
    CHECK(std::filesystem::exists(path));
}

TEST_CASE("config: values clamped and unknown keys ignored") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::wstring json =
            L"{ \"general\": { \"startWithWindows\": true },"
            L"  \"playback\": { \"frameQueue\": 9999, \"mode\": \"garbage\", \"scaling\": \"fit\" },"
            L"  \"wallpaper\": { \"mode\": \"clone\" },"
            L"  \"performance\": { \"cpuPauseThreshold\": -5, \"gpuPauseThreshold\": 150 },"
            L"  \"unknownSection\": { \"junk\": 1 } }";
        const std::string utf8 = vw::util::wideToUtf8(json);
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK(mgr.config().startWithWindows == true);
    CHECK(mgr.config().frameQueue == 16);        // clamped to max
    CHECK(mgr.config().mode == vw::config::PlaybackMode::Loop); // unknown -> default
    CHECK(mgr.config().scaling == vw::config::ScalingMode::Fit);
    CHECK(mgr.config().wallpaperMode == vw::config::WallpaperMode::Clone);
    CHECK(mgr.config().cpuPauseThreshold == 1);  // clamped to min
    CHECK(mgr.config().gpuPauseThreshold == 99); // clamped to max
}

TEST_CASE("config: wallpaper mode defaults to independent and round-trips") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load()); // no file -> defaults
    CHECK(mgr.config().wallpaperMode == vw::config::WallpaperMode::Independent);

    // Clone round-trips through save/load.
    mgr.config().wallpaperMode = vw::config::WallpaperMode::Clone;
    REQUIRE(mgr.save());
    ConfigurationManager again(ConfigurationManager::Options{path});
    REQUIRE(again.load());
    CHECK(again.config().wallpaperMode == vw::config::WallpaperMode::Clone);

    // Unknown mode name -> independent (default).
    {
        const std::wstring json = L"{ \"wallpaper\": { \"mode\": \"garbage\" } }";
        const std::string utf8 = vw::util::wideToUtf8(json);
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr2(ConfigurationManager::Options{path});
    REQUIRE(mgr2.load());
    CHECK(mgr2.config().wallpaperMode == vw::config::WallpaperMode::Independent);
}

TEST_CASE("config: wrong types fall back to defaults") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::string utf8 = vw::util::wideToUtf8(
            L"{ \"playback\": { \"loop\": \"yes\" }, \"performance\": { \"pauseDelaySeconds\": \"3\" } }");
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK(mgr.config().loop == true);                // string "yes" ignored -> default true
    CHECK(mgr.config().pauseDelaySeconds == 3);      // default kept
}

TEST_CASE("config: non-ASCII values round-trip through UTF-8 file") {
    // Real video paths can contain non-ASCII (é, CJK) — the UTF-8 save/load
    // path must round-trip them losslessly.
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    const std::wstring accent = L"caf\u00E9.exe";
    const std::wstring cjk = L"\u4E2D\u6587.exe";
    {
        ConfigurationManager mgr(ConfigurationManager::Options{path});
        REQUIRE(mgr.load());
        mgr.config().alwaysPause = {accent, cjk};
        CHECK(mgr.save());
    }
    {
        ConfigurationManager mgr(ConfigurationManager::Options{path});
        REQUIRE(mgr.load());
        REQUIRE(mgr.config().alwaysPause.size() == 2);
        CHECK(mgr.config().alwaysPause[0] == accent);
        CHECK(mgr.config().alwaysPause[1] == cjk);
    }
}

TEST_CASE("config: invalid UTF-8 file treated as corrupt") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        std::ofstream out(path, std::ios::binary);
        const char bad[] = {'{', static_cast<char>(0xE9), '}'}; // lone lead byte
        out.write(bad, sizeof(bad));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    CHECK(mgr.load()); // must not fail; corrupt-recovery path
    CHECK(std::filesystem::exists(dir / L"config.json.bak"));
    CHECK(mgr.config().loop == true); // defaults restored
    CHECK_FALSE(mgr.lastError().empty());
}

TEST_CASE("config: duplicate keys in file treated as corrupt (strict JSON)") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::string utf8 = vw::util::wideToUtf8(L"{\"playback\":{\"loop\":true,\"loop\":false}}");
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    CHECK(mgr.load());
    CHECK(std::filesystem::exists(dir / L"config.json.bak"));
    CHECK(mgr.config().loop == true); // default, not the ambiguous value
}

TEST_CASE("config: empty object and empty sections use defaults") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::string utf8 = vw::util::wideToUtf8(L"{}");
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK_FALSE(std::filesystem::exists(dir / L"config.json.bak")); // not corrupt
    CHECK(mgr.config().mode == vw::config::PlaybackMode::Loop);
    CHECK(mgr.config().cpuPauseThreshold == 85);
    CHECK(mgr.config().alwaysPause.empty());
}

TEST_CASE("config: enum strings are case-sensitive, unknown -> default") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::wstring json =
            L"{ \"playback\": { \"mode\": \"SEQUENTIAL\", \"scaling\": \"FIT\" },"
            L"  \"battery\": { \"mode\": \"CONTINUE\" },"
            L"  \"performance\": { \"perfMode\": \"QUALITY\" } }";
        const std::string utf8 = vw::util::wideToUtf8(json);
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK(mgr.config().mode == vw::config::PlaybackMode::Loop);       // "SEQUENTIAL" not matched
    CHECK(mgr.config().scaling == vw::config::ScalingMode::Fill);     // "FIT" not matched
    CHECK(mgr.config().batteryMode == vw::config::BatteryMode::Pause); // "CONTINUE" not matched
    CHECK(mgr.config().perfMode == PerfMode::Balanced);               // "QUALITY" not matched
}

TEST_CASE("config: thresholds clamp to min bound too") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::wstring json =
            L"{ \"playback\": { \"frameQueue\": 0 },"
            L"  \"performance\": { \"cpuPauseThreshold\": 0, \"pauseDelaySeconds\": -5,"
            L"                     \"longPauseReleaseSeconds\": 0 } }";
        const std::string utf8 = vw::util::wideToUtf8(json);
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK(mgr.config().frameQueue == 1);
    CHECK(mgr.config().cpuPauseThreshold == 1);
    CHECK(mgr.config().pauseDelaySeconds == 0);
    CHECK(mgr.config().longPauseReleaseSeconds == 1);
}

TEST_CASE("config: string-array fields keep only strings") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        const std::wstring json =
            L"{ \"detection\": { \"alwaysPause\": [1, \"a.exe\", true, \"b.exe\", null] } }";
        const std::string utf8 = vw::util::wideToUtf8(json);
        std::ofstream out(path, std::ios::binary);
        out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    REQUIRE(mgr.config().alwaysPause.size() == 2);
    CHECK(mgr.config().alwaysPause[0] == L"a.exe");
    CHECK(mgr.config().alwaysPause[1] == L"b.exe");
}

TEST_CASE("config: full-field round-trip") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        ConfigurationManager mgr(ConfigurationManager::Options{path});
        REQUIRE(mgr.load());
        auto& c = mgr.config();
        c.startWithWindows = true;
        c.minimizeToTray = false;
        c.mode = vw::config::PlaybackMode::Shuffle;
        c.shuffle = true;
        c.loop = false;
        c.scaling = vw::config::ScalingMode::Center;
        c.frameQueue = 6;
        c.audio = true;
        c.pauseOnGame = false;
        c.pauseOnFullscreen = false;
        c.pauseOnHighCPU = false;
        c.pauseOnHighGPU = false;
        c.pauseOnHighRAM = true;
        c.cpuPauseThreshold = 50;
        c.cpuResumeThreshold = 40;
        c.gpuPauseThreshold = 70;
        c.gpuResumeThreshold = 60;
        c.memoryPauseThreshold = 80;
        c.memoryResumeThreshold = 70;
        c.pauseDelaySeconds = 4;
        c.resumeDelaySeconds = 6;
        c.perfMode = PerfMode::UltraLowResource;
        c.ultraLowResource = true;
        c.longPauseReleaseSeconds = 30;
        c.batteryMode = vw::config::BatteryMode::Continue;
        c.alwaysPause = {L"game.exe"};
        c.neverPause = {L"editor.exe", L"bench.exe"};
        CHECK(mgr.save());
    }
    {
        ConfigurationManager mgr(ConfigurationManager::Options{path});
        REQUIRE(mgr.load());
        const auto& c = mgr.config();
        CHECK(c.startWithWindows == true);
        CHECK(c.minimizeToTray == false);
        CHECK(c.mode == vw::config::PlaybackMode::Shuffle);
        CHECK(c.shuffle == true);
        CHECK(c.loop == false);
        CHECK(c.scaling == vw::config::ScalingMode::Center);
        CHECK(c.frameQueue == 6);
        CHECK(c.audio == true);
        CHECK(c.pauseOnGame == false);
        CHECK(c.pauseOnFullscreen == false);
        CHECK(c.pauseOnHighCPU == false);
        CHECK(c.pauseOnHighGPU == false);
        CHECK(c.pauseOnHighRAM == true);
        CHECK(c.cpuPauseThreshold == 50);
        CHECK(c.cpuResumeThreshold == 40);
        CHECK(c.gpuPauseThreshold == 70);
        CHECK(c.gpuResumeThreshold == 60);
        CHECK(c.memoryPauseThreshold == 80);
        CHECK(c.memoryResumeThreshold == 70);
        CHECK(c.pauseDelaySeconds == 4);
        CHECK(c.resumeDelaySeconds == 6);
        CHECK(c.perfMode == PerfMode::UltraLowResource);
        CHECK(c.ultraLowResource == true);
        CHECK(c.longPauseReleaseSeconds == 30);
        CHECK(c.batteryMode == vw::config::BatteryMode::Continue);
        CHECK(c.alwaysPause.size() == 1);
        CHECK(c.alwaysPause[0] == L"game.exe");
        CHECK(c.neverPause.size() == 2);
        CHECK(c.neverPause[1] == L"bench.exe");
    }
}

TEST_CASE("config: repeated saves produce identical bytes") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    // Reads must not outlive the save: the atomic rename over an open file hits
    // a Windows sharing violation (see BUILD_NOTES gotcha #1).
    const auto readBytes = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    mgr.config().alwaysPause = {L"game.exe"};
    CHECK(mgr.save());
    const std::string bytes1 = readBytes(path);
    CHECK(mgr.save());
    const std::string bytes2 = readBytes(path);
    CHECK(bytes1 == bytes2); // deterministic serialization (std::map order)
}

TEST_CASE("config: save fails cleanly when target is unwritable") {
    const auto dir = uniqueTempDir();
    const auto blocker = dir / L"blocker";
    {
        std::ofstream out(blocker); // a regular FILE where a directory is needed
        out << "x";
    }
    const auto path = blocker / L"config.json";
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    CHECK_FALSE(mgr.save());
    CHECK_FALSE(mgr.lastError().empty());
    CHECK_FALSE(mgr.load()); // I/O failure surfaces as false (per contract)
}

// ---- M11: debounced write-batching (spec §9) -------------------------------

TEST_CASE("config: markDirty debounces — no write before the window, one after") {
    using namespace std::chrono_literals;
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load()); // first run writes defaults

    auto t = std::chrono::steady_clock::now();
    mgr.markDirty(t);
    CHECK(mgr.dirty());

    // Inside the 5 s debounce window: nothing written.
    t += 2s;
    mgr.maybeFlushDirty(t);
    CHECK(mgr.dirty());

    // Past the window: saved + flag cleared.
    t += 4s;
    mgr.maybeFlushDirty(t);
    CHECK_FALSE(mgr.dirty());

    // A second change re-arms the debounce (the LAST change anchors the clock).
    mgr.markDirty(t);
    t += 2s;
    mgr.maybeFlushDirty(t); // still inside the new window
    CHECK(mgr.dirty());
    t += 4s;
    mgr.maybeFlushDirty(t);
    CHECK_FALSE(mgr.dirty());
}

TEST_CASE("config: debounced flush persists the last state") {
    using namespace std::chrono_literals;
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());

    mgr.config().pauseOnHighRAM = true;
    mgr.markDirty();
    auto t = std::chrono::steady_clock::now() + 6s;
    mgr.maybeFlushDirty(t);

    // A FRESH manager reading the file must see the flushed value.
    ConfigurationManager again(ConfigurationManager::Options{path});
    REQUIRE(again.load());
    CHECK(again.config().pauseOnHighRAM);
}

// ---- M11: CONFIG_SET mapping (spec §10.10) --------------------------------

TEST_CASE("config: applyConfigSet maps, clamps, and validates pairs") {
    vw::config::Config cfg;
    std::wstring err;

    CHECK(ConfigurationManager::applyConfigSet(cfg, L"pauseOnGame", L"false", err));
    CHECK_FALSE(cfg.pauseOnGame);
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"pauseOnHighRAM", L"true", err));
    CHECK(cfg.pauseOnHighRAM);

    // Numbers: clamped like load (1..99).
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"cpuPauseThreshold", L"150", err));
    CHECK(cfg.cpuPauseThreshold == 99);
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"cpuResumeThreshold", L"0", err));
    CHECK(cfg.cpuResumeThreshold == 1);

    // Pair validation: resume > pause gets clamped UP to pause.
    cfg.cpuPauseThreshold = 80;
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"cpuResumeThreshold", L"90", err));
    CHECK(cfg.cpuResumeThreshold == 80);

    // Enums (case-insensitive values).
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"batteryMode", L"reduce", err));
    CHECK(cfg.batteryMode == vw::config::BatteryMode::ReduceQuality);
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"scaling", L"Stretch", err));
    CHECK(cfg.scaling == vw::config::ScalingMode::Stretch);
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"wallpaperMode", L"clone", err));
    CHECK(cfg.wallpaperMode == vw::config::WallpaperMode::Clone);
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"logLevel", L"debug", err));
    CHECK(cfg.logLevel == L"debug");
    CHECK(ConfigurationManager::applyConfigSet(cfg, L"loop", L"0", err));
    CHECK_FALSE(cfg.loop);

    // Bad key / bad value: rejected with a message.
    CHECK_FALSE(ConfigurationManager::applyConfigSet(cfg, L"nonsenseKey", L"1", err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(ConfigurationManager::applyConfigSet(cfg, L"pauseOnGame", L"maybe", err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("config: logLevel round-trips through save/load") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    mgr.config().logLevel = L"debug";
    REQUIRE(mgr.save());
    ConfigurationManager again(ConfigurationManager::Options{path});
    REQUIRE(again.load());
    CHECK(again.config().logLevel == L"debug");
}
