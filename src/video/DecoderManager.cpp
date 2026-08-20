#include "video/DecoderManager.h"

#include <cwchar>
#include <cstring>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <initguid.h>
#include <evr.h>

#include "logging/Logger.h"
#include "util/HrToString.h"
#include "util/clock.h"

using Microsoft::WRL::ComPtr;

namespace vw::video {

namespace {

constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kMediaSourceStream = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

Result<void> negotiateNv12Output(IMFSourceReader* reader) {
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideoStream, &native)))
        return std::unexpected(L"no video stream in file");

    UINT32 w = 0, h = 0;
    if (FAILED(::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
        return std::unexpected(L"could not read frame size");

    ComPtr<IMFMediaType> nv12;
    HRESULT hr = ::MFCreateMediaType(&nv12);
    if (SUCCEEDED(hr)) hr = nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = ::MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, w, h);
    if (FAILED(hr)) return std::unexpected(L"could not build NV12 media type");

    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, nv12.Get());
    if (FAILED(hr))
        return std::unexpected(L"NV12 output unsupported: hr=0x" + std::format(L"{:08X}", static_cast<unsigned>(hr)));
    return {};
}

Result<void> negotiateRgb32Output(IMFSourceReader* reader) {
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetCurrentMediaType(kFirstVideoStream, &native)))
        return std::unexpected(L"no video stream in file");

    UINT32 w = 0, h = 0;
    if (FAILED(::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
        return std::unexpected(L"could not read frame size");

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
        if (SUCCEEDED(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride)) && stride > 0)
            hr = rgb->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(stride));
    }
    if (FAILED(hr)) return std::unexpected(L"could not build RGB32 media type");

    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, rgb.Get());
    if (FAILED(hr))
        return std::unexpected(L"RGB32 output unsupported: hr=0x" + std::format(L"{:08X}", static_cast<unsigned>(hr)));
    return {};
}

std::wstring clsidFriendlyName(const GUID& clsid) {
    wchar_t key[64]{};
    ::swprintf(key, 64, L"CLSID\\{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               static_cast<unsigned>(clsid.Data1), clsid.Data2, clsid.Data3,
               clsid.Data4[0], clsid.Data4[1], clsid.Data4[2], clsid.Data4[3],
               clsid.Data4[4], clsid.Data4[5], clsid.Data4[6], clsid.Data4[7]);
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return L"unknown MFT";
    wchar_t buf[256]{};
    DWORD size = sizeof(buf);
    std::wstring name;
    if (::RegQueryValueExW(hk, L"FriendlyName", nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS)
        name = buf;
    else {
        HKEY hkDll = nullptr;
        if (::RegOpenKeyExW(hk, L"InprocServer32", 0, KEY_READ, &hkDll) == ERROR_SUCCESS) {
            size = sizeof(buf);
            if (::RegQueryValueExW(hkDll, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
                std::wstring dll = buf;
                auto slash = dll.find_last_of(L'\\');
                name = slash == std::wstring::npos ? dll : dll.substr(slash + 1);
            }
            ::RegCloseKey(hkDll);
        }
        if (name.empty()) name = L"unknown MFT";
    }
    ::RegCloseKey(hk);
    return name;
}

} // namespace

DecoderManager::~DecoderManager() { close(); }

using vw::util::formatHr;

Result<void> DecoderManager::open(const std::wstring& path) {
    close();
    hardware_ = false;
    decoderName_.clear();

    if (d3dDevice_) {
        auto result = openHardware(path);
        if (result) return {};
        log::Logger::instance().warn(L"hardware decode unavailable ({}); retrying software", result.error());
        close();
    }
    return openSoftware(path);
}

Result<void> DecoderManager::openHardware(const std::wstring& path) {
    auto& log = log::Logger::instance();

    // Determine the render device's LUID so we can identify it among adapters.
    LUID renderLuid{};
    if (d3dDevice_) {
        ComPtr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
            ComPtr<IDXGIAdapter> adap;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adap))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adap->GetDesc(&desc))) renderLuid = desc.AdapterLuid;
            }
        }
    }

    // Enumerate adapters. Try the render adapter FIRST — if its MFT
    // produces GPU-resident DXGI surfaces, the decoded texture is already
    // on the render device (true zero-copy). Fall back to NVIDIA/other
    // adapters which may also produce GPU surfaces but require a GPU-to-GPU
    // shared-handle copy to reach the render device.
    std::vector<ComPtr<ID3D11Device>> nvidiaCandidates;
    std::vector<ComPtr<ID3D11Device>> otherCandidates;
    constexpr UINT kNvidiaVendorId = 0x10DE;

    auto adapters = gfx::D3D11DeviceManager::enumerateAdapters();
    if (adapters) {
        for (UINT i = 0; i < adapters->size(); ++i) {
            const auto& info = (*adapters)[i];

            bool isRender = (info.luid.LowPart == renderLuid.LowPart &&
                             info.luid.HighPart == renderLuid.HighPart);

            auto adapter = gfx::D3D11DeviceManager::getAdapter(i);
            if (!adapter) continue;

            ComPtr<ID3D11Device> dev;
            D3D_FEATURE_LEVEL fl{};
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            HRESULT hr = ::D3D11CreateDevice(adapter->Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                levels, 2, D3D11_SDK_VERSION, &dev, &fl, nullptr);
            if (FAILED(hr)) continue;

            if (isRender) {
                // Render adapter: try first for true zero-copy.
                nvidiaCandidates.insert(nvidiaCandidates.begin(), std::move(dev));
            } else if (info.vendor == kNvidiaVendorId) {
                log.info(L"MF hardware: NVIDIA adapter found ({})", info.description);
                nvidiaCandidates.push_back(std::move(dev));
            } else {
                otherCandidates.push_back(std::move(dev));
            }
        }
    }

    // Build priority order: render adapter first (zero-copy), NVIDIA, then others.
    std::vector<ComPtr<ID3D11Device>> candidates;
    candidates.reserve(nvidiaCandidates.size() + otherCandidates.size());
    for (auto& dev : nvidiaCandidates) candidates.push_back(std::move(dev));
    for (auto& dev : otherCandidates) candidates.push_back(std::move(dev));

    for (auto& dev : candidates) {
        auto result = tryHardwareWithDevice(path, dev.Get());
        if (result) { decodeDevice_ = dev; return {}; }
    }
    return std::unexpected(std::wstring(L"no adapter produced GPU surfaces"));
}

