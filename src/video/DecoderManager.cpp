#include "video/DecoderManager.h"

#include <cstdio>
#include <cstring>
#include <format>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
// MR_VIDEO_ACCELERATION_SERVICE is DEFINE_GUID'd in evr.h but not exported by
// any SDK import lib — initguid.h materializes the definition in this TU.
#include <initguid.h>
#include <evr.h>

#include "logging/Logger.h"
#include "util/clock.h"

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

// Source Reader -> NV12 GPU surfaces (docs/03 §3.7): the DXGI device manager
// was set on the reader attributes, so this output type routes the decode
// through the hardware MFT. Same stride discipline as RGB32.
Result<void> negotiateNv12Output(IMFSourceReader* reader) {
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideoStream, &native))) {
        return std::unexpected(L"no video stream in file");
    }
    UINT32 w = 0, h = 0;
    if (FAILED(::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
        return std::unexpected(L"could not read frame size");
    }

    ComPtr<IMFMediaType> nv12;
    HRESULT hr = ::MFCreateMediaType(&nv12);
    if (SUCCEEDED(hr)) hr = nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) {
        LONG stride = 0;
        if (SUCCEEDED(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_NV12.Data1, w, &stride)) &&
            stride > 0) {
            hr = nv12->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
        }
    }
    if (FAILED(hr)) {
        return std::unexpected(L"could not build NV12 media type");
    }
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, nv12.Get());
    if (FAILED(hr)) {
        return std::unexpected(L"NV12 output unsupported: hr=0x" +
                               std::format(L"{:08X}", static_cast<unsigned>(hr)));
    }
    return {};
}

// Registry friendly name for an MFT CLSID (docs/03 §3.7 — never fabricate
// decoder/vendor names; read what Windows actually registered). Falls back to
// the MFT DLL filename, then "unknown MFT".
std::wstring clsidFriendlyName(const GUID& clsid) {
    wchar_t key[64]{};
    ::swprintf(key, 64,
               L"CLSID\\{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               static_cast<unsigned>(clsid.Data1), clsid.Data2, clsid.Data3, clsid.Data4[0],
               clsid.Data4[1], clsid.Data4[2], clsid.Data4[3], clsid.Data4[4], clsid.Data4[5],
               clsid.Data4[6], clsid.Data4[7]);
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return L"unknown MFT";
    }
    wchar_t buf[256]{};
    DWORD size = static_cast<DWORD>(sizeof(buf));
    std::wstring name;
    if (::RegQueryValueExW(hk, L"FriendlyName", nullptr, nullptr, reinterpret_cast<LPBYTE>(buf),
                           &size) == ERROR_SUCCESS) {
        name = buf;
    } else {
        // Fall back to the DLL name (unambiguous evidence of the vendor).
        HKEY hkDll = nullptr;
        if (::RegOpenKeyExW(hk, L"InprocServer32", 0, KEY_READ, &hkDll) == ERROR_SUCCESS) {
            size = static_cast<DWORD>(sizeof(buf));
            if (::RegQueryValueExW(hkDll, nullptr, nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
                const std::wstring dll = buf;
                const size_t slash = dll.find_last_of(L"\\");
                name = slash == std::wstring::npos ? dll : dll.substr(slash + 1);
            }
            ::RegCloseKey(hkDll);
        }
        if (name.empty()) {
            name = L"unknown MFT";
        }
    }
    ::RegCloseKey(hk);
    return name;
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
    hardware_ = false;
    decoderName_.clear();

    // Hardware first when a D3D device is available (docs/03 §3.7). The
    // hardware path PROBES the first sample: on machines where NV12
    // negotiates but the decoder hands back system-memory samples (no
    // hardware MFT active), it fails cleanly and open() retries through the
    // M4 software path — never a crash, never silent frame drops.
    if (d3dDevice_) {
        auto result = openHardware(path);
        if (result) {
            return {};
        }
        log::Logger::instance().warn(L"hardware decode unavailable ({}); retrying "
                                     L"with the software RGB32 path",
                                     result.error());
        close(); // release the NV12-committed reader entirely
        hardware_ = false;
        decoderName_.clear();
    }
    return openSoftware(path);
}

