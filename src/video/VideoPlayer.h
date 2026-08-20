#pragma once

#include <d3d11.h>
#include <memory>

#include "util/Result.h"
#include "video/DecodedFrame.h"
#include "video/FrameQueue.h"
#include "video/IVideoDecoder.h"

namespace vw::video {

// Playback session glue: owns the IVideoDecoder + FrameQueue and exposes
// the UI-thread contract — open/start, pause/resume/stop, and pollFrame().
// The decoder backend is selected automatically via CreateBestDecoder() in
// DecoderFactory.h (MF → D3D11VA → CUDA → FFmpeg SW).
class VideoPlayer {
public:
    enum class State { Stopped, Playing, Paused };

    VideoPlayer() = default;
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    void setD3DDevice(ID3D11Device* device) { d3dDevice_ = device; }

    Result<void> open(const std::wstring& path);
    Result<void> start();
    Result<void> replay();
    void setQueueCapacity(size_t capacity) { queueCapacity_ = capacity == 0 ? 1 : capacity; }
    void pause();
    Result<void> resume();
    void stop();

    bool pollFrame(DecodedFrame& out);

    State state() const { return state_; }
    const VideoMetadata& metadata() const;
    LONGLONG position100ns() const { return position_; }
    FrameQueue* queue() const { return queue_.get(); }
    void setPosition(LONGLONG pos) { position_ = pos; }
    uint64_t decodedFrames() const;
    bool isOpen() const { return opened_; }
    bool hardwareDecoding() const;
    const std::wstring& decoderName() const;

private:
    void tearDown();

    std::unique_ptr<IVideoDecoder> decoder_;
    std::unique_ptr<FrameQueue> queue_;
    ID3D11Device* d3dDevice_ = nullptr;
    State state_ = State::Stopped;
    LONGLONG position_ = 0;
    size_t queueCapacity_ = 1;
    bool opened_ = false;
};

} // namespace vw::video
