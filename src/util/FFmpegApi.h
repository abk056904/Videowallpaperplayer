#pragma once

// Lazy-load FFmpeg DLLs: instead of statically linking against import
// libraries (which forces the OS loader to map all 5 DLLs at process
// start), we LoadLibrary them on demand and resolve function pointers.
// This saves ~10-30 MB of peak working set when MF HW decode is used
// instead of FFmpeg, and allows clean FreeLibrary on shutdown.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// Suppress FFmpeg header warnings (treated as errors by /WX).
#pragma warning(push)
#pragma warning(disable: 4244 4267 4100 4127 4996)

// Required by FFmpeg headers when compiled as C++.
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace vw::ffmpeg {

// Function pointer types — one per FFmpeg API function we call.
// Types are derived from the real FFmpeg headers above.
struct FFmpegApi {
    // ---- libavutil ----
    AVBufferRef*    (*buffer_ref)(AVBufferRef *buf) = nullptr;
    void            (*buffer_unref)(AVBufferRef **buf) = nullptr;
    AVFrame*        (*frame_alloc)(void) = nullptr;
    void            (*frame_free)(AVFrame **frame) = nullptr;
    void            (*frame_unref)(AVFrame *frame) = nullptr;
    int             (*hwdevice_ctx_create)(AVBufferRef **device_ctx, enum AVHWDeviceType type,
                                           const char *device, const char *opts, int flags) = nullptr;
    int             (*hwdevice_ctx_create_derived)(AVBufferRef **device_ctx,
                                                   enum AVHWDeviceType type,
                                                   AVBufferRef *src, int flags) = nullptr;
    AVBufferRef*    (*hwframe_ctx_alloc)(AVBufferRef *device_ctx) = nullptr;
    int             (*hwframe_ctx_init)(AVBufferRef *hwframe_ctx) = nullptr;
    int             (*hwframe_map)(AVFrame *dst, AVFrame *src, int flags) = nullptr;
    int             (*hwframe_transfer_data)(AVFrame *dst, AVFrame *src, int flags) = nullptr;
    AVPacket*       (*packet_alloc)(void) = nullptr;
    void            (*packet_free)(AVPacket **pkt) = nullptr;
    void            (*packet_unref)(AVPacket *pkt) = nullptr;
    int             (*find_best_stream)(AVFormatContext *s, enum AVMediaType type,
                                        int wanted_stream_nb, int related_stream,
                                        const AVCodec **decoder_ret, int flags) = nullptr;
    int64_t         (*rescale_q)(int64_t a, AVRational bq, AVRational cq) = nullptr;
    int64_t         (*rescale_rnd)(int64_t a, int64_t b, int64_t c, enum AVRounding rnd) = nullptr;
    int             (*strerror)(int errnum, char *errbuf, size_t errbuf_size) = nullptr;
    int             (*get_bytes_per_sample)(enum AVSampleFormat sample_fmt) = nullptr;
    int             (*image_get_buffer_size)(enum AVPixelFormat pix_fmt, int width,
                                             int height, int align) = nullptr;
    int             (*image_fill_arrays)(uint8_t *dst_data[4], int dst_linesize[4],
                                         const uint8_t *src, enum AVPixelFormat pix_fmt,
                                         int width, int height, int align) = nullptr;

    // ---- libavcodec ----
    AVCodecContext* (*codec_alloc_context3)(const AVCodec *codec) = nullptr;
    const AVCodec*  (*codec_find_decoder)(enum AVCodecID id) = nullptr;
    const AVCodec*  (*codec_find_decoder_by_name)(const char *name) = nullptr;
    void            (*codec_flush_buffers)(AVCodecContext *avctx) = nullptr;
    void            (*codec_free_context)(AVCodecContext **avctx) = nullptr;
    const char*     (*codec_get_name)(enum AVCodecID id) = nullptr;
    int             (*codec_open2)(AVCodecContext *avctx, const AVCodec *codec,
                                   const AVDictionary **options) = nullptr;
    int             (*codec_parameters_to_context)(AVCodecContext *codec,
                                                    const AVCodecParameters *par) = nullptr;
    int             (*codec_receive_frame)(AVCodecContext *avctx, AVFrame *frame) = nullptr;
    int             (*codec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt) = nullptr;

    // ---- libavformat ----
    void            (*format_close_input)(AVFormatContext **s) = nullptr;
    int             (*format_find_stream_info)(AVFormatContext *ic,
                                               const AVDictionary **options) = nullptr;
    int             (*format_open_input)(AVFormatContext **ps, const char *url,
                                         const AVInputFormat *fmt,
                                         AVDictionary **options) = nullptr;
    int             (*format_seek_file)(AVFormatContext *s, int stream_index,
                                        int64_t min_ts, int64_t ts,
                                        int64_t max_ts, int flags) = nullptr;
    int             (*read_frame)(AVFormatContext *s, AVPacket *pkt) = nullptr;

    // ---- libswresample ----
    int             (*swr_alloc_set_opts2)(SwrContext **ps,
                                           const AVChannelLayout *out_ch_layout,
                                           enum AVSampleFormat out_sample_fmt,
                                           int out_sample_rate,
                                           const AVChannelLayout *in_ch_layout,
                                           enum AVSampleFormat in_sample_fmt,
                                           int in_sample_rate,
                                           int log_offset, void *log_ctx) = nullptr;
    int             (*swr_init)(SwrContext *s) = nullptr;
    void            (*swr_free)(SwrContext **s) = nullptr;
    int             (*swr_convert)(SwrContext *s, uint8_t **out, int out_count,
                                   const uint8_t **in, int in_count) = nullptr;
    int64_t         (*swr_get_delay)(SwrContext *s, int64_t base) = nullptr;

    // ---- libswscale ----
    struct SwsContext* (*sws_getContext)(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                        int dstW, int dstH, enum AVPixelFormat dstFormat,
                                        int flags, void *srcFilter,
                                        void *dstFilter, const double *param) = nullptr;
    int             (*sws_scale)(struct SwsContext *c,
                                 const uint8_t *const srcSlice[],
                                 const int srcStride[], int srcSliceY,
                                 int srcSliceH, uint8_t *const dst[],
                                 const int dstStride[]) = nullptr;
    void            (*sws_freeContext)(struct SwsContext *swsContext) = nullptr;
};

// Load all FFmpeg DLLs and resolve function pointers.
// Returns true on success. DLLs are loaded from `dllDir` (absolute path).
bool ffmpegApiLoad(const wchar_t* dllDir);

// Free all FFmpeg DLLs. Call after all FFmpeg resources are released.
void ffmpegApiUnload();

// Access the loaded API. Undefined behavior if not loaded.
const FFmpegApi& ffmpegApi();

} // namespace vw::ffmpeg

#pragma warning(pop)
