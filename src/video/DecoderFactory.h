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
// Selection order (codec-aware):
//   H.264/HEVC: MF hardware → NVDEC → FFmpeg software
//   VP9/AV1:    NVDEC → FFmpeg software
//   Other:      FFmpeg software
//
// The factory returns the first backend that successfully:
//   1. Opens the stream
//   2. Confirms codec compatibility
//   3. Initializes hardware resources
//
// A failed hardware initialization never terminates playback.
std::unique_ptr<IVideoDecoder> CreateBestDecoder(
    ID3D11Device* renderDevice,
    const std::wstring& path,
    AdapterInfo* selectedAdapter = nullptr  // OUT: which adapter won
);

} // namespace vw::video
