#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <Windows.h>
#include <string>
#include "util/FFmpegApi.h"

struct FfmpegInit {
    FfmpegInit() {
        wchar_t exePath[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir = exePath;
        auto pos = dir.find_last_of(L'\\');
        if (pos != std::wstring::npos) dir.resize(pos);
        vw::ffmpeg::ffmpegApiLoad(dir.c_str());
    }
    ~FfmpegInit() { vw::ffmpeg::ffmpegApiUnload(); }
};

static FfmpegInit g_ffmpegInit;