Result<void> DecoderManager::openHardware(const std::wstring& path) {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) {
        return std::unexpected(L"MFCreateAttributes failed: " + formatHr(hr));
    }

    // NOTE: MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING must NOT be combined
    // with the D3D manager attributes — the reader rejects the combo with
    // E_INVALIDARG (probed on this machine). The VP flag is only for the
    // software RGB32 path.
    UINT resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    if (FAILED(::MFCreateDXGIDeviceManager(&resetToken, &dxgiManager))) {
        return std::unexpected(L"MFCreateDXGIDeviceManager failed");
    }
    if (FAILED(dxgiManager->ResetDevice(d3dDevice_, resetToken))) {
        return std::unexpected(L"DXGI manager ResetDevice failed");
    }
    if (FAILED(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiManager.Get())) ||
        FAILED(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE))) {
        return std::unexpected(L"could not set hardware reader attributes");
    }

    VideoMetadata meta;
    ComPtr<IMFSourceReader> reader;
    auto prep = prepareReader(path, attrs.Get(), reader, meta);
    if (!prep) {
        return prep;
    }

    auto nv12 = negotiateNv12Output(reader.Get());
    if (!nv12) {
        return nv12;
    }

    // Runtime probe (the whole point of the split): NV12 negotiation
    // succeeding does NOT prove GPU surfaces. Read one sample — the active
    // decoder must hand back a DXGI buffer, else this machine's MF stack
    // has no working hardware path and the caller retries in software.
    DWORD streamIndex = 0, flags = 0;
    ComPtr<IMFSample> sample;
    hr = reader->ReadSample(kFirstVideoStream, 0, &streamIndex, &flags, nullptr, &sample);
    if (FAILED(hr) || !sample) {
        return std::unexpected(L"hardware probe: no first sample (" + formatHr(hr) + L")");
    }
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) {
        return std::unexpected(L"hardware probe: no media buffer");
    }
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    hr = ::MFGetService(buffer.Get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&dxgiBuffer));
    if (FAILED(hr)) {
        return std::unexpected(L"hardware probe: decoder produced system-memory samples "
                               L"(no hardware MFT active)");
    }

    // The probe consumed the first frame — rewind so the worker starts at it.
    PROPVARIANT zero{};
    zero.vt = VT_I8;
    zero.hVal.QuadPart = 0;
    if (FAILED(reader->SetCurrentPosition(GUID_NULL, zero))) {
        log::Logger::instance().warn(L"hardware probe: could not rewind to first frame");
    }

    hardware_ = true;
    dxgiManager_ = std::move(dxgiManager);
    detectDecoder(reader.Get());
    log::Logger::instance().info(L"hardware decode active: NV12 GPU surfaces");
    reader_ = std::move(reader);
    metadata_ = std::move(meta);
    opened_ = true;
    return {};
}

Result<void> DecoderManager::openSoftware(const std::wstring& path) {
    // Software path: RGB32 through the Video Processor MFT (documented
    // YUV->RGB32 path; the decoder's own converter rejects RGB32). Without
    // this the reader yields COMPRESSED samples at the native type and
    // copySampleToFrame would read past the buffer.
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) {
        return std::unexpected(L"MFCreateAttributes failed: " + formatHr(hr));
    }
    if (FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))) {
        return std::unexpected(L"could not set video-processing attribute");
    }

    VideoMetadata meta;
    ComPtr<IMFSourceReader> reader;
    auto prep = prepareReader(path, attrs.Get(), reader, meta);
    if (!prep) {
        return prep;
    }

    auto negotiated = negotiateRgb32Output(reader.Get());
    if (!negotiated) {
        return negotiated;
    }

    decoderName_ = L"software (RGB32 output)";
    log::Logger::instance().info(L"decoder: software (RGB32 output)");
    reader_ = std::move(reader);
    metadata_ = std::move(meta);
    opened_ = true;
    return {};
}

// Shared reader setup: create the Source Reader with the given attributes,
// deselect all streams (audio is out of scope for v1 — its pipeline is never
// initialized), select only the first video stream, and read metadata from
// the native media type + presentation descriptor (never the file extension).
Result<void> DecoderManager::prepareReader(const std::wstring& path, IMFAttributes* attrs,
                                           ComPtr<IMFSourceReader>& reader,
                                           VideoMetadata& meta) {
    HRESULT hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs, &reader);
    if (FAILED(hr)) {
        return std::unexpected(L"MFCreateSourceReaderFromURL failed: " + formatHr(hr));
    }
    if (FAILED(reader->SetStreamSelection(kAllStreams, FALSE))) {
        return std::unexpected(L"could not deselect streams");
    }
    if (FAILED(reader->SetStreamSelection(kFirstVideoStream, TRUE))) {
        return std::unexpected(L"no video stream in file");
    }

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

    // Always reposition the reader: initial start (0), resume (saved
    // position), and M7 loop replay (0, after the reader was left at EOS by a
    // completed stream). Seeking a fresh reader to 0 is harmless; skipping the
    // seek would make a replay immediately hit EOS again.
    {
        PROPVARIANT var{};
        var.vt = VT_I8;
        var.hVal.QuadPart = position100ns;
        HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
        if (FAILED(hr)) {
            log::Logger::instance().warn(L"seek to {} failed: {}", position100ns, formatHr(hr));
        }
    }

    stopRequested_.store(false);
    decodedFrames_.store(0); // M6 stats: counts since this start
    log::Logger::instance().debug(L"starting decode worker (hardware={})", hardware_);
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

