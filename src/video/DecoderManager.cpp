#include "video/DecoderManager.h"

#include <cstdio>
#include <cstring>
#include <format>

#include <mfapi.h>
#include <mfidl.h>

#include "logging/Logger.h"

using Microsoft::WRL::ComPtr;

namespace vw::video {

namespace {

// MF_SOURCE_READER_* are signed enum constants in the SDK headers; DWORD
// parameters need an explicit cast (C4245-safe). MF_SOURCE_READER_MEDIASOURCE
// is a stream-index sentinel for GetServiceForStream, not a service GUID.
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSourceStream = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

// Source Reader -> RGB32: conversion through the Video Processor MFT (the
// documented path) + an explicit stride, exactly as proven by the M5-preview
// harness (MF_E_INVALIDMEDIATYPE without these).
Result<void> negotiateRgb32Output(IMFSourceReader* reader) {
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideoStream, &native))) {
        return std::unexpected(L"no video stream in file");
    }
    UINT32 w = 0, h = 0;
    if (FAILED(::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
        return std::unexpected(L"could not read frame size");
    }

    ComPtr<IMFMediaType> rgb;
    HRESULT hr = ::MFCreateMediaType(&rgb);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = rgb->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = rgb->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = rgb->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) {
        LONG stride = 0;
        if (SUCCEEDED(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride)) &&
            stride > 0) {
            hr = rgb->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
        }
    }
    if (FAILED(hr)) {
        return std::unexpected(L"could not build RGB32 media type");
    }
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, rgb.Get());
    if (FAILED(hr)) {
        return std::unexpected(L"RGB32 output unsupported for this file (codec/color "
                               L"converter): hr=0x" +
                               std::format(L"{:08X}", static_cast<unsigned>(hr)));
    }
    return {};
}

} // namespace

DecoderManager::~DecoderManager() {
    close();
}

std::wstring DecoderManager::formatHr(HRESULT hr) {
    return std::format(L"0x{:08X}", static_cast<unsigned>(hr));
}

Result<void> DecoderManager::open(const std::wstring& path) {
    close();

    // Video-Processor-based conversion is the documented YUV->RGB32 path
    // (the decoder's own converter rejects RGB32 with MF_E_INVALIDMEDIATYPE).
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr)) {
        hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) {
        hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader);
    }
    if (FAILED(hr)) {
        return std::unexpected(L"MFCreateSourceReaderFromURL failed: " + formatHr(hr));
    }

    // Audio is out of scope for v1: deselect every stream, then select only
    // the first video stream. No audio media type is ever set -> the audio
    // pipeline is never initialized.
    if (FAILED(reader->SetStreamSelection(kAllStreams, FALSE))) {
        return std::unexpected(L"could not deselect streams");
    }
    if (FAILED(reader->SetStreamSelection(kFirstVideoStream, TRUE))) {
        return std::unexpected(L"no video stream in file");
    }

    // Metadata from the real media type / presentation descriptor.
    VideoMetadata meta;
    meta.path = path;
    ComPtr<IMFMediaType> native;
    hr = reader->GetCurrentMediaType(kFirstVideoStream, &native);
    if (FAILED(hr)) {
        return std::unexpected(L"no video stream in file");
    }
    auto fill = VideoMetadata::fillFromMediaType(native.Get(), meta);
    if (!fill) {
        return fill;
    }

    // Output type: RGB32 through the Video Processor MFT (documented
    // YUV->RGB32 path; the decoder's own converter rejects RGB32). Without
    // this the reader yields COMPRESSED samples at the native type and
    // copySampleToFrame would read past the buffer.
    auto negotiated = negotiateRgb32Output(reader.Get());
    if (!negotiated) {
        return negotiated;
    }

    ComPtr<IMFMediaSource> source;
    hr = reader->GetServiceForStream(kMediaSourceStream, GUID_NULL, IID_PPV_ARGS(&source));
    if (SUCCEEDED(hr)) {
        ComPtr<IMFPresentationDescriptor> pd;
        if (SUCCEEDED(source->CreatePresentationDescriptor(&pd))) {
            pd->GetUINT64(MF_PD_DURATION, reinterpret_cast<UINT64*>(&meta.duration100ns));
            DWORD streamCount = 0;
            if (SUCCEEDED(pd->GetStreamDescriptorCount(&streamCount))) {
                for (DWORD i = 0; i < streamCount; ++i) {
                    ComPtr<IMFStreamDescriptor> sd;
                    BOOL selected = FALSE;
                    if (SUCCEEDED(pd->GetStreamDescriptorByIndex(i, &selected, &sd))) {
                        // Major type via the media-type handler (the
                        // MF_SD_STREAM_MAJOR_TYPE constant is absent from the
                        // 26100 SDK headers).
                        ComPtr<IMFMediaTypeHandler> handler;
                        GUID major{};
                        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)) &&
                            SUCCEEDED(handler->GetMajorType(&major)) &&
                            major == MFMediaType_Audio) {
                            meta.hasAudio = true;
                        }
                    }
                }
            }
        }
    }

    // Swap in only after everything succeeded (failed open leaves no state).
    reader_ = std::move(reader);
    metadata_ = std::move(meta);
    opened_ = true;
    return {};
}

