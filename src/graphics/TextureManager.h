#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "util/Result.h"

namespace vw::gfx {

// RAII texture / SRV / sampler helpers (docs/02 §2.3). M2 ships the skeleton;
// M5 uses these for video frames (NV12/P010 textures + SRVs, CPU-updated).
class TextureManager {
public:
    // 2D texture with optional dynamic CPU-write access (for video frames).
    static Result<Microsoft::WRL::ComPtr<ID3D11Texture2D>> createTexture(
        ID3D11Device* device, DXGI_FORMAT format, UINT width, UINT height, bool dynamic);

    // Shader-resource view over a texture (default view for its format).
    static Result<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> createSrv(
        ID3D11Device* device, ID3D11Texture2D* texture);

    // Sampler state with the given filter and clamp addressing.
    static Result<Microsoft::WRL::ComPtr<ID3D11SamplerState>> createSampler(
        ID3D11Device* device, D3D11_FILTER filter);
};

} // namespace vw::gfx
