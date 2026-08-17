#pragma once

#include <atomic>
#include <thread>

// mfreadwrite.h requires the interfaces from mfidl.h to be declared first
// (IMFSourceReader, IMFAttributes, ...) — include order matters.
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "util/Result.h"
#include "video/FrameQueue.h"
#include "video/VideoMetadata.h"

namespace vw::video {

// Media Foundation decode session (docs/03 §3.6, M4 software path): opens a
// file through the Source Reader, deselects ALL streams except video (audio is
// out of scope for v1 — never initialized), reads metadata from the native
// media type + presentation descriptor, negotiates RGB32 output, and runs a
// demand-driven decode worker that pushes frames into a FrameQueue. M5 adds
// the hardware path (DXGI surfaces + shader YUV->RGB); the reader + metadata
// pieces are shared.
class DecoderManager {
public:
    DecoderManager() = default;
    ~DecoderManager();

    DecoderManager(const DecoderManager&) = delete;
    DecoderManager& operator=(const DecoderManager&) = delete;

    // Opens the file and validates it via real media metadata (never the file
    // extension). Fails gracefully (Result error) on corrupt/unsupported.
    Result<void> open(const std::wstring& path);

    const VideoMetadata& metadata() const { return metadata_; }

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

private:
    void workerLoop(FrameQueue* queue); // local queue ptr: stop() may null the member
    static bool copySampleToFrame(IMFSample* sample, UINT width, UINT height, DecodedFrame& out,
                                  std::wstring& err);
    static std::wstring formatHr(HRESULT hr);

    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    VideoMetadata metadata_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    FrameQueue* queue_ = nullptr;
    bool opened_ = false;
};

} // namespace vw::video
