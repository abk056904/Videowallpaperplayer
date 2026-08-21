#include "graphics/D3D11Renderer.h"

#include <algorithm>

#include "graphics/D3D11DeviceManager.h"
#include "util/HrToString.h"
#include "graphics/TextureManager.h"
#include "generated/VideoShaderPsData.h"
#include "generated/VideoShaderPsTexData.h"
#include "generated/VideoShaderPsYuvData.h"
#include "generated/VideoShaderVsData.h"

namespace vw::gfx {

namespace {

using vw::util::formatHr;

} // namespace

Result<void> D3D11Renderer::init(ID3D11Device* device, IDXGISwapChain1* swapChain, UINT width,
                                 UINT height) {
    if (!device || !swapChain) return std::unexpected(L"renderer init: null device/swap chain");
    swapChain_ = swapChain;

    // Shaders from embedded build-time-compiled .cso byte arrays.
    if (FAILED(device->CreateVertexShader(kVideoShaderVs, kVideoShaderVs_size, nullptr, &vs_))) {
        return std::unexpected(L"CreateVertexShader failed");
    }
    if (FAILED(device->CreatePixelShader(kVideoShaderPs, kVideoShaderPs_size, nullptr, &ps_))) {
        return std::unexpected(L"CreatePixelShader failed");
    }
    if (FAILED(device->CreatePixelShader(kVideoShaderPsTex, kVideoShaderPsTex_size, nullptr,
                                         &psTex_))) {
        return std::unexpected(L"CreatePixelShader (textured) failed");
    }
    if (FAILED(device->CreatePixelShader(kVideoShaderPsYuv, kVideoShaderPsYuv_size, nullptr,
                                         &psYuv_))) {
        return std::unexpected(L"CreatePixelShader (YUV) failed");
    }

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FrameParams); // tint + scaleOffset, 16-byte aligned
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cb, nullptr, &frameCb_))) {
        return std::unexpected(L"CreateBuffer (frame CB) failed");
    }

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // fullscreen-triangle winding is arbitrary
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &rasterizer_))) {
        return std::unexpected(L"CreateRasterizerState failed");
    }

    // Use the shared TextureManager helper rather than duplicating sampler setup.
    auto sampler = TextureManager::createSampler(device, D3D11_FILTER_MIN_MAG_MIP_LINEAR);
    if (!sampler) return std::unexpected(sampler.error());
    sampler_ = *sampler;
    // Letterbox sampler: linear + black BORDER so Fit/Center margins render
    // black instead of smearing the video edge (CLAMP would stretch the edge
    // texel column/row across the bar — see ScaleMath.h).
    D3D11_SAMPLER_DESC borderDesc{};
    borderDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    borderDesc.AddressU = borderDesc.AddressV = borderDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    borderDesc.BorderColor[0] = borderDesc.BorderColor[1] = borderDesc.BorderColor[2] = 0.0f;
    borderDesc.BorderColor[3] = 1.0f; // opaque black
    if (FAILED(device->CreateSamplerState(&borderDesc, &borderSampler_))) {
        return std::unexpected(L"CreateSamplerState (border) failed");
    }

    // Create blend state for alpha-over overlay (premultiplied alpha).
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;          // premultiplied src
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &overlayBlend_))) {
        return std::unexpected(L"CreateBlendState (overlay blend) failed");
    }

    // The swap chain was just created at this size — build the RTV directly;
    // a same-size ResizeBuffers on a fresh flip-model chain fails (INVALID_CALL).
    return rebuildRtv(device, width, height);
}

Result<void> D3D11Renderer::setVideoTexture(ID3D11ShaderResourceView* srv, float videoAspect,
                                           Scaling scaling) {
    if (srv && !psTex_) return std::unexpected(L"renderer: textured shader not initialized");
    videoSrv_ = srv;
    ySrv_.Reset();
    uvSrv_.Reset();
    if (srv) {
        const ScaleOffset so = computeScaleOffset(width_, height_, videoAspect, scaling);
        scaleOffset_[0] = so.sx;
        scaleOffset_[1] = so.sy;
        scaleOffset_[2] = so.ox;
        scaleOffset_[3] = so.oy;
        needsBorder_ = scalingNeedsBorder(scaling);
    }
    return {};
}

