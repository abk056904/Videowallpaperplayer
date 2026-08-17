#include "doctest.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#include "logging/Logger.h"

#include "util/utf8.h"

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
    const std::string bytes(std::istreambuf_iterator<char>(in), {});
    auto wide = vw::util::utf8ToWide(bytes);
    return wide ? *wide : std::wstring{};
}

size_t countOccurrences(const std::wstring& haystack, const std::wstring& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::wstring::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
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

TEST_CASE("logger: level filtering boundaries") {
    const auto dir = uniqueLogDir();
    {
        Logger log(Logger::Options{.logDir = dir, .level = Level::Warn});
        log.trace(L"t");
        log.debug(L"d");
        log.info(L"i");
        log.warn(L"w");
        log.error(L"e");
        log.fatal(L"f");
    }
    const auto text = readAll(dir / L"current.log");
    // At WARN: only warn/error/fatal survive.
    CHECK(text.find(L"[WARN] w") != std::wstring::npos);
    CHECK(text.find(L"[ERROR] e") != std::wstring::npos);
    CHECK(text.find(L"[FATAL] f") != std::wstring::npos);
    CHECK(text.find(L"[INFO] i") == std::wstring::npos);
    CHECK(text.find(L"[DEBUG] d") == std::wstring::npos);
    CHECK(text.find(L"[TRACE] t") == std::wstring::npos);
}

TEST_CASE("logger: non-ASCII messages survive the UTF-8 sink") {
    const auto dir = uniqueLogDir();
    {
        Logger log(Logger::Options{.logDir = dir, .level = Level::Info});
        log.info(L"caf\u00E9 \u4E2D\u6587 path {}", L"\U0001F600.mp4");
    }
    const auto text = readAll(dir / L"current.log");
    CHECK(text.find(L"caf\u00E9 \u4E2D\u6587 path ") != std::wstring::npos);
    CHECK(text.find(L"\U0001F600.mp4") != std::wstring::npos);
}

TEST_CASE("logger: thread-safe concurrent writes") {
    const auto dir = uniqueLogDir();
    constexpr int kThreads = 4;
    constexpr int kLines = 500;
    {
        Logger log(Logger::Options{.logDir = dir, .level = Level::Info,
                                   .maxFileBytes = 64ull * 1024 * 1024}); // no rotation
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&log, t] {
                for (int i = 0; i < kLines; ++i) log.info(L"thread {} line {}", t, i);
            });
        }
        for (auto& th : threads) th.join();
    }
    const auto text = readAll(dir / L"current.log");
    // Every line present exactly once: no interleaving loss, no duplication.
    CHECK(countOccurrences(text, L"[INFO]") == kThreads * kLines);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kLines; ++i) {
            // Anchor with '\n' so "line 1" doesn't match inside "line 10..19" etc.
            CHECK(countOccurrences(text, std::format(L"thread {} line {}\n", t, i)) == 1);
        }
    }
}

TEST_CASE("logger: repeated rotation keeps current.log bounded") {
    const auto dir = uniqueLogDir();
    {
        Logger log(Logger::Options{.logDir = dir, .level = Level::Debug, .maxFileBytes = 512});
        for (int i = 0; i < 5000; ++i) {
            log.debug(L"padding line number {}", i); // >> several rotations
        }
    }
    // After many rotations: current.log exists, is bounded, previous.log kept.
    CHECK(std::filesystem::exists(dir / L"current.log"));
    CHECK(std::filesystem::exists(dir / L"previous.log"));
    CHECK(std::filesystem::file_size(dir / L"current.log") <= 512 * 2 + 512);
}
