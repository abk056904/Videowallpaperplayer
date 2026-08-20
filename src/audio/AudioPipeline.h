#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include "util/Result.h"

// Forward-declare FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct SwrContext;
struct AVFrame;

namespace vw::audio {

// Audio clock for A/V synchronization. The audio output thread is the master
// clock — its WASAPI playback position drives the video scheduler.
class AudioClock {
public:
    // Get current playback position in 100ns units
    LONGLONG getPosition() const;

    // Get latency (buffered amount in 100ns units)
    LONGLONG getLatency() const;

    // Update base position (for seeking)
    void setBasePosition(LONGLONG position100ns);

    // Set sample rate for conversion
    void setSampleRate(int rate) { sampleRate_ = rate; }

    // Set padding callback (called by AudioPipeline)
    void setPaddingGetter(std::function<UINT32()> getter) { paddingGetter_ = std::move(getter); }

private:
    LONGLONG basePosition_ = 0;
    int sampleRate_ = 48000;
    std::function<UINT32()> paddingGetter_;
};

// Audio output via WASAPI (shared or exclusive mode)
class AudioOutput {
public:
    enum class ShareMode { Shared, Exclusive };

    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Initialize with desired format
    Result<void> init(ShareMode mode = ShareMode::Shared,
                      int sampleRate = 48000,
                      int channels = 2);

    // Start/stop playback
    Result<void> start();
    void stop();

    // Write audio data (blocking until buffer space available)
    Result<void> write(const uint8_t* data, uint32_t size);

    // Query
    bool isPlaying() const { return isPlaying_; }
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    uint32_t bufferSizeFrames() const { return bufferFrameCount_; }

    // Get IAudioClient for clock access
    Microsoft::WRL::ComPtr<IAudioClient> audioClient() const { return audioClient_; }

    // Volume control via ISimpleAudioVolume
    void setVolume(float volume);
    float getVolume() const;
    void setMuted(bool muted);
    bool isMuted() const;

private:
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> renderClient_;
    Microsoft::WRL::ComPtr<ISimpleAudioVolume> volumeCtrl_;

    ShareMode shareMode_ = ShareMode::Shared;
    int sampleRate_ = 48000;
    int channels_ = 2;
    uint32_t bufferFrameCount_ = 0;
    bool isPlaying_ = false;
};

// FFmpeg audio decoder with swresample conversion
class AudioDecoder {
public:
    AudioDecoder() = default;
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // Initialize with FFmpeg format context
    Result<void> init(AVFormatContext* fmtCtx, int audioStreamIdx);

    // Start decoding thread
    Result<void> start();

    // Stop decoding
    void stop();

    // Get next decoded frame (blocks until available or stopped)
    // Returns nullptr on stop/error
    AVFrame* getFrame();

    // Get audio stream info
    int sampleRate() const { return outSampleRate_; }
    int channels() const { return outChannels_; }

    // Convert an AVFrame to output format (S16 interleaved)
    // Returns number of output samples per channel, or < 0 on error
    int convertFrame(AVFrame* inFrame, uint8_t** outBuffer, int outBufferSize);

private:
    AVFormatContext* fmtCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    SwrContext* swr_ = nullptr;

    int audioStreamIdx_ = -1;
    int outSampleRate_ = 48000;
    int outChannels_ = 2;

    std::thread decodeThread_;
    std::atomic<bool> stopRequested_{false};

    // Frame queue (bounded)
    std::vector<AVFrame*> frameQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCond_;
    std::condition_variable queueNotFull_;
    static constexpr size_t kMaxQueueSize = 16;
};

// Complete audio pipeline: FFmpeg decode + swresample conversion + WASAPI output.
// The pipeline owns its own AVFormatContext (separate from the video decoder's)
// and finds the audio stream automatically on init(). On files without audio,
// init() returns success with hasAudio() == false — never an error.
class AudioPipeline {
public:
    AudioPipeline() = default;
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    // Initialize with a video file (finds and opens audio stream)
    Result<void> init(const std::wstring& path);

    // Start/stop/pause/resume
    Result<void> start();
    void stop();
    void pause();
    void resume();

    // Get audio clock for A/V sync
    AudioClock* clock() const { return clock_.get(); }

    // Check if audio is available
    bool hasAudio() const { return hasAudio_; }

    // Volume control (0.0 = mute, 1.0 = full, >1.0 = boost)
    void setVolume(float volume);
    float getVolume() const { return volume_; }
    void setMuted(bool muted);
    bool isMuted() const { return muted_; }

    // Get audio metadata
    int sampleRate() const { return decoder_ ? decoder_->sampleRate() : 0; }
    int channels() const { return decoder_ ? decoder_->channels() : 0; }

private:
    void audioThreadFunc();

    std::unique_ptr<AudioDecoder> decoder_;
    std::unique_ptr<AudioOutput> output_;
    std::unique_ptr<AudioClock> clock_;

    AVFormatContext* fmtCtx_ = nullptr;
    int audioStreamIdx_ = -1;

    std::thread audioThread_;
    std::atomic<bool> stopRequested_{false};
    bool hasAudio_ = false;
    bool isPaused_ = false;
    float volume_ = 1.0f;
    bool muted_ = false;
};

} // namespace vw::audio
