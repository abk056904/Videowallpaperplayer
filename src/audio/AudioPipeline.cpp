#include "audio/AudioPipeline.h"

#include <Windows.h>
#include <objbase.h>
#include <functiondiscoverykeys_devpkey.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>

#include "logging/Logger.h"
#include "util/utf8.h"

using Microsoft::WRL::ComPtr;

// Suppress FFmpeg header warnings
#pragma warning(push)
#pragma warning(disable: 4244 4267 4100 4127 4996)

namespace vw::audio {

// ---- AudioClock ----

LONGLONG AudioClock::getPosition() const {
    if (!paddingGetter_) return basePosition_;
    UINT32 padding = paddingGetter_();
    // Convert padding (frames) to 100ns units
    return basePosition_ - static_cast<LONGLONG>(padding) * 10000000 / sampleRate_;
}

LONGLONG AudioClock::getLatency() const {
    if (!paddingGetter_) return 0;
    UINT32 padding = paddingGetter_();
    return static_cast<LONGLONG>(padding) * 10000000 / sampleRate_;
}

void AudioClock::setBasePosition(LONGLONG position100ns) {
    basePosition_ = position100ns;
}

// ---- AudioOutput ----

AudioOutput::~AudioOutput() { stop(); }

Result<void> AudioOutput::init(ShareMode mode, int sampleRate, int channels) {
    shareMode_ = mode;
    sampleRate_ = sampleRate;
    channels_ = channels;

    auto& log = log::Logger::instance();

    // Get default audio endpoint
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return std::unexpected(L"CoCreateInstance(MMDeviceEnumerator) failed");

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) return std::unexpected(L"GetDefaultAudioEndpoint failed");

    // Activate audio client
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient_.GetAddressOf()));
    if (FAILED(hr)) return std::unexpected(L"IAudioClient activation failed");

    // Build desired format
    WAVEFORMATEX waveFormat{};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = static_cast<WORD>(channels);
    waveFormat.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;

    // Check format support
    WAVEFORMATEX* closestMatch = nullptr;
    AUDCLNT_SHAREMODE shareMode = (mode == ShareMode::Exclusive)
        ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;

    hr = audioClient_->IsFormatSupported(shareMode, &waveFormat, &closestMatch);
    if (hr == S_FALSE && closestMatch) {
        // Use closest match
        waveFormat = *closestMatch;
        sampleRate_ = waveFormat.nSamplesPerSec;
        channels_ = waveFormat.nChannels;
        ::CoTaskMemFree(closestMatch);
        log.info(L"audio: using closest format match: {} Hz, {} ch", sampleRate_, channels_);
    } else if (FAILED(hr)) {
        return std::unexpected(L"audio format not supported");
    }

    // Initialize audio client
    REFERENCE_TIME bufferDuration = 30 * 10000; // 30 ms
    REFERENCE_TIME periodicity = (mode == ShareMode::Exclusive) ? 10 * 10000 : 0;

    hr = audioClient_->Initialize(shareMode, 0, bufferDuration, periodicity, &waveFormat, nullptr);
    if (FAILED(hr)) {
        // Fall back to shared mode if exclusive failed
        if (mode == ShareMode::Exclusive) {
            log.info(L"audio: exclusive mode failed, falling back to shared");
            hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, &waveFormat, nullptr);
            if (FAILED(hr)) return std::unexpected(L"audio init failed (both modes)");
            shareMode_ = ShareMode::Shared;
        } else {
            return std::unexpected(L"audio init failed");
        }
    }

    // Get buffer size
    hr = audioClient_->GetBufferSize(&bufferFrameCount_);
    if (FAILED(hr)) return std::unexpected(L"GetBufferSize failed");

    // Get render client
    hr = audioClient_->GetService(IID_PPV_ARGS(&renderClient_));
    if (FAILED(hr)) return std::unexpected(L"GetService(IAudioRenderClient) failed");

    log.info(L"audio: initialized ({} mode, {} Hz, {} ch, {} frames = {:.1f} ms)",
             shareMode_ == ShareMode::Exclusive ? L"exclusive" : L"shared",
             sampleRate_, channels_, bufferFrameCount_,
             static_cast<double>(bufferFrameCount_) * 1000.0 / sampleRate_);
    return {};
}

Result<void> AudioOutput::start() {
    if (isPlaying_) return {};
    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) return std::unexpected(L"audio start failed");
    isPlaying_ = true;
    return {};
}

void AudioOutput::stop() {
    if (!isPlaying_) return;
    audioClient_->Stop();
    isPlaying_ = false;
}

