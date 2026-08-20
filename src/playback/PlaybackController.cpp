#include "playback/PlaybackController.h"

#include <utility>

#include "audio/AudioPipeline.h"
#include "logging/Logger.h"
#include "util/clock.h"
#include "video/DecodedFrame.h"
#include "video/VideoPlayer.h"

namespace vw::playback {

namespace {
constexpr LONGLONG k100nsPerSecond = 10'000'000LL;
} // namespace

PlaybackController::PlaybackController() {
    // High-resolution waitable timer (Win10 1803+); plain timer as fallback.
    timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!timer_) {
        timer_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
}

PlaybackController::~PlaybackController() {
    stop();
    if (timer_) {
        ::CloseHandle(timer_);
    }
}

Result<void> PlaybackController::open(const std::wstring& path, ID3D11Device* d3dDevice,
                                      size_t queueCapacity, bool enableAudio, int volume) {
    stop();
    auto player = std::make_unique<video::VideoPlayer>();
    if (d3dDevice) {
        player->setD3DDevice(d3dDevice);
    }
    player->setQueueCapacity(queueCapacity);
    auto result = player->open(path);
    if (!result) {
        return result; // player discarded; state stays Stopped
    }
    scheduler_.setSourceFps(player->metadata().fps);
    player_ = std::move(player);
    stats_ = {};

    // Initialize audio pipeline only if enabled in config AND the file has audio.
    // Default is OFF per spec §1.8 ("audio = disabled").
    if (enableAudio && player_->metadata().hasAudio) {
        audioPipeline_ = std::make_unique<audio::AudioPipeline>();
        auto audioResult = audioPipeline_->init(path);
        if (!audioResult) {
            log::Logger::instance().warn(L"audio init failed: {}", audioResult.error());
            audioPipeline_.reset(); // Continue without audio
        } else {
            log::Logger::instance().info(L"audio pipeline initialized (enabled by config)");
            audioPipeline_->setVolume(static_cast<float>(volume) / 100.0f * 10.0f);
        }
    }

    return {};
}

Result<void> PlaybackController::start() {
    if (!player_) {
        return std::unexpected(L"PlaybackController::start: no file opened");
    }
    if (state_ == State::Playing) {
        return {};
    }
    // Anchor the timeline: media time 0 is due now (start-of-file playback).
    // onWake() re-anchors to the first frame's actual PTS when it arrives
    // (anchorPending_) — files with a nonzero initial PTS start immediately.
    scheduler_.reset(util::Clock::instance().now100ns(), 0);
    anchorPending_ = true;
    auto result = player_->start();
    if (!result) {
        return result;
    }
    // Start audio if available
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        auto audioResult = audioPipeline_->start();
        if (!audioResult) {
            log::Logger::instance().warn(L"audio start failed: {}", audioResult.error());
        }
    }
    state_ = State::Playing;
    statsWindowStart_ = util::Clock::instance().now100ns();
    decodedAtWindowStart_ = player_->decodedFrames();
    presentedAtWindowStart_ = stats_.presentedFrames;
    armTimer();
    return {};
}

void PlaybackController::pause() {
    if (state_ != State::Playing || !player_) {
        return;
    }
    cancelTimer();
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        audioPipeline_->pause();
    }
    player_->pause(); // joins the worker; keeps position for resume
    state_ = State::Paused;
    logStatsSummary(); // DEBUG detail at pause boundaries
    log::Logger::instance().info(L"playback paused at {} ms",
                                 player_->position100ns() / 10000);
}

Result<void> PlaybackController::resume() {
    if (state_ != State::Paused || !player_) {
        return state_ == State::Stopped
                   ? std::unexpected(L"PlaybackController::resume: not paused (stopped)")
                   : Result<void>{};
    }
    // Re-anchor: the saved position is due now (the decode worker seeks there).
    // The first resumed frame is already >= the position — no re-anchor.
    scheduler_.reset(util::Clock::instance().now100ns(), player_->position100ns());
    anchorPending_ = false;
    auto result = player_->resume();
    if (!result) {
        return result;
    }
    // Resume audio
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        audioPipeline_->resume();
    }
    state_ = State::Playing;
    statsWindowStart_ = util::Clock::instance().now100ns();
    decodedAtWindowStart_ = player_->decodedFrames();
    presentedAtWindowStart_ = stats_.presentedFrames;
    armTimer();
    return {};
}

