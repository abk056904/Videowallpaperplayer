#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <windows.h>

#include "playback/FrameScheduler.h"
#include "util/Result.h"
#include "video/VideoMetadata.h"

struct ID3D11Device; // d3d11 interface, forward-declared (pointers only)

namespace vw::video {
class VideoPlayer;
struct DecodedFrame;
} // namespace vw::video

namespace vw::playback {

// Per-session playback statistics (docs/03 §3.8, M6 collects; the StatsCollector
// subscribes to these at M9). Rates are recomputed once per second of wall time.
struct PlaybackStats {
    double decodedFps = 0.0;     // frames produced by the decode worker / s
    double presentedFps = 0.0;   // frames presented (uploaded + rendered) / s
    uint64_t droppedFrames = 0;  // stale frames dropped by the queue (freshness)
    double decodeLatencyMs = 0.0; // avg decode->present latency over the last second
    double renderTimeMs = 0.0;    // most recent render/present duration
    uint64_t presentedFrames = 0; // cumulative
    uint64_t decodedFrames = 0;   // cumulative
};

// Owns one playback session: VideoPlayer + FrameQueue (inside the player) +
// FrameScheduler + the waitable timer, and presents the scheduling contract
// (docs/03 §3.8, M6). The app's message loop waits on timerHandle() +
// newFrameEvent() and calls onWake() on any signal; the controller drains the
// queue per the scheduler (newest frame whose media timestamp is due, stale
// frames dropped) and returns the frame to present. Zero busy-wait: between
// deadlines the loop blocks on the waitable timer / event / messages.
class PlaybackController {
public:
    enum class State { Stopped, Playing, Paused };

    PlaybackController();
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    // Opens + validates the file (real media metadata) and configures the
    // scheduler from its FPS. `d3dDevice` enables the M5 hardware path
    // (nullptr = software-only); `queueCapacity` is config.playback.frameQueue.
    Result<void> open(const std::wstring& path, ID3D11Device* d3dDevice,
                      size_t queueCapacity);

    // Starts the decode worker + pacing. Idempotent while Playing.
    Result<void> start();

    // Pauses: stops the decoder (position preserved), clears the queue,
    // cancels the timer. Safe when not playing.
    void pause();

    // Resumes from the saved position (fresh queue + re-anchored timeline).
    Result<void> resume();

    // Stops playback entirely and logs the session stats summary.
    void stop();

    // M7 loop same video (docs §34): replays the currently open file reusing
    // the reader/decoder + GPU resources — resets the timeline + position
    // only (no reopen, no hardware re-probe). Valid after EOS.
    Result<void> replay();

    State state() const { return state_; }
    bool isOpen() const { return player_ != nullptr; }
    const video::VideoMetadata& metadata() const;
    bool hardwareDecoding() const;
    const std::wstring& decoderName() const;

    // ---- message-loop integration (M6) ----
    // Waitable timer armed to the next presentation deadline (valid while
    // Playing). New-frame event from the decoder's queue (valid while Playing;
    // null otherwise).
    HANDLE timerHandle() const { return timer_; }
    HANDLE newFrameEvent() const;

    // Call on ANY wake (deadline fired or new frame arrived): drains the queue
    // per the scheduler and returns the frame to present (nullopt = nothing
    // due; the timer has been re-armed). An EOS frame is returned for the
    // caller to stop on. The caller uploads + renders and calls
    // noteRenderTime(). Never blocks.
    std::optional<video::DecodedFrame> onWake();

    // Feeds the measured render/present duration (wall clock) into the stats.
    void noteRenderTime(double ms);

    const PlaybackStats& stats() const { return stats_; }

    // Observer invoked after each per-second stats recompute (~1 Hz while
    // Playing; NOT invoked while paused/stopped — values freeze). The app
    // wires this to StatsCollector (spec §10.12 telemetry stream).
    void setStatsObserver(std::function<void(const PlaybackStats&)> observer) {
        statsObserver_ = std::move(observer);
    }

private:
    void armTimer();
    void cancelTimer();
    void updateStats(LONGLONG now, LONGLONG frameDecodeTime100ns);
    void logStatsSummary() const;

    std::unique_ptr<video::VideoPlayer> player_;
    FrameScheduler scheduler_;
    State state_ = State::Stopped;
    HANDLE timer_ = nullptr;
    bool anchorPending_ = true; // re-anchor to the first frame's PTS (start only)

    PlaybackStats stats_;
    LONGLONG statsWindowStart_ = 0; // wall 100 ns at the last per-second update
    uint64_t decodedAtWindowStart_ = 0;
    uint64_t presentedAtWindowStart_ = 0;
    double latencySumMs_ = 0.0;
    uint64_t latencyCount_ = 0;
    LONGLONG lastStatsLog_ = 0; // wall 100 ns of the last DEBUG stats line
    std::function<void(const PlaybackStats&)> statsObserver_;
};

} // namespace vw::playback