void DecoderManager::detectDecoder(IMFSourceReader* reader) {
    // The active MFT (docs/03 §3.7): read its CLSID + hardware marker, then
    // resolve the registered friendly name — never fabricate the vendor.
    // GetServiceForStream(MR_VIDEO_ACCELERATION_SERVICE) hard-crashes inside
    // mfreadwrite on this SDK — use the documented IMFGetService route.
    // NOTE: takes the reader (the member is still null during open()).
    ComPtr<IMFGetService> svc;
    if (!reader || FAILED(reinterpret_cast<IUnknown*>(reader)->QueryInterface(
                         IID_PPV_ARGS(&svc)))) {
        return;
    }
    ComPtr<IMFTransform> transform;
    if (FAILED(svc->GetService(MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&transform)))) {
        return;
    }
    ComPtr<IMFAttributes> attrs;
    if (FAILED(transform->GetAttributes(&attrs))) {
        return;
    }
    wchar_t hwUrl[128]{};
    UINT32 len = 0;
    const bool isHw =
        SUCCEEDED(attrs->GetString(MFT_ENUM_HARDWARE_URL_Attribute, hwUrl, 128, &len)) && len > 0;
    GUID clsid{};
    // MFT_TRANSFORM_CLSID_Attribute is this SDK's name for the documented
    // MF_TRANSFORM_ATTRIBUTE_MFT_TRANSFORM_CLSID.
    attrs->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
    decoderName_ = clsidFriendlyName(clsid);
    log::Logger::instance().info(L"decoder: {} ({})", decoderName_, isHw ? L"hardware" : L"software");
}

void DecoderManager::workerLoop(FrameQueue* queue) {
    const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto& log = log::Logger::instance();
    log.debug(L"decode worker started ({}x{} @ {:.2f} fps, {})", metadata_.width, metadata_.height,
              metadata_.fps, hardware_ ? L"hardware" : L"software");

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

        // M13: reuse a recycled frame buffer when one is available (the 14 MB
        // RGB32 block costs VirtualAlloc + demand-zero page faults if
        // reallocated fresh every frame — measured 4.5 ms/f of zeroing vs
        // ~0 with reuse). The consumer returns buffers after the GPU upload.
        DecodedFrame frame;
        queue->takeSpareBuffer(frame.bytes);
        std::wstring copyErr;
        if (hardware_) {
            if (!copySampleToTexture(sample.Get(), frame, copyErr)) {
                log.warn(L"frame surface copy failed: {}", copyErr);
                continue;
            }
        } else {
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
            if (!copySampleToFrame(sample.Get(), w, h, frame, copyErr)) {
                log.warn(L"frame copy failed: {}", copyErr);
                continue;
            }
        }
        // Anamorphic correction: the scaling math consumes the SAR-corrected
        // display aspect, not the raw pixel dims (docs/02 §2.4). Constant per
        // stream, so it is copied from the metadata on every frame.
        if (metadata_.displayAspect > 0.0) {
            frame.displayAspect = static_cast<float>(metadata_.displayAspect);
        }
        frame.decodeTime100ns = util::Clock::instance().now100ns(); // M6 latency stats
        if (!queue->push(std::move(frame))) {
            break; // queue closed (stop requested)
        }
        decodedFrames_.fetch_add(1);
    }

    if (SUCCEEDED(comHr)) {
        ::CoUninitialize();
    }
    log.debug(L"decode worker exited");
}

bool DecoderManager::copySampleToTexture(IMFSample* sample, DecodedFrame& out,
                                         std::wstring& err) {
    // GPU surface extraction (docs/03 §3.7): the media buffer is a DXGI
    // buffer; the texture is the decoder's NV12 surface — no CPU copy.
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) {
        err = L"GetBufferByIndex failed: " + formatHr(hr);
        return false;
    }
    sample->GetSampleTime(&out.timestamp);

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    hr = ::MFGetService(buffer.Get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&dxgiBuffer));
    if (FAILED(hr)) {
        err = L"not a DXGI buffer (no hardware surface): " + formatHr(hr);
        return false;
    }
    ComPtr<ID3D11Texture2D> texture;
    hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) {
        err = L"GetResource failed: " + formatHr(hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    out.texture = std::move(texture);
    out.width = desc.Width;
    out.height = desc.Height;
    out.hardware = true;
    return true;
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
    // M13: with a recycled buffer this resize is a no-op (size already set);
    // with a fresh buffer it zero-inits once (the cost we removed).
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

Result<VideoMetadata> DecoderManager::probeMetadata(const std::wstring& path) {
    // Metadata-only probe (M11 library panel): no attributes (no hardware
    // path, no video processing), select the first video stream, read the
    // native media type + duration, release. The reader's destructor tears
    // down the media source.
    ComPtr<IMFSourceReader> reader;
    VideoMetadata meta;
    auto prepared = prepareReader(path, nullptr, reader, meta);
    if (!prepared) {
        return std::unexpected(prepared.error());
    }
    return meta;
}

} // namespace vw::video