Result<void> AudioOutput::write(const uint8_t* data, uint32_t size) {
    if (!isPlaying_) return std::unexpected(L"audio not playing");

    UINT32 padding = 0;
    HRESULT hr = audioClient_->GetCurrentPadding(&padding);
    if (FAILED(hr)) return std::unexpected(L"GetCurrentPadding failed");

    UINT32 framesAvailable = bufferFrameCount_ - padding;
    UINT32 framesToWrite = size / (channels_ * 2); // 16-bit samples
    framesToWrite = std::min(framesToWrite, framesAvailable);

    if (framesToWrite == 0) return {}; // Buffer full, skip

    BYTE* buffer = nullptr;
    hr = renderClient_->GetBuffer(framesToWrite, &buffer);
    if (FAILED(hr)) return std::unexpected(L"GetBuffer failed");

    std::memcpy(buffer, data, framesToWrite * channels_ * 2);
    renderClient_->ReleaseBuffer(framesToWrite, 0);
    return {};
}

// ---- AudioDecoder ----

AudioDecoder::~AudioDecoder() { stop(); }

Result<void> AudioDecoder::init(AVFormatContext* fmtCtx, int audioStreamIdx) {
    fmtCtx_ = fmtCtx;
    audioStreamIdx_ = audioStreamIdx;

    AVCodecParameters* par = fmtCtx_->streams[audioStreamIdx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return std::unexpected(std::wstring(L"audio decoder not found"));

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) return std::unexpected(std::wstring(L"avcodec_alloc_context3 failed"));

    avcodec_parameters_to_context(codecCtx_, par);

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx_);
        return std::unexpected(std::wstring(L"audio decoder open failed"));
    }

    // Setup swresample for output format conversion
    outSampleRate_ = 48000; // Target output rate
    outChannels_ = 2;       // Target stereo

    AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inChLayout = codecCtx_->ch_layout;

    int ret = swr_alloc_set_opts2(
        &swr_,
        &outChLayout, AV_SAMPLE_FMT_S16, outSampleRate_,
        &inChLayout, codecCtx_->sample_fmt, codecCtx_->sample_rate,
        0, nullptr
    );

    if (ret < 0 || !swr_) {
        avcodec_free_context(&codecCtx_);
        return std::unexpected(std::wstring(L"swr_alloc_set_opts2 failed"));
    }

    ret = swr_init(swr_);
    if (ret < 0) {
        swr_free(&swr_);
        avcodec_free_context(&codecCtx_);
        return std::unexpected(std::wstring(L"swr_init failed"));
    }

    auto& log = log::Logger::instance();
    auto codecNameWide = vw::util::utf8ToWide(avcodec_get_name(par->codec_id));
    log.info(L"audio decoder: {} {} Hz, {} ch \u2192 {} Hz, {} ch (S16)",
             codecNameWide, codecCtx_->sample_rate, codecCtx_->ch_layout.nb_channels,
             outSampleRate_, outChannels_);
    return {};
}

