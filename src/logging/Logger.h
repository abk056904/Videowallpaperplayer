#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

namespace vw::log {

enum class Level { Trace, Debug, Info, Warn, Error, Fatal };

const wchar_t* levelName(Level l);

// Aggregate-event logger (docs/02 §2.10): rotating file sink, bounded size,
// thread-safe, low-contention. Release default INFO; TRACE is compiled to
// Debug-only usage by convention (call sites gate on level()).
class Logger {
public:
    struct Options {
        std::filesystem::path logDir; // empty => no file sink (default for tests)
        Level level = Level::Info;
        uint64_t maxFileBytes = 5ull * 1024 * 1024; // current.log rotates to previous.log
    };

    explicit Logger(Options opts = {});
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // (Re)configure an existing logger (used by the singleton at startup).
    void init(Options opts);

    Level level() const { return level_.load(); }
    void setLevel(Level l) { level_.store(l); }
    bool enabled(Level l) const { return static_cast<int>(l) >= static_cast<int>(level_.load()); }

    void log(Level l, std::wstring msg);
    void flush();

    template <typename... Args>
    void trace(std::wformat_string<Args...> fmt, Args&&... args) {
        if (enabled(Level::Trace)) log(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void debug(std::wformat_string<Args...> fmt, Args&&... args) {
        if (enabled(Level::Debug)) log(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void info(std::wformat_string<Args...> fmt, Args&&... args) {
        if (enabled(Level::Info)) log(Level::Info, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void warn(std::wformat_string<Args...> fmt, Args&&... args) {
        if (enabled(Level::Warn)) log(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void error(std::wformat_string<Args...> fmt, Args&&... args) {
        if (enabled(Level::Error)) log(Level::Error, std::format(fmt, std::forward<Args>(args)...));
    }
    template <typename... Args>
    void fatal(std::wformat_string<Args...> fmt, Args&&... args) {
        if (enabled(Level::Fatal)) log(Level::Fatal, std::format(fmt, std::forward<Args>(args)...));
    }

    static Logger& instance(); // process-wide default; reconfigure via init()

private:
    void openFile();
    void rotateLocked();
    std::wstring timestamp() const;

    std::filesystem::path dir_;
    std::filesystem::path filePath_;
    uint64_t maxBytes_ = 5ull * 1024 * 1024;
    uint64_t bytes_ = 0;
    std::mutex mu_;
    std::wofstream file_; // UTF-8 sink (MSVC wfstream converts wchar_t via CRT codecvt)
    std::atomic<Level> level_{Level::Info};
};

} // namespace vw::log
