#include "library/ThumbnailExtractor.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "logging/Logger.h"
#include "util/FFmpegCompat.h"
#include "util/utf8.h"

namespace vw::library {

namespace {

// Simple BMP writer for 32-bit BGRA images (top-down DIB).
bool writeBmp(const std::wstring& path, const uint8_t* bgra, int width, int height) {
    const int rowBytes = width * 4;
    const int pixelDataSize = rowBytes * height;
    const int headerSize = 14 + 40;
    const int fileSize = headerSize + pixelDataSize;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    // BITMAPFILEHEADER
    uint8_t fh[14]{};
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = fileSize & 0xFF;
    fh[3] = (fileSize >> 8) & 0xFF;
    fh[4] = (fileSize >> 16) & 0xFF;
    fh[5] = (fileSize >> 24) & 0xFF;
    fh[10] = headerSize & 0xFF;
    fh[11] = (headerSize >> 8) & 0xFF;
    out.write(reinterpret_cast<const char*>(fh), 14);

    // BITMAPINFOHEADER (32-bit BGRA, top-down)
    uint8_t ih[40]{};
    ih[0] = 40;
    ih[4] = width & 0xFF;
    ih[5] = (width >> 8) & 0xFF;
    ih[6] = (width >> 16) & 0xFF;
    ih[7] = (width >> 24) & 0xFF;
    const int negH = -height;
    ih[8] = negH & 0xFF;
    ih[9] = (negH >> 8) & 0xFF;
    ih[10] = (negH >> 16) & 0xFF;
    ih[11] = (negH >> 24) & 0xFF;
    ih[12] = 1;
    ih[14] = 32;
    ih[20] = pixelDataSize & 0xFF;
    ih[21] = (pixelDataSize >> 8) & 0xFF;
    ih[22] = (pixelDataSize >> 16) & 0xFF;
    ih[23] = (pixelDataSize >> 24) & 0xFF;
    out.write(reinterpret_cast<const char*>(ih), 40);

    // Pixel data
    for (int y = 0; y < height; ++y) {
        out.write(reinterpret_cast<const char*>(bgra + y * rowBytes), rowBytes);
    }
    return out.good();
}

std::wstring hashPath(const std::wstring& path) {
    uint64_t hash = 5381;
    for (wchar_t c : path) {
        hash = ((hash << 5) + hash) + static_cast<uint64_t>(c);
    }
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%016llx", hash);
    return buf;
}

} // namespace

ThumbnailExtractor::ThumbnailExtractor() {
    worker_ = std::thread(&ThumbnailExtractor::workerLoop, this);
}

ThumbnailExtractor::~ThumbnailExtractor() {
    stop_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ThumbnailExtractor::setCacheDir(const std::wstring& dir) {
    std::lock_guard lock(mu_);
    cacheDir_ = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
}

std::wstring ThumbnailExtractor::cachePathFor(const std::wstring& videoPath) const {
    std::lock_guard lock(mu_);
    return cacheDir_ + L"\\" + hashPath(videoPath) + L".bmp";
}

bool ThumbnailExtractor::isCached(const std::wstring& videoPath) const {
    std::lock_guard lock(mu_);
    const std::wstring path = cacheDir_ + L"\\" + hashPath(videoPath) + L".bmp";
    return std::filesystem::exists(path);
}

void ThumbnailExtractor::request(const std::wstring& videoPath, Callback cb) {
    std::lock_guard lock(mu_);

    const std::wstring cached = cacheDir_ + L"\\" + hashPath(videoPath) + L".bmp";
    if (std::filesystem::exists(cached)) {
        if (cb) cb(videoPath, true, cached);
        return;
    }

    for (auto& req : queue_) {
        if (req.videoPath == videoPath) {
            if (cb) req.callbacks.push_back(std::move(cb));
            return;
        }
    }

    for (const auto& p : inFlight_) {
        if (p == videoPath) {
            if (cb) {
                Request r;
                r.videoPath = videoPath;
                r.callbacks.push_back(std::move(cb));
                queue_.push_back(std::move(r));
            }
            return;
        }
    }

    Request r;
    r.videoPath = videoPath;
    if (cb) r.callbacks.push_back(std::move(cb));
    queue_.push_back(std::move(r));
}

void ThumbnailExtractor::poll() {
    std::vector<Completed> completed;
    {
        std::lock_guard lock(mu_);
        completed.swap(results_);
    }
    for (auto& c : completed) {
        for (auto& cb : c.callbacks) {
            if (cb) cb(c.videoPath, c.success, c.thumbnailPath);
        }
    }
}

size_t ThumbnailExtractor::pendingCount() const {
    std::lock_guard lock(mu_);
    return queue_.size() + inFlight_.size();
}

void ThumbnailExtractor::workerLoop() {
    while (!stop_) {
        Request req;
        {
            std::unique_lock lock(mu_);
            if (queue_.empty()) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            req = std::move(queue_.front());
            queue_.erase(queue_.begin());
            inFlight_.push_back(req.videoPath);
        }

        std::wstring cached;
        bool success = false;
        {
            std::lock_guard lock(mu_);
            cached = cacheDir_ + L"\\" + hashPath(req.videoPath) + L".bmp";
        }

        if (!std::filesystem::exists(cached)) {
            auto result = extractSync(req.videoPath, cached);
            success = result.has_value();
            if (!success) {
                auto& log = log::Logger::instance();
                log.debug(L"thumbnail extract failed for {}: {}", req.videoPath,
                          result.error());
            }
        } else {
            success = true;
        }

        {
            std::lock_guard lock(mu_);
            inFlight_.erase(
                std::remove(inFlight_.begin(), inFlight_.end(), req.videoPath),
                inFlight_.end());
            Completed c;
            c.videoPath = req.videoPath;
            c.success = success;
            c.thumbnailPath = cached;
            c.callbacks = std::move(req.callbacks);
            results_.push_back(std::move(c));
        }
    }
}

Result<std::wstring> ThumbnailExtractor::extractSync(const std::wstring& videoPath,
                                                      const std::wstring& outputPath) {
    if (!avformat_open_input) {
        return std::unexpected(std::wstring(L"FFmpeg not loaded"));
    }

    AVFormatContext* fmtCtx = nullptr;
    const std::string pathUtf8 = vw::util::wideToUtf8(videoPath);
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0) {
        return std::unexpected(std::wstring(L"cannot open video for thumbnail"));
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"cannot read stream info"));
    }

    int videoIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"no video stream"));
    }

    AVStream* stream = fmtCtx->streams[videoIdx];
    AVCodecParameters* par = stream->codecpar;

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"no decoder for codec"));
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, par);
    avcodec_open2(codecCtx, codec, nullptr);

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();

    // Seek to ~1 second.
    const int64_t seekTarget = (stream->time_base.den > 0 && stream->time_base.num > 0)
                                   ? stream->time_base.den / stream->time_base.num
                                   : 0;
    avformat_seek_file(fmtCtx, videoIdx, INT64_MIN, seekTarget, INT64_MAX, 0);

    AVPacket* pkt = av_packet_alloc();
    bool gotFrame = false;
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(codecCtx, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        if (avcodec_receive_frame(codecCtx, frame) >= 0) {
            gotFrame = true;
            break;
        }
    }

    if (!gotFrame) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"no frame decoded"));
    }

    // Scale to thumbnail size.
    const int w = frame->width;
    const int h = frame->height;
    const int maxW = 320;
    const int thumbW = (w > maxW) ? maxW : w;
    const int thumbH = static_cast<int>(static_cast<double>(h) * thumbW / w);

    SwsContext* sws = sws_getContext(
        w, h, static_cast<AVPixelFormat>(frame->format),
        thumbW, thumbH, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_frame_free(&rgbFrame);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return std::unexpected(std::wstring(L"sws_getContext failed"));
    }

    // Allocate RGB buffer manually (av_image_fill_arrays not in API).
    const int rgbLinesize = thumbW * 3;
    const int rgbSize = rgbLinesize * thumbH;
    std::vector<uint8_t> rgbBuf(rgbSize);
    rgbFrame->data[0] = rgbBuf.data();
    rgbFrame->linesize[0] = rgbLinesize;

    sws_scale(sws, frame->data, frame->linesize, 0, h, rgbFrame->data, rgbFrame->linesize);
    sws_freeContext(sws);

    // Convert RGB24 to BGRA for BMP.
    std::vector<uint8_t> bgra(thumbW * thumbH * 4);
    for (int y = 0; y < thumbH; ++y) {
        const uint8_t* src = rgbBuf.data() + y * rgbLinesize;
        uint8_t* dst = bgra.data() + y * thumbW * 4;
        for (int x = 0; x < thumbW; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 2]; // B
            dst[x * 4 + 1] = src[x * 3 + 1]; // G
            dst[x * 4 + 2] = src[x * 3 + 0]; // R
            dst[x * 4 + 3] = 255;
        }
    }

    bool ok = writeBmp(outputPath, bgra.data(), thumbW, thumbH);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (!ok) {
        return std::unexpected(std::wstring(L"BMP write failed"));
    }
    return outputPath;
}

} // namespace vw::library
