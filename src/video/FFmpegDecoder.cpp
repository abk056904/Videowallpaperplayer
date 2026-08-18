#include "video/FFmpegDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <format>
#include <cstring>

#include "logging/Logger.h"

// Suppress FFmpeg header warnings (treated as errors by /WX).
#pragma warning(push)
#pragma warning(disable: 4244 4267 4100 4127 4996)

namespace vw::video {

namespace {

std::wstring utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len - 1, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, result.data(), len);
    return result;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), result.data(), len, nullptr, nullptr);
    return result;
}

enum AVPixelFormat hwGetFormat(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts) {
    for (const auto* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_CUDA) return *p;
    }
    return pixFmts[0];
}

} // namespace

FFmpegDecoder::~FFmpegDecoder() { close(); }

Result<void> FFmpegDecoder::open(const std::wstring& path) {
    close();
    if (tryOpenHw(path)) return {};
    auto& log = log::Logger::instance();
    log.info(L"FFmpeg: HW decode unavailable, trying software");
    if (tryOpenSw(path)) return {};
    return std::unexpected(std::wstring(L"FFmpeg: failed to open file"));
}

bool FFmpegDecoder::tryOpenHw(const std::wstring& path) {
    auto& log = log::Logger::instance();
    std::string pathUtf8 = wideToUtf8(path);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) { avformat_close_input(&fmtCtx); return false; }

    int vidIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidIdx < 0) { avformat_close_input(&fmtCtx); return false; }

    AVCodecParameters* par = fmtCtx->streams[vidIdx]->codecpar;

    // Try h264_cuvid, hevc_cuvid, av1_cuvid.
    const char* hwDecoders[] = {"h264_cuvid", "hevc_cuvid", "av1_cuvid"};
    const AVCodec* codec = nullptr;
    for (auto* name : hwDecoders) {
        codec = avcodec_find_decoder_by_name(name);
        if (codec && codec->id == par->codec_id) break;
        codec = nullptr;
    }
    if (!codec) { avformat_close_input(&fmtCtx); return false; }

    AVBufferRef* hwDeviceCtx = nullptr;
    if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
        avformat_close_input(&fmtCtx); return false;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) { av_buffer_unref(&hwDeviceCtx); avformat_close_input(&fmtCtx); return false; }

    avcodec_parameters_to_context(codecCtx, par);
    codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    codecCtx->get_format = hwGetFormat;
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx->thread_count = 0;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        log.debug(L"FFmpeg: HW decoder open failed");
        avcodec_free_context(&codecCtx); av_buffer_unref(&hwDeviceCtx); avformat_close_input(&fmtCtx);
        return false;
    }

    hwCtx_ = hwDeviceCtx;
    fmtCtx_ = fmtCtx;
    codecCtx_ = codecCtx;
    videoStreamIdx_ = vidIdx;
    hardware_ = true;
    decoderName_ = utf8ToWide(codec->name);

    auto* stream = fmtCtx_->streams[videoStreamIdx_];
    metadata_.width = static_cast<UINT>(par->width);
    metadata_.height = static_cast<UINT>(par->height);
    metadata_.fps = av_q2d(stream->avg_frame_rate);
    if (metadata_.fps <= 0 || metadata_.fps > 240) metadata_.fps = 30.0;
    metadata_.duration100ns = (fmtCtx_->duration > 0) ? fmtCtx_->duration * 10 : 0;
    metadata_.codec = utf8ToWide(avcodec_get_name(par->codec_id));
    metadata_.bitDepth = 8;
    metadata_.displayAspect = static_cast<double>(par->width) / par->height;

    frame_ = av_frame_alloc();
    opened_ = true;
    log.info(L"FFmpeg: HW decode opened ({}) {}x{} @ {:.1f} fps",
             decoderName_, metadata_.width, metadata_.height, metadata_.fps);
    return true;
}

