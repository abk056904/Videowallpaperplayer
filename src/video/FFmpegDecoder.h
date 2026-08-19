#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <d3d11.h>

#include "util/Result.h"
#include "video/FrameQueue.h"
#include "video/IVideoDecoder.h"
#include "video/VideoMetadata.h"

// Forward-declare FFmpeg types to avoid polluting the header.
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace vw::video {

// FFmpeg-based decode session supporting both NVDEC (CUDA) and D3D11VA
// hardware acceleration, falling back to software when GPU decode is
// unavailable. Implements IVideoDecoder for backend-agnostic use.
//
// Zero-copy paths:
//   D3D11VA: frames arrive as ID3D11Texture2D (AV_PIX_FMT_D3D11)
//   CUDA: frames are mapped to D3D11 via av_hwframe_map()
//   Software: frames are uploaded to D3D11 via TextureManager
class FFmpegDecoder : public IVideoDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder() override;

    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    void setD3DDevice(ID3D11Device* device) { d3dDevice_ = device; }

    // IVideoDecoder interface
    Result<void> open(const std::wstring& path) override;
    Result<void> start(FrameQueue* queue, LONGLONG position100ns = 0) override;
    void stop() override;
    void close() override;

    bool isHardwareDecoding() const override { return hardware_; }
    const std::wstring& decoderName() const override { return decoderName_; }
    const VideoMetadata& metadata() const override { return metadata_; }
    bool isOpen() const override { return opened_; }
    uint64_t decodedFrames() const override { return decodedFrames_.load(); }

    // Metadata-only probe (no decode pipeline).
    static Result<VideoMetadata> probeMetadata(const std::wstring& path);

private:
    void workerLoop(FrameQueue* queue);
    bool tryOpenD3d11va(const std::wstring& path);
    bool tryOpenCuda(const std::wstring& path);
    bool tryOpenSw(const std::wstring& path);

    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVBufferRef* hwCtx_ = nullptr;      // HW device context (CUDA or D3D11VA)
    AVBufferRef* hwFramesCtx_ = nullptr; // HW frames context (for D3D11 mapping)
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
