#include "video/FFmpegDecoder.h"

// D3D11 headers must come before FFmpeg to avoid forward-declaration conflicts.
#include <d3d11_1.h>
#include <dxgi1_2.h>

// Lazy-loaded FFmpeg: types from real headers, calls through function pointers.
#include "util/FFmpegCompat.h"
#include <libavcodec/d3d11va.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>

#include <cstring>

#include "logging/Logger.h"
#include "util/clock.h"
#include "util/utf8.h"

// Suppress FFmpeg header warnings (treated as errors by /WX).
#pragma warning(push)
#pragma warning(disable: 4244 4267 4100 4127 4996)

namespace vw::video {

namespace {

// Decode thread priority (THREAD_PRIORITY_HIGHER = 1). Named constant
// avoids scattering magic numbers across FFmpegDecoder + DecoderManager.
constexpr int kDecodeThreadPriority = 1;

// D3D11VA pixel format callback
enum AVPixelFormat hwGetFormatD3d11(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts) {
    for (const auto* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    return pixFmts[0];
}

// CUDA pixel format callback
enum AVPixelFormat hwGetFormatCuda(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts) {
    for (const auto* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_CUDA) return *p;
    }
    return pixFmts[0];
}

// DRY: fill VideoMetadata from an opened AVFormatContext + AVCodecParameters.
// Called by tryOpenD3d11va, tryOpenCuda, tryOpenSw, and probeMetadata.
void fillMetadata(VideoMetadata& meta, const AVFormatContext* fmtCtx, int videoStreamIdx,
                  const AVCodecParameters* par) {
    auto* stream = fmtCtx->streams[videoStreamIdx];
    meta.width = static_cast<UINT>(par->width);
    meta.height = static_cast<UINT>(par->height);
    meta.fps = av_q2d(stream->avg_frame_rate);
    if (meta.fps <= 0 || meta.fps > 240) meta.fps = 30.0;
    meta.duration100ns = (fmtCtx->duration > 0) ? fmtCtx->duration * 10 : 0;
    meta.codec = vw::util::utf8ToWide(avcodec_get_name(par->codec_id));
    meta.bitDepth = 8;

    // Use the stream's sample aspect ratio (SAR) when available.
    // Anamorphic content (e.g. 720x480 DVD) has non-square pixels and the
    // display aspect differs from width/height — the scaling math needs the
    // SAR-corrected value or the video will distort (ScaleMath.h docs).
    // Use stream->sample_aspect_ratio directly (av_guess_sample_aspect_ratio
    // is not available in our minimal FFmpeg build).
    if (stream->sample_aspect_ratio.num > 0 && stream->sample_aspect_ratio.den > 0) {
        meta.sarNum = static_cast<UINT>(stream->sample_aspect_ratio.num);
        meta.sarDen = static_cast<UINT>(stream->sample_aspect_ratio.den);
    } else if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
        meta.sarNum = static_cast<UINT>(par->sample_aspect_ratio.num);
        meta.sarDen = static_cast<UINT>(par->sample_aspect_ratio.den);
    }
    if (par->height > 0) {
        meta.displayAspect =
            (static_cast<double>(par->width) * meta.sarNum) /
            (static_cast<double>(par->height) * meta.sarDen);
    }