Result<void> DecoderManager::tryHardwareWithDevice(const std::wstring& path, ID3D11Device* device) {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) return std::unexpected(L"MFCreateAttributes failed");

    UINT resetToken = 0;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    if (FAILED(::MFCreateDXGIDeviceManager(&resetToken, &dxgiManager)))
        return std::unexpected(L"MFCreateDXGIDeviceManager failed");
    if (FAILED(dxgiManager->ResetDevice(device, resetToken)))
        return std::unexpected(L"DXGI manager ResetDevice failed");
    if (FAILED(attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgiManager.Get())) ||
        FAILED(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE)))
        return std::unexpected(L"could not set hardware reader attributes");

    VideoMetadata meta;
    ComPtr<IMFSourceReader> reader;
    auto prep = prepareReader(path, attrs.Get(), reader, meta);
    if (!prep) return prep;

    auto nv12 = negotiateNv12Output(reader.Get());
    if (!nv12) return nv12;

    DWORD streamIndex = 0, flags = 0;
    ComPtr<IMFSample> sample;
    hr = reader->ReadSample(kFirstVideoStream, 0, &streamIndex, &flags, nullptr, &sample);
    if (FAILED(hr) || !sample)
        return std::unexpected(L"hardware probe: no first sample");

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)))
        return std::unexpected(L"hardware probe: no media buffer");

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    hr = ::MFGetService(buffer.Get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&dxgiBuffer));
    if (FAILED(hr))
        return std::unexpected(L"hardware probe: decoder produced system-memory samples");

    PROPVARIANT zero{};
    zero.vt = VT_I8;
    zero.hVal.QuadPart = 0;
    reader->SetCurrentPosition(GUID_NULL, zero);

    hardware_ = true;
    dxgiManager_ = std::move(dxgiManager);
    detectDecoder(reader.Get());
    reader_ = std::move(reader);
    metadata_ = std::move(meta);
    opened_ = true;
    return {};
}

Result<void> DecoderManager::openSoftware(const std::wstring& path) {
    auto nv12 = openSoftwareNv12(path);
    if (nv12) return {};
    log::Logger::instance().warn(L"software NV12 unavailable ({}); falling back to RGB32", nv12.error());
    return openSoftwareRgb32(path);
}

Result<void> DecoderManager::openSoftwareNv12(const std::wstring& path) {
    VideoMetadata meta;
    ComPtr<IMFSourceReader> reader;
    auto prep = prepareReader(path, nullptr, reader, meta);
    if (!prep) return prep;
    auto negotiated = negotiateNv12Output(reader.Get());
    if (!negotiated) return negotiated;

    softwareNv12_ = true;
    decoderName_ = L"software (NV12 output)";
    reader_ = std::move(reader);
    metadata_ = std::move(meta);
    opened_ = true;
    return {};
}

