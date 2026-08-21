#include "util/FFmpegApi.h"

// Suppress FFmpeg header warnings (treated as errors by /WX).
#pragma warning(push)
#pragma warning(disable: 4244 4267 4100 4127 4996)

#include <cstdio>
#include <string>

#include "logging/Logger.h"

namespace vw::ffmpeg {

namespace {

FFmpegApi g_api{};
HMODULE g_hAvutil = nullptr;
HMODULE g_hAvcodec = nullptr;
HMODULE g_hAvformat = nullptr;
HMODULE g_hSwresample = nullptr;
HMODULE g_hSwscale = nullptr;

template <typename Fn>
bool loadFn(HMODULE h, Fn& fn, const char* name) {
    fn = reinterpret_cast<Fn>(GetProcAddress(h, name));
    if (!fn) {
        fprintf(stderr, "[FFmpegApi] FAILED to resolve: %s\n", name);
    }
    return fn != nullptr;
}

// Helper: narrow-to-wide for log messages
std::wstring toWide(const char* s) {
    if (!s) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws.data(), len);
    return ws;
}

bool loadDll(HMODULE& h, const wchar_t* dllDir, const char* dllName) {
    std::wstring path = std::wstring(dllDir) + L"\\" + toWide(dllName);
    h = LoadLibraryW(path.c_str());
    if (!h) {
        fprintf(stderr, "[FFmpegApi] FAILED to load: %s (error %lu)\n", dllName, GetLastError());
        return false;
    }
    return true;
}

} // namespace