Result<void> D3D11Renderer::setVideoPlanes(ID3D11ShaderResourceView* ySrv,
                                           ID3D11ShaderResourceView* uvSrv, float videoAspect,
                                           Scaling scaling) {
    if (ySrv && !psYuv_) return std::unexpected(L"renderer: YUV shader not initialized");
    ySrv_ = ySrv;
    uvSrv_ = uvSrv;
    if (!ySrv) {
        videoSrv_.Reset();
        return {};
    }
    // Map the fullscreen UV [0,1]^2 onto the texture UV per the scaling mode
    // (pure math in ScaleMath.h, unit-tested): texUv = uv*scale + offset.
    const ScaleOffset so = computeScaleOffset(width_, height_, videoAspect, scaling);
    scaleOffset_[0] = so.sx;
    scaleOffset_[1] = so.sy;
    scaleOffset_[2] = so.ox;
    scaleOffset_[3] = so.oy;
    needsBorder_ = scalingNeedsBorder(scaling);
    videoSrv_.Reset();
    return {};
}

Result<void> D3D11Renderer::render(ID3D11DeviceContext* context, const FrameParams& params) {
    deviceLost_ = false; // per-call: only the CURRENT failure reports device loss
    if (!context || !rtv_) return std::unexpected(L"renderer: not initialized");

    const float clear[4] = {0.04f, 0.05f, 0.09f, 1.0f}; // solid base color
    context->ClearRenderTargetView(rtv_.Get(), clear);
    context->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    // Per-frame params: tint + the scaling computed by setVideoTexture/setVideoPlanes
    // (identity for the placeholder). Both video paths honor the same mapping.
    FrameParams cbParams = params;
    if (ySrv_ || videoSrv_) {
        cbParams.scaleOffset[0] = scaleOffset_[0];
        cbParams.scaleOffset[1] = scaleOffset_[1];
        cbParams.scaleOffset[2] = scaleOffset_[2];
        cbParams.scaleOffset[3] = scaleOffset_[3];
    } else {
        cbParams.scaleOffset[0] = 1.0f;
        cbParams.scaleOffset[1] = 1.0f;
        cbParams.scaleOffset[2] = 0.0f;
        cbParams.scaleOffset[3] = 0.0f;
    }
    // Dirty-track the CB (spec §22: never update unchanged data): tint is a
    // constant 1.0 and scaleOffset changes only on a video size/scaling
    // change, so the update happens once per change, not once per frame.
    if (!(cbParams == cb_)) {
        context->UpdateSubresource(frameCb_.Get(), 0, nullptr, &cbParams, 0, 0);
        cb_ = cbParams;
    }

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr); // vertex-less: SV_VertexID only
    context->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* const nullSrvs[2] = {nullptr, nullptr};
    if (ySrv_) {
        // Hardware path: NV12/P010 planes through the YUV shader.
        ID3D11ShaderResourceView* const srvs[2] = {ySrv_.Get(), uvSrv_.Get()};
        context->PSSetShader(psYuv_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 2, srvs);
    } else if (videoSrv_) {
        // Software path: RGB32 frame through the textured shader.
        context->PSSetShader(psTex_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, videoSrv_.GetAddressOf());
        context->PSSetShaderResources(1, 1, nullSrvs); // clear stale YUV binding
    } else {
        context->PSSetShader(ps_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 2, nullSrvs); // clear stale bindings
    }
    context->VSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    context->RSSetState(rasterizer_.Get());
    // Fit/Center letterbox margins must sample BLACK (BORDER) rather than
    // CLAMP (which would smear the video edge across the bar).
    ID3D11SamplerState* const sampler = needsBorder_ ? borderSampler_.Get() : sampler_.Get();
    context->PSSetSamplers(0, 1, &sampler);
    context->RSSetViewports(1, &viewport_);
    context->Draw(3, 0);

    // --- Overlay pass: draw the debug text in the top-left corner ---
    if (overlaySrv_ && overlayW_ > 0 && overlayH_ > 0) {
        // Small viewport in the top-left corner with padding.
        const float pad = 10.0f;
        D3D11_VIEWPORT ovVP{};
        ovVP.TopLeftX = pad;
        ovVP.TopLeftY = pad;
        ovVP.Width = static_cast<float>(overlayW_);
        ovVP.Height = static_cast<float>(overlayH_);
        ovVP.MinDepth = 0.0f;
        ovVP.MaxDepth = 1.0f;

        // UV scale/offset to map the overlay texture to the quad.
        FrameParams ovCb{};
        ovCb.tint[0] = ovCb.tint[1] = ovCb.tint[2] = ovCb.tint[3] = 1.0f;
        ovCb.scaleOffset[0] = 1.0f;  // sx
        ovCb.scaleOffset[1] = 1.0f;  // sy
        ovCb.scaleOffset[2] = 0.0f;  // ox
        ovCb.scaleOffset[3] = 0.0f;  // oy
        context->UpdateSubresource(frameCb_.Get(), 0, nullptr, &ovCb, 0, 0);

        // Enable alpha blending for the overlay.
        const float blendFactor[4] = {0, 0, 0, 0};
        context->OMSetBlendState(overlayBlend_.Get(), blendFactor, 0xFFFFFFFF);

        context->RSSetViewports(1, &ovVP);
        ID3D11ShaderResourceView* ovSrv = overlaySrv_.Get();
        ID3D11ShaderResourceView* nullSrv[2] = {nullptr, nullptr};
        context->PSSetShader(psTex_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &ovSrv);
        context->PSSetShaderResources(1, 1, nullSrv);
        context->RSSetState(rasterizer_.Get());
        ID3D11SamplerState* smp = sampler_.Get();
        context->PSSetSamplers(0, 1, &smp);
        context->Draw(3, 0);

        // Restore default blend state (no alpha blend for main render next frame).
        context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    }

    // Present with vsync; device loss propagates for the M12 recreate path.
    const HRESULT hr = swapChain_->Present(1, 0);
    if (D3D11DeviceManager::isDeviceLost(hr)) {
        deviceLost_ = true;
        return std::unexpected(std::wstring(L"present failed: ") +
                               D3D11DeviceManager::deviceLostReason(hr));
    }
    if (FAILED(hr)) {
        return std::unexpected(L"present failed: " + formatHr(hr));
    }
    return {};
}