Result<void> DecoderManager::openSoftwareRgb32(const std::wstring& path) {
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = ::MFCreateAttributes(&attrs, 4);
    if (FAILED(hr)) return std::unexpected(L"MFCreateAttributes failed");
    if (FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)))
        return std::unexpected(L"could not set video-processing attribute");

    VideoMetadata meta;
    ComPtr<IMFSourceReader> reader;
    auto prep = prepareReader(path, attrs.Get(), reader, meta);
    if (!prep) return prep;
    auto negotiated = negotiateRgb32Output(reader.Get());
    if (!negotiated) return negotiated;

    softwareNv12_ = false;
    decoderName_ = L"software (RGB32 output)";
    reader_ = std::move(reader);
    metadata_ = std::move(meta);
    opened_ = true;
    return {};
}

Result<void> DecoderManager::prepareReader(const std::wstring& path, IMFAttributes* attrs,
                                           ComPtr<IMFSourceReader>& reader, VideoMetadata& meta) {
    HRESULT hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs, &reader);
    if (FAILED(hr)) return std::unexpected(L"MFCreateSourceReaderFromURL failed: " + formatHr(hr));
    if (FAILED(reader->SetStreamSelection(kAllStreams, FALSE)))
        return std::unexpected(L"could not deselect streams");
    if (FAILED(reader->SetStreamSelection(kFirstVideoStream, TRUE)))
        return std::unexpected(L"no video stream in file");

    meta.path = path;
    ComPtr<IMFMediaType> native;
    hr = reader->GetCurrentMediaType(kFirstVideoStream, &native);
    if (FAILED(hr)) return std::unexpected(L"no video stream in file");
    auto fill = VideoMetadata::fillFromMediaType(native.Get(), meta);
    if (!fill) return fill;

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
                        ComPtr<IMFMediaTypeHandler> handler;
                        GUID major{};
                        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)) &&
                            SUCCEEDED(handler->GetMajorType(&major)) && major == MFMediaType_Audio)
                            meta.hasAudio = true;
                    }
                }
            }
        }
    }
    return {};
}

Result<void> DecoderManager::start(FrameQueue* queue, LONGLONG position100ns) {
    if (!opened_ || !reader_) return std::unexpected(L"DecoderManager::start: not opened");
    if (worker_.joinable()) return std::unexpected(L"DecoderManager::start: already running");
    if (!queue) return std::unexpected(L"DecoderManager::start: null queue");

    queue_ = queue;
    { PROPVARIANT var{}; var.vt = VT_I8; var.hVal.QuadPart = position100ns;
      reader_->SetCurrentPosition(GUID_NULL, var); }

    stopRequested_.store(false);
    decodedFrames_.store(0);
    worker_ = std::thread([this, queue] { workerLoop(queue); });
    return {};
}

void DecoderManager::stop() {
    stopRequested_.store(true);
    if (queue_) queue_->close();
    if (worker_.joinable()) worker_.join();
    queue_ = nullptr;
}

void DecoderManager::close() {
    stop();
    reader_.Reset();
    opened_ = false;
}

void DecoderManager::detectDecoder(IMFSourceReader* reader) {
    ComPtr<IMFGetService> svc;
    if (!reader || FAILED(reinterpret_cast<IUnknown*>(reader)->QueryInterface(IID_PPV_ARGS(&svc)))) return;
    ComPtr<IMFTransform> transform;
    if (FAILED(svc->GetService(MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&transform)))) return;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(transform->GetAttributes(&attrs))) return;
    wchar_t hwUrl[128]{}; UINT32 len = 0;
    const bool isHw = SUCCEEDED(attrs->GetString(MFT_ENUM_HARDWARE_URL_Attribute, hwUrl, 128, &len)) && len > 0;
    GUID clsid{};
    attrs->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
    decoderName_ = clsidFriendlyName(clsid);
    log::Logger::instance().info(L"decoder: {} ({})", decoderName_, isHw ? L"hardware" : L"software");
}

void DecoderManager::workerLoop(FrameQueue* queue) {
    const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto& log = log::Logger::instance();

    while (!stopRequested_.load()) {
        DWORD streamIndex = 0, flags = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample(kFirstVideoStream, 0, &streamIndex, &flags, nullptr, &sample);
        if (FAILED(hr)) { DecodedFrame end; end.endOfStream = true; queue->push(std::move(end)); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { DecodedFrame end; end.endOfStream = true; queue->push(std::move(end)); break; }
        if (!sample) continue;

        DecodedFrame frame;
        std::wstring copyErr;
        if (hardware_) {
            if (!copySampleToTexture(sample.Get(), frame, copyErr)) { log.warn(L"frame surface copy failed: {}", copyErr); continue; }
        } else {
            ComPtr<IMFMediaType> current;
            if (FAILED(reader_->GetCurrentMediaType(kFirstVideoStream, &current))) continue;
            UINT w = 0, h = 0;
            ::MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w == 0 || h == 0) continue;
            if (softwareNv12_) {
                if (!copySampleToNv12(sample.Get(), w, h, frame, copyErr)) { log.warn(L"frame copy failed: {}", copyErr); continue; }
            } else if (!copySampleToFrame(sample.Get(), w, h, frame, copyErr)) { log.warn(L"frame copy failed: {}", copyErr); continue; }
        }

        if (metadata_.displayAspect > 0.0) frame.displayAspect = static_cast<float>(metadata_.displayAspect);
        frame.decodeTime100ns = util::Clock::instance().now100ns();
        if (!queue->push(std::move(frame))) break;
        decodedFrames_.fetch_add(1);
    }

    if (SUCCEEDED(comHr)) ::CoUninitialize();
}

