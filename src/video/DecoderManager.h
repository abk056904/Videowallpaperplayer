#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <d3d11.h>

// mfreadwrite.h requires the interfaces from mfidl.h to be declared first
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "util/Result.h"
#include "video/FrameQueue.h"
#include "video/IVideoDecoder.h"
#include "video/VideoMetadata.h"
#include "graphics/D3D11DeviceManager.h"

namespace vw::video {

// Media Foundation decode session implementing IVideoDecoder.
// Uses MF Source Reader with DXGI-backed decoder surfaces for zero-copy
// GPU-resident rendering (H.264, HEVC via system MFT).
//
// Zero-copy path: MF sample → IMFDXGIBuffer → ID3D11Texture2D
// No CPU frame copies for hardware-decoded frames.
class DecoderManager : public IVideoDecoder {
public:
    DecoderManager() = default;
    ~DecoderManager() override;

    DecoderManager(const DecoderManager&) = delete;
    DecoderManager& operator=(const DecoderManager&) = delete;

    void setD3DDevice(ID3D11Device* device) { d3dDevice_ = device; }

    // IVideoDecoder interface
    Result<void> open(const std::wstring& path) override;
    Result<void> start(FrameQueue* queue, LONGLONG position100ns = 0) override;
    void stop() override;
    void close() override;

    bool isHardwareDecoding() const override { return hardware_; }
    const std::wstring& decoderName() const override { return decoderName_; }
    const VideoMetadata& metadata() const override { return metadata_; }
    bool isOpen() const override { return opened_; }
    uint64_t decodedFrames() const override { return decodedFrames_.load(); }

    // Metadata-only probe
    static Result<VideoMetadata> probeMetadata(const std::wstring& path);

private:
    void workerLoop(FrameQueue* queue);
    Result<void> openHardware(const std::wstring& path);
    Result<void> tryHardwareWithDevice(const std::wstring& path, ID3D11Device* device);
    Result<void> openSoftware(const std::wstring& path);
    Result<void> openSoftwareNv12(const std::wstring& path);
    Result<void> openSoftwareRgb32(const std::wstring& path);

    static bool copySampleToTexture(IMFSample* sample, DecodedFrame& out, std::wstring& err);
    static bool copySampleToNv12(IMFSample* sample, UINT width, UINT height,
                                  DecodedFrame& out, std::wstring& err);
    static bool copySampleToFrame(IMFSample* sample, UINT width, UINT height,
                                   DecodedFrame& out, std::wstring& err);
    void detectDecoder(IMFSourceReader* reader);
    static Result<void> prepareReader(const std::wstring& path, IMFAttributes* attrs,
                                      Microsoft::WRL::ComPtr<IMFSourceReader>& reader,
                                      VideoMetadata& meta);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    Microsoft::WRL::ComPtr<ID3D11Device> decodeDevice_;
    VideoMetadata metadata_;
    ID3D11Device* d3dDevice_ = nullptr;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> decodedFrames_{0};
    FrameQueue* queue_ = nullptr;
    std::wstring decoderName_;
    bool hardware_ = false;
    bool softwareNv12_ = false;
    bool opened_ = false;
};

} // namespace vw::video
