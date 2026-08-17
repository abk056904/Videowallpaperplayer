#pragma once

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

namespace vw::video {

// One decoded frame (docs/02 §2.7). Two paths:
// - Software (M4): `bytes` are tightly packed B8G8R8A8 (upload verbatim).
// - Hardware (M5): `texture` is the decoder's NV12/P010 GPU surface (no CPU
//   copy); plane SRVs are created by the consumer on the D3D device.
struct DecodedFrame {
    std::vector<uint8_t> bytes;   // software path: w*4 per row, no padding
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture; // hardware path
    UINT width = 0;
    UINT height = 0;
    // Display aspect ratio = width/height corrected by the sample aspect
    // ratio (anamorphic content). 0 when unknown, in which case the renderer
    // falls back to width/height. The scaling math MUST use this (not the raw
    // pixel dims) or non-square-pixel videos crop/scale distorted.
    float displayAspect = 0.0f;
    LONGLONG timestamp = 0;     // media time, 100 ns units
    LONGLONG decodeTime100ns = 0; // wall clock (util::Clock) at decode, 100 ns
                                // units — for decodeLatencyMs stats (M6)
    bool endOfStream = false;   // sentinel: the source reached the end
    bool hardware = false;      // true when `texture` is the payload
};

} // namespace vw::video
