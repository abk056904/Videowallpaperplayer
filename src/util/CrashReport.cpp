#include "util/CrashReport.h"

#include <chrono>
#include <filesystem>

#include <Windows.h>
#include <minidumpapiset.h>

#pragma comment(lib, "dbghelp.lib")

namespace vw::util {

namespace {

std::filesystem::path s_crashesDir;
std::wstring s_lastDumpPath;

std::wstring formatTimestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32]{};
    ::swprintf(buf, 32, L"%04u-%02u-%02u_%02u-%02u-%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void cleanOldDumps(const std::filesystem::path& dir, int maxAgeDays) {
    if (!std::filesystem::exists(dir)) return;
    const auto cutoff = std::chrono::file_clock::now() - std::chrono::hours(maxAgeDays * 24);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == L".dmp") {
            if (entry.last_write_time(ec) < cutoff) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exInfo) {
    if (s_crashesDir.empty()) return EXCEPTION_CONTINUE_SEARCH;

    std::filesystem::create_directories(s_crashesDir);

    const auto filename = L"crash_" + formatTimestamp() + L".dmp";
    const auto dumpPath = s_crashesDir / filename;
    s_lastDumpPath = dumpPath.wstring();

    HANDLE hFile = ::CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exInfoParam{};
        exInfoParam.ThreadId = ::GetCurrentThreadId();
        exInfoParam.ExceptionPointers = exInfo;
        exInfoParam.ClientPointers = FALSE;

        ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(),
                            hFile, MiniDumpNormal, &exInfoParam, nullptr, nullptr);
        ::CloseHandle(hFile);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

} // anonymous namespace

void CrashReport::install(const std::filesystem::path& crashesDir, int maxAgeDays) {
    s_crashesDir = crashesDir;
    s_lastDumpPath.clear();
    cleanOldDumps(crashesDir, maxAgeDays);
    ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
}

} // namespace vw::util