void PlaybackController::stop() {
    cancelTimer();
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        audioPipeline_->stop();
    }
    if (player_) {
        player_->stop();
        player_.reset();
    }
    audioPipeline_.reset();
    if (state_ != State::Stopped) {
        logStatsSummary();
    }
    state_ = State::Stopped;
}

Result<void> PlaybackController::replay() {
    if (!player_ || !player_->isOpen()) {
        return std::unexpected(L"PlaybackController::replay: no open file");
    }
    // Loop same video: reuse the reader/decoder + GPU resources; reset the
    // timeline to media time 0 and the per-session stats (fresh loop cycle).
    cancelTimer();
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        audioPipeline_->stop();
    }
    scheduler_.reset(util::Clock::instance().now100ns(), 0);
    anchorPending_ = true;
    stats_ = {};
    auto result = player_->replay();
    if (!result) {
        return result;
    }
    // Restart audio from the beginning
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        auto audioResult = audioPipeline_->start();
        if (!audioResult) {
            log::Logger::instance().warn(L"audio replay start failed: {}", audioResult.error());
        }
    }
    state_ = State::Playing;
    statsWindowStart_ = util::Clock::instance().now100ns();
    decodedAtWindowStart_ = player_->decodedFrames();
    presentedAtWindowStart_ = stats_.presentedFrames;
    armTimer();
    log::Logger::instance().info(L"playback looping (position reset)");
    return {};
}

const video::VideoMetadata& PlaybackController::metadata() const {
    static const video::VideoMetadata empty;
    return player_ ? player_->metadata() : empty;
}

bool PlaybackController::hardwareDecoding() const {
    return player_ && player_->hardwareDecoding();
}

const std::wstring& PlaybackController::decoderName() const {
    static const std::wstring empty;
    return player_ ? player_->decoderName() : empty;
}

HANDLE PlaybackController::newFrameEvent() const {
    // Null-safe: the queue only exists while Playing (pause destroys it); the
    // message loop additionally gates on state(), but never deref a null queue.
    return player_ && player_->queue() ? player_->queue()->newFrameEvent() : nullptr;
}

std::optional<video::DecodedFrame> PlaybackController::onWake() {
    const LONGLONG now = util::Clock::instance().now100ns();
    if (state_ != State::Playing || !player_) {
        return std::nullopt;
    }
    video::FrameQueue* queue = player_->queue();
    if (!queue) {
        return std::nullopt; // pause raced a wake; the pump re-checks state
    }
    // Reset BEFORE draining: a push during the drain re-signals the event
    // (no lost wakeups).
    ::ResetEvent(queue->newFrameEvent());

    // Anchor to the FIRST frame's media timestamp so playback starts the
    // moment the first frame is ready: files with a nonzero initial PTS (edit
    // lists, trimmed starts) would otherwise show the placeholder for the PTS
    // offset. Resume re-anchors to the saved position instead (already due).
    if (anchorPending_) {
        if (auto ts = queue->peekTimestamp(); ts.has_value()) {
            scheduler_.reset(now, *ts);
            anchorPending_ = false;
        }
    }

    video::DecodedFrame frame;
    const bool got = scheduler_.pacingEnabled()
                         ? queue->popNewestUpTo(scheduler_.dueMediaAt(now), frame)
                         : player_->pollFrame(frame);
    if (!got) {
        // Deadline passed with nothing due (decoder still working): re-arm
        // relative to now — never a past deadline (no busy re-fire).
        scheduler_.advanceIdle(now);
        armTimer();
        return std::nullopt;
    }
    if (frame.endOfStream) {
        armTimer(); // caller stops immediately; harmless
        return frame;
    }
    scheduler_.advanceAfterPresent(now, frame.timestamp);
    ++stats_.presentedFrames;
    player_->setPosition(frame.timestamp); // keeps resume/seek accurate
    updateStats(now, frame.decodeTime100ns);
    armTimer();
    return frame;
}

void PlaybackController::noteRenderTime(double ms) {
    stats_.renderTimeMs = ms;
}