bool FFmpegDecoder::tryOpenSw(const std::wstring& path) {
    auto& log = log::Logger::instance();
    std::string pathUtf8 = wideToUtf8(path);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) { avformat_close_input(&fmtCtx); return false; }

    int vidIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidIdx < 0) { avformat_close_input(&fmtCtx); return false; }

    AVCodecParameters* par = fmtCtx->streams[vidIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { avformat_close_input(&fmtCtx); return false; }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) { avformat_close_input(&fmtCtx); return false; }

    avcodec_parameters_to_context(codecCtx, par);
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx->thread_count = 0;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx); avformat_close_input(&fmtCtx); return false;
    }

    fmtCtx_ = fmtCtx;
    codecCtx_ = codecCtx;
    videoStreamIdx_ = vidIdx;
    hardware_ = false;
    decoderName_ = utf8ToWide(codec->name);

    auto* stream = fmtCtx_->streams[videoStreamIdx_];
    metadata_.width = static_cast<UINT>(par->width);
    metadata_.height = static_cast<UINT>(par->height);
    metadata_.fps = av_q2d(stream->avg_frame_rate);
    if (metadata_.fps <= 0 || metadata_.fps > 240) metadata_.fps = 30.0;
    metadata_.duration100ns = (fmtCtx_->duration > 0) ? fmtCtx_->duration * 10 : 0;
    metadata_.codec = utf8ToWide(avcodec_get_name(par->codec_id));
    metadata_.bitDepth = 8;
    metadata_.displayAspect = static_cast<double>(par->width) / par->height;

    frame_ = av_frame_alloc();
    opened_ = true;
    log.info(L"FFmpeg: SW decode opened ({}) {}x{} @ {:.1f} fps",
             decoderName_, metadata_.width, metadata_.height, metadata_.fps);
    return true;
}

void FFmpegDecoder::close() {
    stop();
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (rgbFrame_) { av_frame_free(&rgbFrame_); rgbFrame_ = nullptr; }
    if (codecCtx_) { avcodec_free_context(&codecCtx_); codecCtx_ = nullptr; }
    if (fmtCtx_) { avformat_close_input(&fmtCtx_); fmtCtx_ = nullptr; }
    if (hwCtx_) { av_buffer_unref(&hwCtx_); hwCtx_ = nullptr; }
    videoStreamIdx_ = -1;
    hardware_ = false;
    decoderName_.clear();
    opened_ = false;
}

Result<void> FFmpegDecoder::start(FrameQueue* queue, LONGLONG position100ns) {
    if (!opened_) return std::unexpected(std::wstring(L"FFmpeg: not opened"));
    if (worker_.joinable()) return {};

    if (position100ns > 0 && fmtCtx_) {
        int64_t ts = position100ns / 10; // 100ns units → microseconds → AV_TIME_BASE is microseconds
        avformat_seek_file(fmtCtx_, -1, INT64_MIN, ts, INT64_MAX, 0);
        avcodec_flush_buffers(codecCtx_);
    }

    queue_ = queue;
    stopRequested_ = false;
    decodedFrames_ = 0;
    worker_ = std::thread(&FFmpegDecoder::workerLoop, this, queue);
    return {};
}

void FFmpegDecoder::stop() {
    stopRequested_ = true;
    if (queue_) queue_->close();
    if (worker_.joinable()) worker_.join();
    queue_ = nullptr;
}

