#include "graphics/D3D11Renderer.h"

#include <format>

#include "graphics/D3D11DeviceManager.h"
#include "generated/VideoShaderPsData.h"
#include "generated/VideoShaderVsData.h"

namespace vw::gfx {

namespace {

std::wstring formatHr(HRESULT hr) {
    return std::format(L"hr=0x{:08X}", static_cast<unsigned>(hr));
}

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

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(FrameParams); // two float4, 16-byte aligned
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

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sd, &sampler_))) {
        return std::unexpected(L"CreateSamplerState failed");
    }

    // The swap chain was just created at this size — build the RTV directly;
    // a same-size ResizeBuffers on a fresh flip-model chain fails (INVALID_CALL).
    return rebuildRtv(device, width, height);
}

Result<void> D3D11Renderer::render(ID3D11DeviceContext* context, const FrameParams& params) {
    if (!context || !rtv_) return std::unexpected(L"renderer: not initialized");

    const float clear[4] = {0.04f, 0.05f, 0.09f, 1.0f}; // solid base color
    context->ClearRenderTargetView(rtv_.Get(), clear);
    context->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);

    context->UpdateSubresource(frameCb_.Get(), 0, nullptr, &params, 0, 0);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr); // vertex-less: SV_VertexID only
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->PSSetShader(ps_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    context->RSSetState(rasterizer_.Get());
    context->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context->RSSetViewports(1, &viewport_);
    context->Draw(3, 0);

    // Present with vsync; device loss propagates for the M12 recreate path.
    const HRESULT hr = swapChain_->Present(1, 0);
    if (D3D11DeviceManager::isDeviceLost(hr)) {
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