bool DecoderManager::copySampleToTexture(IMFSample* sample, DecodedFrame& out, std::wstring& err) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) { err = L"GetBufferByIndex failed"; return false; }
    sample->GetSampleTime(&out.timestamp);

    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    hr = ::MFGetService(buffer.Get(), MR_VIDEO_ACCELERATION_SERVICE, IID_PPV_ARGS(&dxgiBuffer));
    if (FAILED(hr)) { err = L"not a DXGI buffer"; return false; }

    ComPtr<ID3D11Texture2D> texture;
    hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) { err = L"GetResource failed"; return false; }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    out.texture = std::move(texture);
    out.width = desc.Width;
    out.height = desc.Height;
    out.hardware = true;
    return true;
}

bool DecoderManager::copySampleToNv12(IMFSample* sample, UINT width, UINT height,
                                      DecodedFrame& out, std::wstring& err) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) { err = L"GetBufferByIndex failed"; return false; }
    sample->GetSampleTime(&out.timestamp);

    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* scanline0 = nullptr; LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        hr = buffer2d->Lock2D(&scanline0, &pitch);
        if (FAILED(hr)) { err = L"Lock2D failed"; return false; }
    } else {
        DWORD len = 0;
        hr = buffer->Lock(&scanline0, nullptr, &len);
        if (FAILED(hr)) { err = L"Lock failed"; return false; }
        pitch = 0;
    }

    out.width = width; out.height = height; out.nv12 = true;
    const size_t srcPitch = pitch > 0 ? static_cast<size_t>(pitch) : static_cast<size_t>(width);
    const size_t rowBytes = static_cast<size_t>(width);
    out.bytes.resize(rowBytes * static_cast<size_t>(height) * 3 / 2);
    BYTE* dst = out.bytes.data();
    for (UINT y = 0; y < height; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes, scanline0 + static_cast<size_t>(y) * srcPitch, rowBytes);
    const BYTE* uv = scanline0 + srcPitch * static_cast<size_t>(height);
    const size_t ySize = rowBytes * static_cast<size_t>(height);
    for (UINT y = 0; y < height / 2; ++y)
        std::memcpy(dst + ySize + static_cast<size_t>(y) * rowBytes, uv + static_cast<size_t>(y) * srcPitch, rowBytes);

    if (buffer2d) buffer2d->Unlock2D(); else buffer->Unlock();
    return true;
}

bool DecoderManager::copySampleToFrame(IMFSample* sample, UINT width, UINT height,
                                       DecodedFrame& out, std::wstring& err) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) { err = L"GetBufferByIndex failed"; return false; }
    sample->GetSampleTime(&out.timestamp);

    ComPtr<IMF2DBuffer> buffer2d;
    BYTE* scanline0 = nullptr; LONG pitch = 0;
    if (SUCCEEDED(buffer.As(&buffer2d))) {
        hr = buffer2d->Lock2D(&scanline0, &pitch);
        if (FAILED(hr)) { err = L"Lock2D failed"; return false; }
    } else {
        DWORD len = 0;
        hr = buffer->Lock(&scanline0, nullptr, &len);
        if (FAILED(hr)) { err = L"Lock failed"; return false; }
        pitch = 0;
    }

    out.width = width; out.height = height;
    const size_t srcPitch = pitch > 0 ? static_cast<size_t>(pitch) : static_cast<size_t>(width) * 4;
    out.bytes.resize(static_cast<size_t>(height) * static_cast<size_t>(width) * 4);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(out.bytes.data() + static_cast<size_t>(y) * width * 4,
                    scanline0 + static_cast<size_t>(y) * srcPitch, static_cast<size_t>(width) * 4);

    if (buffer2d) buffer2d->Unlock2D(); else buffer->Unlock();
    return true;
}

Result<VideoMetadata> DecoderManager::probeMetadata(const std::wstring& path) {
    ComPtr<IMFSourceReader> reader;
    VideoMetadata meta;
    auto prepared = prepareReader(path, nullptr, reader, meta);
    if (!prepared) return std::unexpected(prepared.error());
    return meta;
}

} // namespace vw::video