bool ffmpegApiLoad(const wchar_t* dllDir) {
    auto& log = log::Logger::instance();

    // Load DLLs in dependency order: avutil first, then the rest.
    if (!loadDll(g_hAvutil, dllDir, "avutil-59.dll")) return false;
    if (!loadDll(g_hAvcodec, dllDir, "avcodec-61.dll")) { ffmpegApiUnload(); return false; }
    if (!loadDll(g_hAvformat, dllDir, "avformat-61.dll")) { ffmpegApiUnload(); return false; }
    if (!loadDll(g_hSwresample, dllDir, "swresample-5.dll")) { ffmpegApiUnload(); return false; }
    if (!loadDll(g_hSwscale, dllDir, "swscale-8.dll")) { ffmpegApiUnload(); return false; }

    bool ok = true;

    // libavutil — buffer, frame, hwcontext, math, error, samplefmt
    ok &= loadFn(g_hAvutil, g_api.buffer_ref, "av_buffer_ref");
    ok &= loadFn(g_hAvutil, g_api.buffer_unref, "av_buffer_unref");
    ok &= loadFn(g_hAvutil, g_api.frame_alloc, "av_frame_alloc");
    ok &= loadFn(g_hAvutil, g_api.frame_free, "av_frame_free");
    ok &= loadFn(g_hAvutil, g_api.frame_unref, "av_frame_unref");
    ok &= loadFn(g_hAvutil, g_api.hwdevice_ctx_create, "av_hwdevice_ctx_create");
    ok &= loadFn(g_hAvutil, g_api.hwdevice_ctx_create_derived, "av_hwdevice_ctx_create_derived");
    ok &= loadFn(g_hAvutil, g_api.hwframe_ctx_alloc, "av_hwframe_ctx_alloc");
    ok &= loadFn(g_hAvutil, g_api.hwframe_ctx_init, "av_hwframe_ctx_init");
    ok &= loadFn(g_hAvutil, g_api.hwframe_map, "av_hwframe_map");
    ok &= loadFn(g_hAvutil, g_api.hwframe_transfer_data, "av_hwframe_transfer_data");
    ok &= loadFn(g_hAvutil, g_api.rescale_q, "av_rescale_q");
    ok &= loadFn(g_hAvutil, g_api.rescale_rnd, "av_rescale_rnd");
    ok &= loadFn(g_hAvutil, g_api.strerror, "av_strerror");
    ok &= loadFn(g_hAvutil, g_api.get_bytes_per_sample, "av_get_bytes_per_sample");
    ok &= loadFn(g_hAvutil, g_api.image_get_buffer_size, "av_image_get_buffer_size");
    ok &= loadFn(g_hAvutil, g_api.image_fill_arrays, "av_image_fill_arrays");

    // libavcodec — packet, codec, hwaccel
    ok &= loadFn(g_hAvcodec, g_api.packet_alloc, "av_packet_alloc");
    ok &= loadFn(g_hAvcodec, g_api.packet_free, "av_packet_free");
    ok &= loadFn(g_hAvcodec, g_api.packet_unref, "av_packet_unref");
    ok &= loadFn(g_hAvcodec, g_api.codec_alloc_context3, "avcodec_alloc_context3");
    ok &= loadFn(g_hAvcodec, g_api.codec_find_decoder, "avcodec_find_decoder");
    ok &= loadFn(g_hAvcodec, g_api.codec_find_decoder_by_name, "avcodec_find_decoder_by_name");
    ok &= loadFn(g_hAvcodec, g_api.codec_flush_buffers, "avcodec_flush_buffers");
    ok &= loadFn(g_hAvcodec, g_api.codec_free_context, "avcodec_free_context");
    ok &= loadFn(g_hAvcodec, g_api.codec_get_name, "avcodec_get_name");
    ok &= loadFn(g_hAvcodec, g_api.codec_open2, "avcodec_open2");
    ok &= loadFn(g_hAvcodec, g_api.codec_parameters_to_context, "avcodec_parameters_to_context");
    ok &= loadFn(g_hAvcodec, g_api.codec_receive_frame, "avcodec_receive_frame");
    ok &= loadFn(g_hAvcodec, g_api.codec_send_packet, "avcodec_send_packet");

    // libavformat — container I/O
    ok &= loadFn(g_hAvformat, g_api.find_best_stream, "av_find_best_stream");
    ok &= loadFn(g_hAvformat, g_api.format_close_input, "avformat_close_input");
    ok &= loadFn(g_hAvformat, g_api.format_find_stream_info, "avformat_find_stream_info");
    ok &= loadFn(g_hAvformat, g_api.format_open_input, "avformat_open_input");
    ok &= loadFn(g_hAvformat, g_api.format_seek_file, "avformat_seek_file");
    ok &= loadFn(g_hAvformat, g_api.read_frame, "av_read_frame");

    // libswresample
    ok &= loadFn(g_hSwresample, g_api.swr_alloc_set_opts2, "swr_alloc_set_opts2");
    ok &= loadFn(g_hSwresample, g_api.swr_init, "swr_init");
    ok &= loadFn(g_hSwresample, g_api.swr_free, "swr_free");
    ok &= loadFn(g_hSwresample, g_api.swr_convert, "swr_convert");
    ok &= loadFn(g_hSwresample, g_api.swr_get_delay, "swr_get_delay");

    // libswscale
    ok &= loadFn(g_hSwscale, g_api.sws_getContext, "sws_getContext");
    ok &= loadFn(g_hSwscale, g_api.sws_scale, "sws_scale");
    ok &= loadFn(g_hSwscale, g_api.sws_freeContext, "sws_freeContext");

    if (ok) {
        log.info(L"FFmpegApi: all 5 DLLs loaded and 42 functions resolved");
    } else {
        log.warn(L"FFmpegApi: some function resolutions failed");
    }
    return ok;
}

void ffmpegApiUnload() {
    if (g_hSwscale)   { FreeLibrary(g_hSwscale);   g_hSwscale = nullptr; }
    if (g_hSwresample) { FreeLibrary(g_hSwresample); g_hSwresample = nullptr; }
    if (g_hAvformat)  { FreeLibrary(g_hAvformat);   g_hAvformat = nullptr; }
    if (g_hAvcodec)   { FreeLibrary(g_hAvcodec);    g_hAvcodec = nullptr; }
    if (g_hAvutil)    { FreeLibrary(g_hAvutil);     g_hAvutil = nullptr; }
    g_api = {};
}

const FFmpegApi& ffmpegApi() {
    return g_api;
}

} // namespace vw::ffmpeg
