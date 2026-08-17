#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "logging/Logger.h"

using vw::log::Level;
using vw::log::Logger;

namespace {

std::filesystem::path uniqueLogDir() {
    std::random_device rd;
    const auto base = std::filesystem::temp_directory_path() / L"vw_test";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    for (int i = 0; i < 1000; ++i) {
        const auto dir = base / (L"log_" + std::to_wstring(rd()));
        if (std::filesystem::create_directories(dir, ec)) return dir;
    }
    return base;
}

std::wstring readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::wstring(std::istreambuf_iterator<char>(in), {});
}

} // namespace

TEST_CASE("logger: writes file and filters by level") {
    const auto dir = uniqueLogDir();
    {
        Logger log(Logger::Options{.logDir = dir, .level = Level::Info, .maxFileBytes = 1024 * 1024});
        log.info(L"hello {}", 42);
        log.debug(L"hidden at INFO");
        log.warn(L"warning");
    } // destructor flushes
    const auto text = readAll(dir / L"current.log");
    CHECK(text.find(L"hello 42") != std::wstring::npos);
    CHECK(text.find(L"warning") != std::wstring::npos);
    CHECK(text.find(L"hidden at INFO") == std::wstring::npos); // filtered
    CHECK(text.find(L"[INFO]") != std::wstring::npos);
}

TEST_CASE("logger: rotates to previous.log when size cap reached") {
    const auto dir = uniqueLogDir();
    {
        Logger log(Logger::Options{.logDir = dir, .level = Level::Debug, .maxFileBytes = 300});
        for (int i = 0; i < 200; ++i) {
            log.debug(L"padding line number {}", i);
        }
    }
    CHECK(std::filesystem::exists(dir / L"current.log"));
    CHECK(std::filesystem::exists(dir / L"previous.log"));
    const auto currentSize = std::filesystem::file_size(dir / L"current.log");
    CHECK(currentSize <= 300 * 2 + 512); // approximate cap + line slack
}

TEST_CASE("logger: empty logDir disables file sink") {
    Logger log(Logger::Options{.logDir = {}, .level = Level::Info});
    CHECK_NOTHROW(log.info(L"no file sink, must not crash"));
    log.flush();
}

TEST_CASE("logger: singleton reconfigurable") {
    auto& log = Logger::instance();
    const auto dir = uniqueLogDir();
    log.init(Logger::Options{.logDir = dir, .level = Level::Error});
    CHECK(log.level() == Level::Error);
    log.info(L"suppressed at ERROR");
    log.error(L"visible error");
    log.flush();
    const auto text = readAll(dir / L"current.log");
    CHECK(text.find(L"visible error") != std::wstring::npos);
    CHECK(text.find(L"suppressed at ERROR") == std::wstring::npos);
    // Reset to defaults so other tests are unaffected.
    log.init(Logger::Options{.logDir = {}, .level = Level::Info});
}
