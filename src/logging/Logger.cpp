#include "logging/Logger.h"

#include <windows.h>

#include "util/utf8.h"

namespace vw::log {

const wchar_t* levelName(Level l) {
    switch (l) {
        case Level::Trace: return L"TRACE";
        case Level::Debug: return L"DEBUG";
        case Level::Info: return L"INFO";
        case Level::Warn: return L"WARN";
        case Level::Error: return L"ERROR";
        case Level::Fatal: return L"FATAL";
    }
    return L"????";
}

Logger::Logger(Options opts) {
    init(std::move(opts));
}

Logger::~Logger() {
    flush();
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(Options opts) {
    std::lock_guard lock(mu_);
    dir_ = std::move(opts.logDir);
    maxBytes_ = opts.maxFileBytes;
    level_.store(opts.level);
    file_.close();
    bytes_ = 0;
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        filePath_ = dir_ / L"current.log";
        openFile();
    }
}

void Logger::openFile() {
    file_.open(filePath_, std::ios::out | std::ios::trunc | std::ios::binary);
    if (file_) {
        bytes_ = 0;
    } else {
        file_.clear();
    }
}

void Logger::rotateLocked() {
    file_.close();
    const auto previous = dir_ / L"previous.log";
    std::error_code ec;
    std::filesystem::remove(previous, ec);
    std::filesystem::rename(filePath_, previous, ec);
    openFile();
}

void Logger::log(Level l, std::wstring msg) {
    if (!enabled(l)) return;
    const std::wstring line = std::format(L"[{}] [{}] {}\n", timestamp(), levelName(l), msg);

    std::lock_guard lock(mu_);
    if (file_) {
        const std::string utf8 = util::wideToUtf8(line);
        const uint64_t approxBytes = utf8.size();
        if (maxBytes_ > 0 && bytes_ + approxBytes > maxBytes_) rotateLocked();
        if (file_) {
            file_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            bytes_ += approxBytes;
        }
    }
}

void Logger::flush() {
    std::lock_guard lock(mu_);
    if (file_) file_.flush();
}

std::wstring Logger::timestamp() const {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return std::format(L"{:02}:{:02}:{:02}.{:03}",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

} // namespace vw::log
