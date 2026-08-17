#pragma once

#include <cstdint>
#include <vector>

#include <windows.h>

namespace vw::video {

// One decoded frame (docs/02 §2.7, M4 software path). `bytes` are tightly
// packed B8G8R8A8 (DXGI_FORMAT_B8G8R8A8_UNORM order) so they upload verbatim
// to a texture. M5 replaces this with GPU surfaces (no CPU copy).
struct DecodedFrame {
    std::vector<uint8_t> bytes; // w*4 per row, no padding
    UINT width = 0;
    UINT height = 0;
    LONGLONG timestamp = 0;     // media time, 100 ns units
    bool endOfStream = false;   // sentinel: the source reached the end
};

} // namespace vw::video