    // Detect bit depth from codec parameters or known pixel formats.
    // bits_per_raw_sample is the most reliable source (set by decoders).
    if (par->bits_per_raw_sample > 0) {
        meta.bitDepth = static_cast<UINT>(par->bits_per_raw_sample);
    } else {
        // Fallback: map known pixel formats to bit depth.
        switch (par->format) {
            case AV_PIX_FMT_P010LE:
            case AV_PIX_FMT_P010BE: meta.bitDepth = 10; break;
            default: meta.bitDepth = 8; break;
        }
    }
}

// DRY: copy NV12 rows from src (with linesize stride) to a tightly-packed dst.
// Handles both Y plane and interleaved UV plane. Used by D3D11VA fallback and
// software NV12 paths in workerLoop.
void copyNv12Tightly(uint8_t* dst, const uint8_t* const* srcData,
                     const int* linesize, uint32_t w, uint32_t h) {
    const uint32_t ySize = w * h;
    const uint32_t uvSize = w * (h / 2);
    const bool yBulk = (linesize[0] == static_cast<int>(w));
    const bool uvBulk = (linesize[1] == static_cast<int>(w));
    if (yBulk) {
        std::memcpy(dst, srcData[0], ySize);
    } else {
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(dst + y * w, srcData[0] + y * linesize[0], w);
    }
    uint8_t* uvDst = dst + ySize;
    if (uvBulk) {
        std::memcpy(uvDst, srcData[1], uvSize);
    } else {
        for (uint32_t y = 0; y < h / 2; ++y)
            std::memcpy(uvDst + y * w, srcData[1] + y * linesize[1], w);
    }
}

// DRY: copy YUV420P planes (separate Y, U, V) to tightly-packed NV12 layout.
void copyYuv420pToNv12(uint8_t* dst, const uint8_t* const* data,
                        const int* linesize, uint32_t w, uint32_t h) {
    const uint32_t ySize = w * h;
    // Y plane
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(dst + y * w, data[0] + y * linesize[0], w);
    // Interleave U/V into NV12 UV plane
    uint8_t* uvDst = dst + ySize;
    for (uint32_t y = 0; y < h / 2; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) {
            uvDst[y * w + x * 2] = data[1][y * linesize[1] + x];
            uvDst[y * w + x * 2 + 1] = data[2][y * linesize[2] + x];
        }
}

} // namespace

FFmpegDecoder::~FFmpegDecoder() { close(); }

Result<void> FFmpegDecoder::open(const std::wstring& path) {
    close();
    // Try D3D11VA first (zero-copy for H.264/HEVC), then CUDA (VP9/AV1), then software
    if (tryOpenD3d11va(path)) return {};
    if (tryOpenCuda(path)) return {};
    auto& log = log::Logger::instance();
    log.info(L"FFmpeg: HW decode unavailable, trying software");
    if (tryOpenSw(path)) return {};
    return std::unexpected(std::wstring(L"FFmpeg: failed to open file"));
}

bool FFmpegDecoder::tryOpenD3d11va(const std::wstring& path) {
    auto& log = log::Logger::instance();
    std::string pathUtf8 = vw::util::wideToUtf8(path);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) { avformat_close_input(&fmtCtx); return false; }

    int vidIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidIdx < 0) { avformat_close_input(&fmtCtx); return false; }

    AVCodecParameters* par = fmtCtx->streams[vidIdx]->codecpar;

    // Use the regular decoder — D3D11VA activates via hw_device_ctx +
    // get_format callback, NOT via a separate decoder name.
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        log.info(L"FFmpeg: no decoder found for codec id {}", static_cast<int>(par->codec_id));
        avformat_close_input(&fmtCtx);
        return false;
    }
    log.info(L"FFmpeg: using decoder {} for D3D11VA hwaccel", vw::util::utf8ToWide(codec->name));

    // D3D11VA: FFmpeg uses its OWN D3D11 device (not the render device)
    // because D3D11 immediate contexts are NOT thread-safe. Decoded textures
    // are GPU-resident; CopySubresourceRegion copies them to a shared texture
    // that the render device opens — this is a GPU-to-GPU copy, not CPU,
    // so there are still zero CPU↔GPU copies (meets §1.2.9).
    AVBufferRef* hwDeviceCtx = nullptr;
    {
        int err = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (err < 0) {
            char errbuf[128]{};
            av_strerror(err, errbuf, sizeof(errbuf));
            log.warn(L"FFmpeg: D3D11VA device create failed: {}",
                     vw::util::utf8ToWide(errbuf));
            avformat_close_input(&fmtCtx); return false;
        }
        log.info(L"FFmpeg: D3D11VA device created (GPU decode, CPU texture readback)");
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) { av_buffer_unref(&hwDeviceCtx); avformat_close_input(&fmtCtx); return false; }

    avcodec_parameters_to_context(codecCtx, par);
    codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    codecCtx->get_format = hwGetFormatD3d11;
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx->thread_count = 0;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        log.warn(L"FFmpeg: D3D11VA decoder open failed");
        avcodec_free_context(&codecCtx); av_buffer_unref(&hwDeviceCtx); avformat_close_input(&fmtCtx);
        return false;
    }

    hwCtx_ = hwDeviceCtx;
    fmtCtx_ = fmtCtx;
    codecCtx_ = codecCtx;
    videoStreamIdx_ = vidIdx;
    hardware_ = true;
    decoderName_ = vw::util::utf8ToWide(codec->name);

    fillMetadata(metadata_, fmtCtx_, videoStreamIdx_, par);

    frame_ = av_frame_alloc();
    opened_ = true;
    log.info(L"FFmpeg: D3D11VA decode opened ({}) {}x{} @ {:.1f} fps",
             decoderName_, metadata_.width, metadata_.height, metadata_.fps);
    return true;
}

