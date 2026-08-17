#include "graphics/TextureManager.h"

#include <format>

namespace vw::gfx {

namespace {

std::wstring formatHr(HRESULT hr) {
    return std::format(L"hr=0x{:08X}", static_cast<unsigned>(hr));
}

} // namespace

Result<Microsoft::WRL::ComPtr<ID3D11Texture2D>> TextureManager::createTexture(
    ID3D11Device* device, DXGI_FORMAT format, UINT width, UINT height, bool dynamic) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        return std::unexpected(L"CreateTexture2D failed: " + formatHr(hr));
    }
    return texture;
}

Result<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> TextureManager::createSrv(
    ID3D11Device* device, ID3D11Texture2D* texture) {
    if (!texture) return std::unexpected(L"createSrv: null texture");
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device->CreateShaderResourceView(texture, nullptr, &srv);
    if (FAILED(hr)) {
        return std::unexpected(L"CreateShaderResourceView failed: " + formatHr(hr));
    }
    return srv;
}

Result<Microsoft::WRL::ComPtr<ID3D11SamplerState>> TextureManager::createSampler(
    ID3D11Device* device, D3D11_FILTER filter) {
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = filter;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    const HRESULT hr = device->CreateSamplerState(&desc, &sampler);
    if (FAILED(hr)) {
        return std::unexpected(L"CreateSamplerState failed: " + formatHr(hr));
    }
    return sampler;
}

} // namespace vw::gfx
