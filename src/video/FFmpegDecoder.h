#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <d3d11.h>

#include "util/Result.h"
#include "video/FrameQueue.h"
#include "video/VideoMetadata.h"

// Forward-declare FFmpeg types to avoid polluting the header.
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace vw::video {

// FFmpeg-based decode session: replaces the Media Foundation DecoderManager
// with libavcodec/libavformat. Supports NVDEC hardware decode (h264_cuvid,
// hevc_cuvid) via FFmpeg's hwaccel, falling back to software when the GPU
// decoder is unavailable.
//
// Public interface matches DecoderManager so the rest of the engine
// (VideoPlayer, PlaybackController) needs minimal changes.
class FFmpegDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder();

    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    void setD3DDevice(ID3D11Device* device) { d3dDevice_ = device; }

    Result<void> open(const std::wstring& path);
    const VideoMetadata& metadata() const { return metadata_; }
    bool hardwareDecoding() const { return hardware_; }
    const std::wstring& decoderName() const { return decoderName_; }

    Result<void> start(FrameQueue* queue, LONGLONG position100ns = 0);
    void stop();
    void close();

    static Result<VideoMetadata> probeMetadata(const std::wstring& path);

    bool isRunning() const { return worker_.joinable(); }
    uint64_t decodedFrames() const { return decodedFrames_.load(); }

private:
    void workerLoop(FrameQueue* queue);
    bool tryOpenHw(const std::wstring& path);
    bool tryOpenSw(const std::wstring& path);

    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* rgbFrame_ = nullptr;   // software decode: RGB conversion target
    AVBufferRef* hwCtx_ = nullptr;  // HW device context (CUDA)
    int videoStreamIdx_ = -1;
    ID3D11Device* d3dDevice_ = nullptr;

    VideoMetadata metadata_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> decodedFrames_{0};
    FrameQueue* queue_ = nullptr;
    std::wstring decoderName_;
    bool hardware_ = false;
    bool opened_ = false;
};

} // namespace vw::video