bool FFmpegDecoder::tryOpenCuda(const std::wstring& path) {
    auto& log = log::Logger::instance();
    std::string pathUtf8 = vw::util::wideToUtf8(path);

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) { avformat_close_input(&fmtCtx); return false; }

    int vidIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidIdx < 0) { avformat_close_input(&fmtCtx); return false; }

    AVCodecParameters* par = fmtCtx->streams[vidIdx]->codecpar;

    // Try CUDA-based decoders (cuvid)
    const char* cudaDecoders[] = {"h264_cuvid", "hevc_cuvid", "vp9_cuvid", "av1_cuvid"};
    const AVCodec* codec = nullptr;
    for (auto* name : cudaDecoders) {
        codec = avcodec_find_decoder_by_name(name);
        if (codec && codec->id == par->codec_id) break;
        codec = nullptr;
    }
    if (!codec) { avformat_close_input(&fmtCtx); return false; }

    // Create CUDA device context
    AVBufferRef* hwDeviceCtx = nullptr;
    if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Derive D3D11VA device context from CUDA, then create a frames context
    // for av_hwframe_map (CUDA frame -> D3D11 texture, zero-copy).
    AVBufferRef* d3d11DeviceCtx = nullptr;
    if (av_hwdevice_ctx_create_derived(&d3d11DeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, hwDeviceCtx, 0) < 0) {
        log.debug(L"FFmpeg: could not derive D3D11VA from CUDA");
        av_buffer_unref(&hwDeviceCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Create frames context so av_hwframe_map can map CUDA -> D3D11.
    AVBufferRef* d3d11FramesCtx = av_hwframe_ctx_alloc(d3d11DeviceCtx);
    if (!d3d11FramesCtx) {
        av_buffer_unref(&d3d11DeviceCtx);
        av_buffer_unref(&hwDeviceCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }
    {
        auto* fc = reinterpret_cast<AVHWFramesContext*>(d3d11FramesCtx->data);
        fc->width = par->width;
        fc->height = par->height;
        fc->format = AV_PIX_FMT_D3D11;
        fc->sw_format = AV_PIX_FMT_NV12;
    }
    if (av_hwframe_ctx_init(d3d11FramesCtx) < 0) {
        log.debug(L"FFmpeg: D3D11VA frames context init failed");
        av_buffer_unref(&d3d11FramesCtx);
        av_buffer_unref(&d3d11DeviceCtx);
        av_buffer_unref(&hwDeviceCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        av_buffer_unref(&d3d11DeviceCtx);
        av_buffer_unref(&hwDeviceCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    avcodec_parameters_to_context(codecCtx, par);
    codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    codecCtx->get_format = hwGetFormatCuda;
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codecCtx->thread_count = 0;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        log.debug(L"FFmpeg: CUDA decoder open failed");
        avcodec_free_context(&codecCtx);
        av_buffer_unref(&d3d11DeviceCtx);
        av_buffer_unref(&hwDeviceCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    hwCtx_ = hwDeviceCtx;
    hwFramesCtx_ = d3d11FramesCtx; // Frames context for av_hwframe_map
    av_buffer_unref(&d3d11DeviceCtx); // Device ctx no longer needed separately
    fmtCtx_ = fmtCtx;
    codecCtx_ = codecCtx;
    videoStreamIdx_ = vidIdx;
    hardware_ = true;
    decoderName_ = vw::util::utf8ToWide(codec->name);

    fillMetadata(metadata_, fmtCtx_, videoStreamIdx_, par);

    frame_ = av_frame_alloc();
    opened_ = true;
    log.info(L"FFmpeg: CUDA decode opened ({}) {}x{} @ {:.1f} fps",
             decoderName_, metadata_.width, metadata_.height, metadata_.fps);
    return true;
}

bool FFmpegDecoder::tryOpenSw(const std::wstring& path) {
    auto& log = log::Logger::instance();
    std::string pathUtf8 = vw::util::wideToUtf8(path);

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
    decoderName_ = vw::util::utf8ToWide(codec->name);

    fillMetadata(metadata_, fmtCtx_, videoStreamIdx_, par);

    frame_ = av_frame_alloc();
    opened_ = true;
    log.info(L"FFmpeg: SW decode opened ({}) {}x{} @ {:.1f} fps",
             decoderName_, metadata_.width, metadata_.height, metadata_.fps);
    return true;
}

void FFmpegDecoder::close() {
    stop();
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (codecCtx_) { avcodec_free_context(&codecCtx_); codecCtx_ = nullptr; }
    if (fmtCtx_) { avformat_close_input(&fmtCtx_); fmtCtx_ = nullptr; }
    if (hwFramesCtx_) { av_buffer_unref(&hwFramesCtx_); hwFramesCtx_ = nullptr; }
    if (hwCtx_) { av_buffer_unref(&hwCtx_); hwCtx_ = nullptr; }
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; swsSrcW_ = swsSrcH_ = 0; swsSrcFmt_ = -1; }
    videoStreamIdx_ = -1;
    hardware_ = false;
    decoderName_.clear();
    opened_ = false;
}

ID3D11Device* FFmpegDecoder::ffmpegDevice() const {
    if (!hwCtx_ || !hardware_) return nullptr;
    auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(hwCtx_->data);
    auto* d3d11ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    return d3d11ctx ? d3d11ctx->device : nullptr;
}

ID3D11DeviceContext* FFmpegDecoder::ffmpegContext() const {
    if (!hwCtx_ || !hardware_) return nullptr;
    auto* hwctx = reinterpret_cast<AVHWDeviceContext*>(hwCtx_->data);
    auto* d3d11ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    return d3d11ctx ? d3d11ctx->device_context : nullptr;
}

Result<void> FFmpegDecoder::start(FrameQueue* queue, LONGLONG position100ns) {
    if (!opened_) return std::unexpected(std::wstring(L"FFmpegDecoder::start: not opened"));
    if (worker_.joinable()) return {};

    if (fmtCtx_) {
        // Always seek to the requested position (including 0 for replay).
        // AVSEEK_FLAG_BACKWARD ensures we land on a keyframe BEFORE the target
        // so the decoder has valid reference frames from the start (fixes
        // pixelation at video start). Without it, seeking to a non-keyframe
        // position produces corrupted frames until the next keyframe arrives.
        int64_t ts = position100ns / 10; // 100ns units -> microseconds
        avformat_seek_file(fmtCtx_, -1, INT64_MIN, ts, INT64_MAX,
                          AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(codecCtx_);
    }

    queue_ = queue;
    stopRequested_ = false;
    decodedFrames_ = 0;
    worker_ = std::thread([this, queue]() {
        // Boost decode thread priority so OS background tasks (AV scans,
        // Windows Update, indexers) cannot preempt the decode loop and
        // cause frame-starvation lag on the consumer side.
        ::SetThreadPriority(::GetCurrentThread(), kDecodeThreadPriority);
        workerLoop(queue);
    });
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
        if (ret < 0) {
            static int sendFailures = 0;
            if (++sendFailures <= 3) {
                char errbuf[128]{};
                av_strerror(ret, errbuf, sizeof(errbuf));
                log.warn(L"FFmpeg: send_packet failed ({})", vw::util::utf8ToWide(errbuf));
            }
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codecCtx_, frame_);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                static int recvFailures = 0;
                if (++recvFailures <= 3) {
                    char errbuf[128]{};
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    log.warn(L"FFmpeg: receive_frame failed ({})", vw::util::utf8ToWide(errbuf));
                }
                break;
            }

            DecodedFrame df;
            df.width = static_cast<UINT>(frame_->width);
            df.height = static_cast<UINT>(frame_->height);
            df.displayAspect = static_cast<float>(metadata_.displayAspect);

            // Set timestamp from PTS
            if (frame_->pts != AV_NOPTS_VALUE && fmtCtx_ && videoStreamIdx_ >= 0) {
                const AVRational tb = fmtCtx_->streams[videoStreamIdx_]->time_base;
                df.timestamp = static_cast<LONGLONG>(av_rescale_q(frame_->pts, tb, {1, 10000000}));
            }

            if (hardware_ && frame_->format == AV_PIX_FMT_D3D11) {
                // D3D11VA: GPU decodes on FFmpeg's own D3D11 device.
                // GPU decode + CPU texture readback: av_hwframe_transfer_data
                // copies the decoded GPU texture to a CPU buffer via FFmpeg's
                // internal path (which properly synchronizes with D3D11VA).
                // The heavy decode work (H.264/HEVC) is done on the GPU;
                // only the final texture readback is CPU.
                //
                // Why not GPU-to-GPU shared copy? D3D11VA internally owns its
                // device's immediate context for reference frame management.
                // Calling CopySubresourceRegion on it from our code causes a
                // crash. av_hwframe_transfer_data avoids this by going through
                // FFmpeg's internal D3D11VA synchronization path.
                static thread_local AVFrame* swFrame = nullptr;
                if (!swFrame) swFrame = av_frame_alloc();
                swFrame->format = AV_PIX_FMT_NONE;
                if (av_hwframe_transfer_data(swFrame, frame_, 0) >= 0) {
                    const AVPixelFormat swFmt = static_cast<AVPixelFormat>(swFrame->format);
                    const uint32_t w = swFrame->width;
                    const uint32_t h = swFrame->height;
                    if (swFmt == AV_PIX_FMT_NV12 || swFmt == AV_PIX_FMT_YUV420P) {
                        df.nv12 = true;
                        df.bytes.resize(w * h * 3 / 2);
                        if (swFmt == AV_PIX_FMT_NV12) {
                            copyNv12Tightly(df.bytes.data(), swFrame->data, swFrame->linesize, w, h);
                        } else {
                            copyYuv420pToNv12(df.bytes.data(), swFrame->data, swFrame->linesize, w, h);
                        }
                    } else {
                        // P010 or other 10-bit: convert to BGRA via swscale.
                        df.nv12 = false;
                        df.bytes.resize(w * h * 4);
                        if (!swsCtx_ || swsSrcW_ != w || swsSrcH_ != h ||
                            swsSrcFmt_ != static_cast<int>(swFmt)) {
                            sws_freeContext(swsCtx_);
                            swsCtx_ = sws_getContext(w, h, swFmt, w, h,
                                                      AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR,
                                                      nullptr, nullptr, nullptr);
                            swsSrcW_ = w; swsSrcH_ = h; swsSrcFmt_ = static_cast<int>(swFmt);
                        }
                        if (swsCtx_) {
                            uint8_t* dst[1] = {df.bytes.data()};
                            int dstStride[1] = {static_cast<int>(w * 4)};
                            sws_scale(swsCtx_, swFrame->data, swFrame->linesize,
                                      0, h, dst, dstStride);
                        }
                    }
                } else {
                    log.warn(L"FFmpeg: D3D11VA frame transfer failed");
                    av_frame_unref(frame_);
                    continue;
                }
                av_frame_unref(swFrame);
            } else if (hardware_ && frame_->format == AV_PIX_FMT_CUDA) {
                // CUDA -> D3D11 zero-copy: map CUDA frame to D3D11 texture.
                AVFrame* d3d11Frame = av_frame_alloc();
                d3d11Frame->format = AV_PIX_FMT_D3D11;
                d3d11Frame->hw_frames_ctx = av_buffer_ref(hwFramesCtx_);
                bool mapped = (av_hwframe_map(d3d11Frame, frame_, 0) == 0);
                if (mapped) {
                    auto* desc = reinterpret_cast<AVD3D11FrameDescriptor*>(d3d11Frame->data[0]);
                    if (desc && desc->texture) {
                        desc->texture->AddRef(); // ComPtr needs its own reference
                        df.texture.Attach(desc->texture);
                        df.hardware = true;
                    }
                }
                av_frame_unref(d3d11Frame);
                av_frame_free(&d3d11Frame);
                if (!df.hardware) { av_frame_unref(frame_); continue; }
            } else if (frame_->format == AV_PIX_FMT_YUV420P || frame_->format == AV_PIX_FMT_NV12) {
                // Software NV12
                df.nv12 = true;
                const uint32_t w = frame_->width;
                const uint32_t h = frame_->height;
                df.bytes.resize(w * h * 3 / 2);
                if (frame_->format == AV_PIX_FMT_NV12) {
                    copyNv12Tightly(df.bytes.data(), frame_->data, frame_->linesize, w, h);
                } else {
                    copyYuv420pToNv12(df.bytes.data(), frame_->data, frame_->linesize, w, h);
                }
            } else {
                // Software BGRA fallback — reuse cached SwsContext across
                // frames (avoids ~0.5 ms alloc/free overhead per frame).
                uint32_t w = frame_->width;
                uint32_t h = frame_->height;
                uint32_t stride = w * 4;
                df.bytes.resize(stride * h);
                const auto srcFmt = static_cast<AVPixelFormat>(frame_->format);
                if (!swsCtx_ || swsSrcW_ != w || swsSrcH_ != h ||
                    swsSrcFmt_ != static_cast<int>(srcFmt)) {
                    sws_freeContext(swsCtx_);
                    swsCtx_ = sws_getContext(w, h, srcFmt, w, h,
                                              AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR,
                                              nullptr, nullptr, nullptr);
                    swsSrcW_ = w; swsSrcH_ = h; swsSrcFmt_ = static_cast<int>(srcFmt);
                }
                if (swsCtx_) {
                    uint8_t* dst[1] = {df.bytes.data()};
                    int dstStride[1] = {static_cast<int>(stride)};
                    sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, h, dst, dstStride);
                }
            }

            df.decodeTime100ns = util::Clock::instance().now100ns();
            if (!queue->push(std::move(df))) break;
            decodedFrames_++;
        }
    }

    av_packet_free(&pkt);
    log.info(L"FFmpeg decode worker finished ({} frames)", decodedFrames_.load());
}

Result<VideoMetadata> FFmpegDecoder::probeMetadata(const std::wstring& path) {
    std::string pathUtf8 = vw::util::wideToUtf8(path);

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
    VideoMetadata meta;
    fillMetadata(meta, fmtCtx, vidIdx, par);

    avformat_close_input(&fmtCtx);
    return meta;
}

} // namespace vw::video

#pragma warning(pop)
