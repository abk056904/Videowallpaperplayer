#pragma once

#include <format>
#include <string>

#include <windows.h>

namespace vw::util {

// Format an HRESULT as L"0x{:08X}". Used throughout the graphics/video stack
// for human-readable error messages. Extracted from per-file duplicates
// (DecoderManager, D3D11Renderer, D3D11DeviceManager, TextureManager).
inline std::wstring formatHr(HRESULT hr) {
    return std::format(L"0x{:08X}", static_cast<unsigned>(hr));
}

} // namespace vw::util