Result<void> AudioDecoder::start() {
    if (decodeThread_.joinable()) return {};

    stopRequested_ = false;
    decodeThread_ = std::thread([this] {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) return;

        while (!stopRequested_) {
            int ret = av_read_frame(fmtCtx_, pkt);
            if (ret < 0) break;

            if (pkt->stream_index != audioStreamIdx_) {
                av_packet_unref(pkt);
                continue;
            }

            ret = avcodec_send_packet(codecCtx_, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;

            while (ret >= 0) {
                AVFrame* frame = av_frame_alloc();
                ret = avcodec_receive_frame(codecCtx_, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { av_frame_free(&frame); break; }
                if (ret < 0) { av_frame_free(&frame); break; }

                // Queue frame (blocking if full)
                {
                    std::unique_lock lock(queueMutex_);
                    queueCond_.wait(lock, [this] { return frameQueue_.size() < kMaxQueueSize || stopRequested_; });
                    if (stopRequested_) { av_frame_free(&frame); break; }
                    frameQueue_.push_back(frame);
                }
                queueCond_.notify_one();
            }
        }
        av_packet_free(&pkt);
    });
    return {};
}

void AudioDecoder::stop() {
    stopRequested_ = true;
    queueCond_.notify_all();
    queueNotFull_.notify_all();
    if (decodeThread_.joinable()) decodeThread_.join();

    // Free queued frames
    std::lock_guard lock(queueMutex_);
    for (auto* f : frameQueue_) av_frame_free(&f);
    frameQueue_.clear();
}

AVFrame* AudioDecoder::getFrame() {
    std::unique_lock lock(queueMutex_);
    queueCond_.wait(lock, [this] { return !frameQueue_.empty() || stopRequested_; });
    if (frameQueue_.empty()) return nullptr;

    AVFrame* frame = frameQueue_.front();
    frameQueue_.erase(frameQueue_.begin());
    queueNotFull_.notify_one();
    return frame;
}

int AudioDecoder::convertFrame(AVFrame* inFrame, uint8_t** outBuffer, int outBufferSize) {
    if (!swr_ || !inFrame) return -1;

    int outSamples = av_rescale_rnd(
        swr_get_delay(swr_, inFrame->sample_rate) + inFrame->nb_samples,
        outSampleRate_, inFrame->sample_rate, AV_ROUND_UP
    );

    int bytesPerSample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    int neededSize = outSamples * outChannels_ * bytesPerSample;
    if (outBufferSize < neededSize) return -1;

    return swr_convert(swr_, outBuffer, outSamples,
                       (const uint8_t**)inFrame->data, inFrame->nb_samples);
}

// ---- AudioPipeline ----

AudioPipeline::~AudioPipeline() { stop(); }

Result<void> AudioPipeline::init(const std::wstring& path) {
    auto& log = log::Logger::instance();

    // Open file with FFmpeg
    std::string pathUtf8 = vw::util::wideToUtf8(path);
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.c_str(), nullptr, nullptr) < 0)
        return std::unexpected(L"cannot open file for audio");

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return std::unexpected(L"cannot find stream info");
    }

    // Find audio stream
    int audioIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        log.info(L"audio: no audio stream found");
        return {}; // No audio is not an error
    }

    fmtCtx_ = fmtCtx;
    audioStreamIdx_ = audioIdx;
    hasAudio_ = true;

    // Initialize decoder
    decoder_ = std::make_unique<AudioDecoder>();
    auto result = decoder_->init(fmtCtx_, audioStreamIdx_);
    if (!result) {
        avformat_close_input(&fmtCtx_);
        fmtCtx_ = nullptr;
        hasAudio_ = false;
        return std::unexpected(L"audio decoder init failed: " + result.error());
    }

    // Initialize output
    output_ = std::make_unique<AudioOutput>();
    result = output_->init(AudioOutput::ShareMode::Shared, decoder_->sampleRate(), decoder_->channels());
    if (!result) {
        avformat_close_input(&fmtCtx_);
        fmtCtx_ = nullptr;
        hasAudio_ = false;
        return std::unexpected(L"audio output init failed: " + result.error());
    }

    // Initialize clock
    clock_ = std::make_unique<AudioClock>();
    clock_->setSampleRate(output_->sampleRate());
    clock_->setPaddingGetter([this]() -> UINT32 {
        UINT32 padding = 0;
        if (output_ && output_->audioClient())
            output_->audioClient()->GetCurrentPadding(&padding);
        return padding;
    });

    log.info(L"audio pipeline initialized: {} Hz, {} ch", decoder_->sampleRate(), decoder_->channels());
    return {};
}

Result<void> AudioPipeline::start() {
    if (!hasAudio_ || !decoder_ || !output_) return {};
    if (isPaused_) { resume(); return {}; }

    auto result = decoder_->start();
    if (!result) return result;

    result = output_->start();
    if (!result) return result;

    stopRequested_ = false;
    audioThread_ = std::thread(&AudioPipeline::audioThreadFunc, this);
    return {};
}

void AudioPipeline::stop() {
    stopRequested_ = true;
    if (audioThread_.joinable()) audioThread_.join();
    if (decoder_) decoder_->stop();
    if (output_) output_->stop();
}

void AudioPipeline::pause() {
    if (!hasAudio_ || isPaused_) return;
    stopRequested_ = true;
    if (audioThread_.joinable()) audioThread_.join();
    if (output_) output_->stop();
    isPaused_ = true;
}

void AudioPipeline::resume() {
    if (!hasAudio_ || !isPaused_) return;
    isPaused_ = false;
    stopRequested_ = false;
    if (output_) output_->start();
    audioThread_ = std::thread(&AudioPipeline::audioThreadFunc, this);
}

void AudioPipeline::audioThreadFunc() {
    auto& log = log::Logger::instance();

    while (!stopRequested_) {
        AVFrame* frame = decoder_->getFrame();
        if (!frame) break;

        // Convert to output format
        uint8_t* outBuffer = nullptr;
        int outSamples = decoder_->convertFrame(frame, &outBuffer, frame->nb_samples * decoder_->channels() * 4);

        if (outSamples > 0 && outBuffer) {
            // Write to WASAPI
            uint32_t dataSize = outSamples * decoder_->channels() * 2; // S16 = 2 bytes/sample
            output_->write(outBuffer, dataSize);

            // Update clock (base position advances with samples written)
            LONGLONG samplesWritten = outSamples;
            LONGLONG advance = samplesWritten * 10000000 / output_->sampleRate();
            clock_->setBasePosition(clock_->getPosition() + advance);
        }

        av_frame_free(&frame);
    }
}

} // namespace vw::audio

#pragma warning(pop)
