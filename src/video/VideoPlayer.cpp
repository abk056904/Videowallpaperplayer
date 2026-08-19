#include "video/VideoPlayer.h"

#include <utility>

#include "logging/Logger.h"
#include "video/DecoderFactory.h"

namespace vw::video {

VideoPlayer::~VideoPlayer() { stop(); }

Result<void> VideoPlayer::open(const std::wstring& path) {
    stop();

    // Use factory to select best decoder backend
    AdapterInfo adapterInfo;
    decoder_ = CreateBestDecoder(d3dDevice_, path, &adapterInfo);
    if (!decoder_) {
        return std::unexpected(std::wstring(L"VideoPlayer::open: no decoder available"));
    }

    opened_ = true;
    position_ = 0;
    auto& log = log::Logger::instance();
    const auto& m = metadata();
    log.info(L"video opened: {} | {}x{} @ {:.2f} fps, {} ms, codec {}, {}-bit{}, audio={} | decoder: {}",
             path, m.width, m.height, m.fps, m.duration100ns / 10000, m.codec, m.bitDepth,
             m.hdr ? L" HDR" : L"", m.hasAudio ? L"yes" : L"no", decoderName());
    return {};
}

Result<void> VideoPlayer::start() {
    if (!opened_) return std::unexpected(L"VideoPlayer::start: no file opened");
    if (state_ == State::Playing) return {};

    queue_ = std::make_unique<FrameQueue>(queueCapacity_);
    auto result = decoder_->start(queue_.get(), position_);
    if (!result) return result;

    state_ = State::Playing;
    log::Logger::instance().info(L"playback started (position {} ms)", position_ / 10000);
    return {};
}

Result<void> VideoPlayer::replay() {
    if (!opened_) return std::unexpected(L"VideoPlayer::replay: no open file");
    decoder_->stop();
    if (queue_) queue_.reset();
    position_ = 0;
    state_ = State::Stopped;
    return start();
}

void VideoPlayer::pause() {
    if (state_ != State::Playing) return;
    decoder_->stop();
    if (queue_) { queue_->close(); queue_.reset(); }
    state_ = State::Paused;
}

Result<void> VideoPlayer::resume() {
    if (state_ != State::Paused)
        return state_ == State::Stopped
                   ? std::unexpected(L"VideoPlayer::resume: not paused (stopped)")
                   : Result<void>{};
    return start();
}

void VideoPlayer::stop() {
    if (!opened_ && state_ == State::Stopped) return;
    tearDown();
    state_ = State::Stopped;
    position_ = 0;
    log::Logger::instance().info(L"playback stopped");
}

bool VideoPlayer::pollFrame(DecodedFrame& out) {
    if (state_ != State::Playing || !queue_) return false;
    if (!queue_->tryPop(out)) return false;
    if (out.endOfStream) return true;
    position_ = out.timestamp;
    return true;
}

void VideoPlayer::tearDown() {
    if (queue_) queue_->close();
    decoder_->close();
    queue_.reset();
}

const VideoMetadata& VideoPlayer::metadata() const {
    static const VideoMetadata empty;
    return decoder_ ? decoder_->metadata() : empty;
}

uint64_t VideoPlayer::decodedFrames() const {
    return decoder_ ? decoder_->decodedFrames() : 0;
}

bool VideoPlayer::hardwareDecoding() const {
    return decoder_ ? decoder_->isHardwareDecoding() : false;
}

const std::wstring& VideoPlayer::decoderName() const {
    static const std::wstring empty;
    return decoder_ ? decoder_->decoderName() : empty;
}

} // namespace vw::video
