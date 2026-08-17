#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <d3d11.h>

// mfreadwrite.h requires the interfaces from mfidl.h to be declared first
// (IMFSourceReader, IMFAttributes, ...) — include order matters.
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "util/Result.h"
#include "video/FrameQueue.h"
#include "video/VideoMetadata.h"

namespace vw::video {

// Media Foundation decode session (docs/03 §3.6/§3.7, M4 + M5): opens a file
// through the Source Reader, deselects ALL streams except video (audio is out
// of scope for v1 — never initialized), reads metadata from the native media
// type + presentation descriptor, and runs a demand-driven decode worker that
// pushes frames into a FrameQueue.
//
// M5 hardware path: when a D3D11 device is supplied via setD3DDevice(), an
// IMFDXGIDeviceManager is created and the reader is asked for NV12 GPU
// surfaces (MF_SOURCE_READER_D3D_MANAGER + MF_READWRITE_ENABLE_HARDWARE_
// TRANSFORMS). Frames arrive as ID3D11Texture2D (no CPU copy). If NV12
// negotiation fails the session falls back to the M4 RGB32 software path with
// a diagnostic.
class DecoderManager {
public:
    DecoderManager() = default;
    ~DecoderManager();

    DecoderManager(const DecoderManager&) = delete;
    DecoderManager& operator=(const DecoderManager&) = delete;

    // Supplies the D3D device for the hardware path (call before open();
    // nullptr = software-only). The device must outlive this manager.
    void setD3DDevice(ID3D11Device* device) { d3dDevice_ = device; }

    // Opens the file and validates it via real media metadata (never the file
    // extension). Fails gracefully (Result error) on corrupt/unsupported.
    Result<void> open(const std::wstring& path);

    const VideoMetadata& metadata() const { return metadata_; }

    // True when the active session decodes to GPU surfaces (NV12) instead of
    // RGB32 bytes.
    bool hardwareDecoding() const { return hardware_; }

    // Honest decoder identification (docs/03 §3.7): the friendly name read
    // from the registry for the active MFT's CLSID (never fabricated), e.g.
    // "NVIDIA ..." or "Microsoft H.264 Video Decoder MFT". Empty when the
    // MFT could not be introspected.
    const std::wstring& decoderName() const { return decoderName_; }

    // Spawns the decode worker. `queue` must outlive stop() and remain valid
    // while the worker runs. If `position100ns` > 0 the source is seeked
    // there first (pause/resume).
    Result<void> start(FrameQueue* queue, LONGLONG position100ns = 0);

    // Joins the worker (unblocking it if it is stuck pushing into a full
    // queue). Keeps the reader + metadata so a paused session can resume with
    // start(). Idempotent.
    void stop();

    // stop() + releases the reader entirely (fresh open() needed afterwards).
    void close();

    bool isRunning() const { return worker_.joinable(); }

    // Frames successfully pushed since the last start() (M6 decodedFps stats).
    uint64_t decodedFrames() const { return decodedFrames_.load(); }

    // open() split: hardware path (DXGI manager + NV12 + probe), software
    // path (VP MFT + RGB32). The hardware path PROBES the first sample — on
    // machines where NV12 negotiates but the decoder hands back system-memory
    // samples (no hardware MFT), the whole reader is discarded and open()
    // retries via openSoftware (re-negotiating RGB32 on the same reader fails
    // with MF_E_INVALIDTYPE once NV12 is committed — probed).
    Result<void> openHardware(const std::wstring& path);
    Result<void> openSoftware(const std::wstring& path);

private:
    void workerLoop(FrameQueue* queue); // local queue ptr: stop() may null the member
    static bool copySampleToFrame(IMFSample* sample, UINT width, UINT height, DecodedFrame& out,
                                  std::wstring& err);
    static bool copySampleToTexture(IMFSample* sample, DecodedFrame& out, std::wstring& err);
    static std::wstring formatHr(HRESULT hr);
    void detectDecoder(IMFSourceReader* reader); // MFT CLSID -> registry name
    static Result<void> prepareReader(const std::wstring& path, IMFAttributes* attrs,
                                      Microsoft::WRL::ComPtr<IMFSourceReader>& reader,
                                      VideoMetadata& meta);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    VideoMetadata metadata_;
    ID3D11Device* d3dDevice_ = nullptr; // non-owning; must outlive this manager
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> decodedFrames_{0}; // M6: since last start()
    FrameQueue* queue_ = nullptr;
    std::wstring decoderName_;
    bool hardware_ = false;
    bool opened_ = false;
};

} // namespace vw::video
