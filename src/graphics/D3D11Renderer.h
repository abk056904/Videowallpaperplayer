#pragma once

#include <cstdint>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "graphics/ScaleMath.h" // pure scaling math (shared with tests)
#include "util/Result.h" // (D3D11DeviceManager.h is only needed by the .cpp)

namespace vw::gfx {

// Renders the fullscreen-triangle placeholder (solid color + UV gradient) to a
// swap-chain back buffer (docs/02 §2.3, M2). Shaders are compiled at build
// time by fxc and embedded (see shaders/VideoShader.hlsl and
// generated/VideoShader*Data.h). M5 replaces the placeholder pixel shader with
// video-texture sampling; the constant buffer stays the per-frame interface.
class D3D11Renderer {
public:
    // Texture-to-window mapping (config.playback.scaling, Fill default).
    using Scaling = vw::gfx::Scaling;

    struct FrameParams {
        float tint[4];      // per-frame color modulation
        float scaleOffset[4]; // xy = texture UV scale, zw = UV offset
    };

    D3D11Renderer() = default;

    // Stores the swap chain, creates the pipeline, and builds the RTV.
    Result<void> init(ID3D11Device* device, IDXGISwapChain1* swapChain, UINT width, UINT height);

    // Software path (M4/M5-preview): when set, render() samples this SRV through
    // the textured pixel shader; null restores the gradient placeholder. The
    // video dimensions + scaling mode drive the same UV mapping the hardware
    // path uses, so Fit/Fill/Center/Stretch apply on both paths.
    Result<void> setVideoTexture(ID3D11ShaderResourceView* srv, UINT videoWidth,
                                 UINT videoHeight, Scaling scaling);

    // Hardware path (M5): two plane SRVs over one NV12/P010 decoder texture
    // (Y: R8/R16, UV: R8G8/R16G16) + the video dimensions so the YUV shader
    // can scale per the configured mode. Null ySrv restores the placeholder.
    Result<void> setVideoPlanes(ID3D11ShaderResourceView* ySrv, ID3D11ShaderResourceView* uvSrv,
                                UINT videoWidth, UINT videoHeight, Scaling scaling);

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
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psTex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psYuv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> videoSrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ySrv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uvSrv_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> frameCb_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;      // linear + CLAMP (Fill/Stretch)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> borderSampler_; // linear + BORDER black (Fit/Center)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    D3D11_VIEWPORT viewport_{};
    float scaleOffset_[4] = {1.0f, 1.0f, 0.0f, 0.0f}; // set by setVideoTexture/setVideoPlanes
    bool needsBorder_ = false; // Fit/Center: out-of-range UVs must be black bars
    UINT width_ = 0;
    UINT height_ = 0;
};

} // namespace vw::gfx
