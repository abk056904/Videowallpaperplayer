#include "graphics/TextureManager.h"

#include "util/HrToString.h"

namespace vw::gfx {

namespace {

using vw::util::formatHr;

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

Result<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> TextureManager::createPlaneSrv(
    ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format) {
    if (!texture) return std::unexpected(L"createPlaneSrv: null texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    const HRESULT hr = device->CreateShaderResourceView(texture, &desc, &srv);
    if (FAILED(hr)) {
        return std::unexpected(L"CreateShaderResourceView (plane " +
                               std::to_wstring(static_cast<int>(format)) + L") failed: " +
                               formatHr(hr));
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
