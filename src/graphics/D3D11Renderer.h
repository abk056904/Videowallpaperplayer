#pragma once

#include <cstdint>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "util/Result.h"

namespace vw::gfx {

// Renders the fullscreen-triangle placeholder (solid color + UV gradient) to a
// swap-chain back buffer (docs/02 §2.3, M2). Shaders are compiled at build
// time by fxc and embedded (see shaders/VideoShader.hlsl and
// generated/VideoShader*Data.h). M5 replaces the placeholder pixel shader with
// video-texture sampling; the constant buffer stays the per-frame interface.
class D3D11Renderer {
public:
    struct FrameParams {
        float tint[4]; // per-frame color modulation
    };

    D3D11Renderer() = default;

    // Stores the swap chain, creates the pipeline, and builds the RTV.
    Result<void> init(ID3D11Device* device, IDXGISwapChain1* swapChain, UINT width, UINT height);

    // Clears, draws, and presents (vsync). Device loss propagates as an error
    // so the caller can run the M12 recreate path.
    Result<void> render(ID3D11DeviceContext* context, const FrameParams& params);

    // Resizes the swap chain and rebuilds the RTV + viewport. No-op when the
    // size is unchanged (avoids the flip-model same-size ResizeBuffers error).
    Result<void> resize(ID3D11Device* device, UINT width, UINT height);

private:
    Result<void> rebuildRtv(ID3D11Device* device, UINT width, UINT height);
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> frameCb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_; // linear; used from M5 on
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    D3D11_VIEWPORT viewport_{};
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace vw::gfx