Result<void> D3D11Renderer::resize(ID3D11Device* device, UINT width, UINT height) {
    if (!device || !swapChain_ || width == 0 || height == 0) {
        return std::unexpected(L"renderer resize: bad arguments");
    }
    if (width == width_ && height == height_) return {}; // no-op: already this size

    const HRESULT hr =
        swapChain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) return std::unexpected(L"ResizeBuffers failed: " + formatHr(hr));
    return rebuildRtv(device, width, height);
}

void D3D11Renderer::setOverlay(ID3D11ShaderResourceView* srv, UINT overlayW, UINT overlayH) {
    overlaySrv_ = srv;
    overlayW_ = overlayW;
    overlayH_ = overlayH;
}

Result<void> D3D11Renderer::rebuildRtv(ID3D11Device* device, UINT width, UINT height) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) return std::unexpected(L"GetBuffer(0) failed: " + formatHr(hr));
    rtv_.Reset();
    hr = device->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
    if (FAILED(hr)) return std::unexpected(L"CreateRenderTargetView failed: " + formatHr(hr));

    viewport_.TopLeftX = 0;
    viewport_.TopLeftY = 0;
    viewport_.Width = static_cast<float>(width);
    viewport_.Height = static_cast<float>(height);
    viewport_.MinDepth = 0.0f;
    viewport_.MaxDepth = 1.0f;
    width_ = width;
    height_ = height;
    return {};
}

} // namespace vw::gfx
