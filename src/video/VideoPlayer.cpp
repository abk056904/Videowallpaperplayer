#include "video/VideoPlayer.h"

#include <utility>

#include "logging/Logger.h"

namespace vw::video {

VideoPlayer::~VideoPlayer() {
    stop();
}

Result<void> VideoPlayer::open(const std::wstring& path) {
    stop();
    auto result = decoder_.open(path);
    if (!result) {
        return result;
    }
    opened_ = true;
    position_ = 0;
    auto& log = log::Logger::instance();
    const auto& m = metadata();
    log.info(L"video opened: {} | {}x{} @ {:.2f} fps, {} ms, codec {}, {}-bit{}, audio={}",
             path, m.width, m.height, m.fps, m.duration100ns / 10000, m.codec, m.bitDepth,
             m.hdr ? L" HDR" : L"", m.hasAudio ? L"yes" : L"no");
    return {};
}

Result<void> VideoPlayer::start() {
    if (!opened_) {
        return std::unexpected(L"VideoPlayer::start: no file opened");
    }
    if (state_ == State::Playing) {
        return {};
    }
    queue_ = std::make_unique<FrameQueue>(queueCapacity_);
    auto result = decoder_.start(queue_.get(), position_);
    if (!result) {
        return result;
    }
    state_ = State::Playing;
    log::Logger::instance().info(L"playback started (position {} ms)",
                                 position_ / 10000);
    return {};
}

void VideoPlayer::pause() {
    if (state_ != State::Playing) {
        return;
    }
    decoder_.stop(); // joins the worker; keeps the reader for resume
    if (queue_) {
        queue_->close();
        queue_.reset();
    }
    state_ = State::Paused;
    // (PlaybackController logs the pause position — it is the integration
    // owner and keeps the position in sync; this layer stays quiet.)
}

Result<void> VideoPlayer::resume() {
    if (state_ != State::Paused) {
        return state_ == State::Stopped
                   ? std::unexpected(L"VideoPlayer::resume: not paused (stopped)")
                   : Result<void>{};
    }
    return start(); // fresh queue + worker; start() seeks to position_
}

void VideoPlayer::stop() {
    if (!opened_ && state_ == State::Stopped) {
        return;
    }
    tearDown();
    state_ = State::Stopped;
    position_ = 0;
    log::Logger::instance().info(L"playback stopped");
}

bool VideoPlayer::pollFrame(DecodedFrame& out) {
    if (state_ != State::Playing || !queue_) {
        return false;
    }
    if (!queue_->tryPop(out)) {
        return false;
    }
    if (out.endOfStream) {
        // Consumed the sentinel: the stream is done. Keep state Playing until
        // the caller stops (so the caller can distinguish eos from empty).
        return true;
    }
    position_ = out.timestamp;
    return true;
}

void VideoPlayer::tearDown() {
    if (queue_) {
        queue_->close(); // unblocks a worker blocked on a full queue
    }
    // Join the worker BEFORE destroying the queue: the worker holds a raw
    // pointer and may still be inside push() when it wakes from close() —
    // destroying the queue first destroys the mutex/CV under it (hang).
    decoder_.close(); // join worker + release the reader entirely
    queue_.reset();
}

} // namespace vw::video
