#pragma once

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <wrl/client.h>

#include "util/Result.h"

namespace vw::gfx {

// Debug overlay rendered via GDI text onto a D3D11 texture (top-left corner).
// The overlay shows FPS, decoder info, RAM usage, and dropped frames.
// Updated at ~1 Hz to avoid GDI overhead per frame.
class DebugOverlay {
public:
    DebugOverlay() = default;
    ~DebugOverlay() = default;

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    // Initialize the GDI-backed texture. Must be called on the render thread.
    Result<void> init(ID3D11Device* device, UINT width, UINT height);

    // Update the overlay text and re-render to the texture.
    // Stats are formatted and drawn via GDI at most once per call.
    void update(const wchar_t* line1, const wchar_t* line2,
                const wchar_t* line3, const wchar_t* line4);

    // Resize the overlay texture (e.g. on window resize).
    Result<void> resize(ID3D11Device* device, UINT width, UINT height);

    // Render the text to the texture (call when textDirty_). Idempotent.
    Result<void> renderText(ID3D11Device* device);

    // Access the rendered SRV for compositing.
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    UINT width() const { return width_; }
    UINT height() const { return height_; }
    bool valid() const { return srv_ != nullptr; }

    void release();

private:

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    void* gdiDc_ = nullptr; // HDC from GetDC on the shared texture
    HBITMAP gdiBitmap_ = nullptr;
    HDC gdiMemDc_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    // Cached stats to avoid re-rendering when nothing changed
    std::wstring lines_[4];
    bool textDirty_ = true;
};

} // namespace vw::gfx
