#pragma once

#include <memory>
#include <string>

#include <d3d11.h>

#include "video/IVideoDecoder.h"

namespace vw::video {

// Adapter information for diagnostic reporting
struct AdapterInfo {
    std::wstring name;
    LUID luid{};
    bool isRenderDevice = false;
    bool hasNvdec = false;
    bool hasD3d11va = false;
};

// Creates the best video decoder backend for the given file.
//
// Selection order (codec-aware, each step falls through on failure):
//   H.264/HEVC: MF hardware → D3D11VA → CUDA → FFmpeg software
//   VP9:        D3D11VA → CUDA → FFmpeg software
//   AV1:        CUDA → FFmpeg software
//   Other:      FFmpeg software
//
// The factory returns the first backend that successfully:
//   1. Probes the file to determine the codec
//   2. Opens the stream with a HW-capable decoder
//   3. Initializes hardware resources on the render device
//
// A failed hardware initialization never terminates playback — the factory
// falls back to the next backend, ending with pure software if nothing else
// works. See FFmpegDecoder.h for the FFmpeg-specific details.
std::unique_ptr<IVideoDecoder> CreateBestDecoder(
    ID3D11Device* renderDevice,
    const std::wstring& path,
    AdapterInfo* selectedAdapter = nullptr  // OUT: which adapter won
);

} // namespace vw::video