void FFmpegDecoder::workerLoop(FrameQueue* queue) {
    auto& log = log::Logger::instance();
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) { log.error(L"FFmpeg: failed to allocate packet"); return; }

    while (!stopRequested_) {
        int ret = av_read_frame(fmtCtx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                DecodedFrame eos;
                eos.endOfStream = true;
                queue->push(std::move(eos));
                log.debug(L"FFmpeg: EOS after {} frames", decodedFrames_.load());
            }
            break;
        }

        if (pkt->stream_index != videoStreamIdx_) { av_packet_unref(pkt); continue; }

        ret = avcodec_send_packet(codecCtx_, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (ret >= 0) {
            ret = avcodec_receive_frame(codecCtx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            DecodedFrame df;
            df.width = static_cast<UINT>(frame_->width);
            df.height = static_cast<UINT>(frame_->height);

            if (hardware_ && frame_->format == AV_PIX_FMT_CUDA) {
                // HW → CPU transfer as NV12.
                AVFrame* swFrame = av_frame_alloc();
                if (av_hwframe_transfer_data(swFrame, frame_, 0) == 0) {
                    df.nv12 = true;
                    df.hardware = true;
                    uint32_t w = swFrame->width;
                    uint32_t h = swFrame->height;
                    uint32_t ySize = w * h;
                    uint32_t uvSize = w * (h / 2);
                    df.bytes.resize(ySize + uvSize);
                    for (uint32_t y = 0; y < h; ++y)
                        std::memcpy(df.bytes.data() + y * w, swFrame->data[0] + y * swFrame->linesize[0], w);
                    for (uint32_t y = 0; y < h / 2; ++y)
                        std::memcpy(df.bytes.data() + ySize + y * w, swFrame->data[1] + y * swFrame->linesize[1], w);
                    av_frame_free(&swFrame);
                } else { av_frame_free(&swFrame); continue; }
            } else if (frame_->format == AV_PIX_FMT_YUV420P || frame_->format == AV_PIX_FMT_NV12) {
                df.nv12 = true;
                uint32_t w = frame_->width;
                uint32_t h = frame_->height;
                uint32_t ySize = w * h;
                uint32_t uvSize = w * (h / 2);
                df.bytes.resize(ySize + uvSize);
                for (uint32_t y = 0; y < h; ++y)
                    std::memcpy(df.bytes.data() + y * w, frame_->data[0] + y * frame_->linesize[0], w);
                uint8_t* uvDst = df.bytes.data() + ySize;
                if (frame_->format == AV_PIX_FMT_NV12) {
                    for (uint32_t y = 0; y < h / 2; ++y)
                        std::memcpy(uvDst + y * w, frame_->data[1] + y * frame_->linesize[1], w);
                } else {
                    for (uint32_t y = 0; y < h / 2; ++y)
                        for (uint32_t x = 0; x < w / 2; ++x) {
                            uvDst[y * w + x * 2] = frame_->data[1][y * frame_->linesize[1] + x];
                            uvDst[y * w + x * 2 + 1] = frame_->data[2][y * frame_->linesize[2] + x];
                        }
                }
            } else {
                // Convert to BGRA as last resort.
                uint32_t w = frame_->width;
                uint32_t h = frame_->height;
                uint32_t stride = w * 4;
                df.bytes.resize(stride * h);
                SwsContext* sws = sws_getContext(w, h, static_cast<AVPixelFormat>(frame_->format),
                                                  w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (sws) {
                    uint8_t* dst[1] = {df.bytes.data()};
                    int dstStride[1] = {static_cast<int>(stride)};
                    sws_scale(sws, frame_->data, frame_->linesize, 0, h, dst, dstStride);
                    sws_freeContext(sws);
                }
            }

            if (!queue->push(std::move(df))) break;
            decodedFrames_++;
        }
    }

    av_packet_free(&pkt);
    log.info(L"FFmpeg decode worker finished ({} frames)", decodedFrames_.load());
}

Result<VideoMetadata> FFmpegDecoder::probeMetadata(const std::wstring& path) {
    std::string pathUtf8 = wideToUtf8(path);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0)
        return std::unexpected(std::wstring(L"cannot open file"));
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"cannot find stream info"));
    }

    int vidIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidIdx < 0) {
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"no video stream"));
    }

    AVCodecParameters* par = fmtCtx->streams[vidIdx]->codecpar;
    auto* stream = fmtCtx->streams[vidIdx];

    VideoMetadata meta;
    meta.width = static_cast<UINT>(par->width);
    meta.height = static_cast<UINT>(par->height);
    meta.fps = av_q2d(stream->avg_frame_rate);
    if (meta.fps <= 0 || meta.fps > 240) meta.fps = 30.0;
    meta.duration100ns = (fmtCtx->duration > 0) ? fmtCtx->duration * 10 : 0;
    meta.codec = utf8ToWide(avcodec_get_name(par->codec_id));
    meta.bitDepth = 8;
    meta.displayAspect = (par->height > 0) ? static_cast<double>(par->width) / par->height : 0.0;

    avformat_close_input(&fmtCtx);
    return meta;
}

} // namespace vw::video

#pragma warning(pop)
