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
    LONGLONG timestamp = 0;     // media time, 100 ns units
    bool endOfStream = false;   // sentinel: the source reached the end
    bool hardware = false;      // true when `texture` is the payload
};

} // namespace vw::video