Result<void> DecoderManager::start(FrameQueue* queue, LONGLONG position100ns) {
    if (!opened_ || !reader_) {
        return std::unexpected(L"DecoderManager::start: not opened");
    }
    if (worker_.joinable()) {
        return std::unexpected(L"DecoderManager::start: already running");
    }
    if (!queue) {
        return std::unexpected(L"DecoderManager::start: null queue");
    }
    queue_ = queue;

    if (position100ns > 0) {
        PROPVARIANT var{};
        var.vt = VT_I8;
        var.hVal.QuadPart = position100ns;
        HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
        if (FAILED(hr)) {
            log::Logger::instance().warn(L"seek to {} failed: {}", position100ns, formatHr(hr));
        }
    }

    stopRequested_.store(false);
    // The worker uses a LOCAL copy of the queue pointer: stop() closes the
    // queue then joins BEFORE nulling queue_, so the worker can never see a
    // nulled member (race that SIGSEGV'd in Release builds).
    worker_ = std::thread([this, queue] { workerLoop(queue); });
    return {};
}

void DecoderManager::stop() {
    stopRequested_.store(true);
    // Unblock a worker stuck pushing into a full queue, THEN join, THEN drop
    // the pointer — the worker holds its own copy and may still push during
    // the join window.
    if (queue_) {
        queue_->close();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    queue_ = nullptr;
    // reader_ + opened_ are kept: a paused session resumes via start().
}

void DecoderManager::close() {
    stop();
    reader_.Reset();
    opened_ = false;
}

void DecoderManager::workerLoop(FrameQueue* queue) {
    const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto& log = log::Logger::instance();
    log.debug(L"decode worker started ({}x{} @ {:.2f} fps)", metadata_.width, metadata_.height,
              metadata_.fps);

    while (!stopRequested_.load()) {
        DWORD streamIndex = 0, flags = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample(kFirstVideoStream, 0, &streamIndex, &flags, nullptr,
                                         &sample);
        if (FAILED(hr)) {
            log.warn(L"decode ReadSample failed: {}", formatHr(hr));
            DecodedFrame end;
            end.endOfStream = true;
            queue->push(std::move(end));
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            log.debug(L"decode end of stream");
            DecodedFrame end;
            end.endOfStream = true;
            queue->push(std::move(end));
            break;
        }
        if (!sample) {
            continue; // no sample this call — keep reading
        }

        // Current output type (RGB32) carries the frame size for copying.
        ComPtr<IMFMediaType> current;
        if (FAILED(reader_->GetCurrentMediaType(kFirstVideoStream, &current))) {
            log.warn(L"decode: could not read current media type");
            continue;
        }
        UINT w = 0, h = 0;
        ::MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w == 0 || h == 0) {
            log.warn(L"decode: current media type has no frame size");
            continue;
        }

        DecodedFrame frame;
        std::wstring copyErr;
        if (!copySampleToFrame(sample.Get(), w, h, frame, copyErr)) {
            log.warn(L"frame copy failed: {}", copyErr);
            continue;
        }
        if (!queue->push(std::move(frame))) {
            break; // queue closed (stop requested)
        }
    }

    if (SUCCEEDED(comHr)) {
        ::CoUninitialize();
    }
    log.debug(L"decode worker exited");
}

bool DecoderManager::copySampleToFrame(IMFSample* sample, UINT width, UINT height,
                                       DecodedFrame& out, std::wstring& err) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        err = L"GetBufferByIndex failed: " + formatHr(hr);
        return false;
    }
    sample->GetSampleTime(&out.timestamp);

    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        hr = buffer2d->Lock2D(&scanline0, &pitch);
        if (FAILED(hr)) {
            err = L"Lock2D failed: " + formatHr(hr);
            return false;
        }
    } else {
        DWORD len = 0;
        hr = buffer->Lock(&scanline0, nullptr, &len);
        if (FAILED(hr)) {
            err = L"media buffer Lock failed: " + formatHr(hr);
            return false;
        }
        pitch = 0; // tight packing assumed
    }

    out.width = width;
    out.height = height;
    const size_t srcPitch =
        (pitch > 0 ? static_cast<size_t>(pitch) : static_cast<size_t>(width) * 4);
    out.bytes.resize(static_cast<size_t>(height) * static_cast<size_t>(width) * 4);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(out.bytes.data() + static_cast<size_t>(y) * width * 4,
                    scanline0 + static_cast<size_t>(y) * srcPitch, static_cast<size_t>(width) * 4);
    }

    if (buffer2d) {
        buffer2d->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return true;
}

} // namespace vw::video