void PlaybackController::recycleFrame(std::vector<uint8_t>& bytes) {
    // Null-safe like newFrameEvent(): the queue only exists while Playing.
    if (player_ && player_->queue()) {
        player_->queue()->recycleBuffer(bytes);
    }
}

void PlaybackController::armTimer() {
    if (!timer_ || !scheduler_.pacingEnabled()) {
        return; // pacing disabled: presentation is new-frame-event driven
    }
    const LONGLONG now = util::Clock::instance().now100ns();
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(scheduler_.msUntilNextDeadline(now)) * 10000;
    ::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
}

void PlaybackController::cancelTimer() {
    if (timer_) {
        ::CancelWaitableTimer(timer_);
    }
}

void PlaybackController::setPlaybackSpeed(double speed) {
    // Clamp to a sensible range: 0.25× – 4×.
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0) speed = 4.0;
    scheduler_.setSpeed(speed);
    if (state_ == State::Playing) {
        armTimer();
    }
}

void PlaybackController::setVolume(int volume) {
    volume = std::max(0, std::min(volume, 100));
    if (audioPipeline_ && audioPipeline_->hasAudio()) {
        audioPipeline_->setVolume(static_cast<float>(volume) / 100.0f * 10.0f);
    }
}

void PlaybackController::stopAudio() {
    if (audioPipeline_) {
        audioPipeline_->stop();
        audioPipeline_.reset();
    }
}

void PlaybackController::updateStats(LONGLONG now, LONGLONG frameDecodeTime100ns) {
    // Latency: decode wall time (stamped by the worker) -> presentation now.
    if (frameDecodeTime100ns > 0) {
        latencySumMs_ += static_cast<double>(now - frameDecodeTime100ns) / 10000.0;
        ++latencyCount_;
    }
    if (now - statsWindowStart_ < k100nsPerSecond) {
        return; // one rate sample per second of wall time
    }
    const double elapsedSec =
        static_cast<double>(now - statsWindowStart_) / static_cast<double>(k100nsPerSecond);
    const uint64_t decoded = player_ ? player_->decodedFrames() : 0;
    stats_.decodedFps = static_cast<double>(decoded - decodedAtWindowStart_) / elapsedSec;
    stats_.presentedFps =
        static_cast<double>(stats_.presentedFrames - presentedAtWindowStart_) / elapsedSec;
    stats_.decodedFrames = decoded;
    stats_.droppedFrames = player_ && player_->queue() ? player_->queue()->droppedFrames() : 0;
    stats_.decodeLatencyMs = latencyCount_ > 0 ? latencySumMs_ / static_cast<double>(latencyCount_)
                                               : 0.0;
    latencySumMs_ = 0.0;
    latencyCount_ = 0;
    statsWindowStart_ = now;
    decodedAtWindowStart_ = decoded;
    presentedAtWindowStart_ = stats_.presentedFrames;

    // Feed the telemetry stream (StatsCollector via the app, spec §10.12):
    // ~1 Hz while playing. The observer must not block (it only copies stats
    // into the collector's snapshot).
    if (statsObserver_) {
        statsObserver_(stats_);
    }

    // Aggregate DEBUG stats ~ every 5 s (never spams the INFO log).
    if (now - lastStatsLog_ >= 5 * k100nsPerSecond) {
        lastStatsLog_ = now;
        log::Logger::instance().debug(
            L"stats: decoded {:.1f} fps, presented {:.1f} fps, dropped {}, "
            L"decodeLatency {:.1f} ms, render {:.1f} ms",
            stats_.decodedFps, stats_.presentedFps, stats_.droppedFrames,
            stats_.decodeLatencyMs, stats_.renderTimeMs);
    }
}

void PlaybackController::logStatsSummary() const {
    log::Logger::instance().info(
        L"playback session: decoded {:.1f} fps, presented {:.1f} fps, {} frame(s) "
        L"dropped, avg decodeLatency {:.1f} ms, render {:.1f} ms, {} frame(s) presented",
        stats_.decodedFps, stats_.presentedFps, stats_.droppedFrames, stats_.decodeLatencyMs,
        stats_.renderTimeMs, stats_.presentedFrames);
}

} // namespace vw::playback
