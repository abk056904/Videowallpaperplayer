#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <random>

#include "config/ConfigurationManager.h"

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
    CHECK_FALSE(mgr.config().audio);

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
        std::wofstream out(path, std::ios::binary);
        out << L"{ this is not json";
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
        std::wofstream out(path, std::ios::binary);
        out << L"{ \"general\": { \"startWithWindows\": true },"
               L"  \"playback\": { \"frameQueue\": 9999, \"mode\": \"garbage\", \"scaling\": \"fit\" },"
               L"  \"performance\": { \"cpuPauseThreshold\": -5, \"gpuPauseThreshold\": 150 },"
               L"  \"unknownSection\": { \"junk\": 1 } }";
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK(mgr.config().startWithWindows == true);
    CHECK(mgr.config().frameQueue == 16);        // clamped to max
    CHECK(mgr.config().mode == vw::config::PlaybackMode::Loop); // unknown -> default
    CHECK(mgr.config().scaling == vw::config::ScalingMode::Fit);
    CHECK(mgr.config().cpuPauseThreshold == 1);  // clamped to min
    CHECK(mgr.config().gpuPauseThreshold == 99); // clamped to max
}

TEST_CASE("config: wrong types fall back to defaults") {
    const auto dir = uniqueTempDir();
    const auto path = dir / L"config.json";
    {
        std::wofstream out(path, std::ios::binary);
        out << L"{ \"playback\": { \"loop\": \"yes\" }, \"performance\": { \"pauseDelaySeconds\": \"3\" } }";
    }
    ConfigurationManager mgr(ConfigurationManager::Options{path});
    REQUIRE(mgr.load());
    CHECK(mgr.config().loop == true);                // string "yes" ignored -> default true
    CHECK(mgr.config().pauseDelaySeconds == 3);      // default kept
}
