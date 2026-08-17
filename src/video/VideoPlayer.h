#pragma once

#include <d3d11.h>
#include <memory>

#include "util/Result.h"
#include "video/DecodedFrame.h"
#include "video/DecoderManager.h"
#include "video/FrameQueue.h"

namespace vw::video {

// Playback session glue (docs/03 §3.6, M4): owns the DecoderManager + the
// FrameQueue and exposes the UI-thread contract — open/start, pause/resume/
// stop, and pollFrame() (called on the frame timer; the caller uploads +
// renders). Pacing is a simple frame-interval timer for M4; M6 replaces it
// with the FrameScheduler. UI-thread calls are never blocked (the decode
// worker owns the queue producer side).
class VideoPlayer {
public:
    enum class State { Stopped, Playing, Paused };

    VideoPlayer() = default;
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Supplies the D3D device for the hardware decode path (M5; forwarded to
    // the DecoderManager before open). nullptr = software-only.
    void setD3DDevice(ID3D11Device* device) { decoder_.setD3DDevice(device); }

    // Opens + validates the file (real media metadata). Does not start
    // decoding. Idempotent re-open.
    Result<void> open(const std::wstring& path);

    // Starts the decode worker at the current position.
    Result<void> start();

    // Pauses: joins the worker, clears the queue, keeps the position. Safe
    // when not playing.
    void pause();

    // Resumes from the saved position (fresh worker + queue).
    Result<void> resume();

    // Stops playback entirely (worker joined, queue cleared, position reset).
    void stop();

    // UI-thread frame pull: returns true when a new frame is available.
    // `frame.endOfStream` marks the end of the stream (caller should stop).
    bool pollFrame(DecodedFrame& out);

    State state() const { return state_; }
    const VideoMetadata& metadata() const { return decoder_.metadata(); }
    LONGLONG position100ns() const { return position_; }
    bool isOpen() const { return opened_; }
    bool hardwareDecoding() const { return decoder_.hardwareDecoding(); }
    const std::wstring& decoderName() const { return decoder_.decoderName(); }

private:
    void tearDown(); // join worker + close queue (idempotent)

    DecoderManager decoder_;
    std::unique_ptr<FrameQueue> queue_;
    State state_ = State::Stopped;
    LONGLONG position_ = 0;
    bool opened_ = false;
};

} // namespace vw::video
