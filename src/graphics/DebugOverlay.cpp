#include "graphics/DebugOverlay.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "graphics/TextureManager.h"

namespace vw::gfx {

namespace {

constexpr int kLineHeight = 18;
constexpr int kPadding = 8;
constexpr int kBorderWidth = 1;

} // namespace

Result<void> DebugOverlay::init(ID3D11Device* device, UINT width, UINT height) {
    if (!device || width == 0 || height == 0)
        return std::unexpected(L"debug overlay: bad dimensions");

    width_ = width;
    height_ = height;

    // Create the D3D11 texture (staging + default for GDI upload).
    auto texResult = TextureManager::createTexture(device, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                    width, height, true);
    if (!texResult) return std::unexpected(texResult.error());
    texture_ = *texResult;

    auto srvResult = TextureManager::createSrv(device, texture_.Get());
    if (!srvResult) return std::unexpected(srvResult.error());
    srv_ = *srvResult;

    return {};
}

void DebugOverlay::update(const wchar_t* line1, const wchar_t* line2,
                           const wchar_t* line3, const wchar_t* line4) {
    // Check if text changed
    const std::wstring newLines[4] = { line1 ? line1 : L"", line2 ? line2 : L"",
                                        line3 ? line3 : L"", line4 ? line4 : L"" };
    textDirty_ = false;
    for (int i = 0; i < 4; ++i) {
        if (lines_[i] != newLines[i]) {
            lines_[i] = newLines[i];
            textDirty_ = true;
        }
    }
}

Result<void> DebugOverlay::renderText(ID3D11Device* device) {
    if (!texture_ || !textDirty_) return {};

    const UINT w = width_;
    const UINT h = height_;

    // Create a GDI-compatible DIB section for rendering text
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(w);
    bi.bmiHeader.biHeight = -static_cast<LONG>(h); // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = ::GetDC(nullptr);
    if (!screenDc) return std::unexpected(L"debug overlay: GetDC failed");

    HDC memDc = ::CreateCompatibleDC(screenDc);
    HBITMAP bmp = ::CreateDIBSection(memDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        ::ReleaseDC(nullptr, screenDc);
        return std::unexpected(L"debug overlay: CreateDIBSection failed");
    }

    HBITMAP oldBmp = reinterpret_cast<HBITMAP>(::SelectObject(memDc, bmp));

    // Fill with semi-transparent black background
    // Use 0x00000000 for DIB (alpha unused in BGRA for D3D upload, but RGB = black)
    std::memset(bits, 0, w * h * 4);

    // Fill background with dark semi-transparent color (RGB only, A=0 for D3D)
    for (UINT y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<uint8_t*>(bits) + y * w * 4;
        for (UINT x = 0; x < w; ++x) {
            row[x * 4 + 0] = 0x00; // B
            row[x * 4 + 1] = 0x00; // G
            row[x * 4 + 2] = 0x00; // R
            row[x * 4 + 3] = 0xCC; // A (for premul; will be used as BGRA8)
        }
    }

    // Draw border
    HBRUSH borderBrush = ::CreateSolidBrush(RGB(0x40, 0x80, 0xC0));
    RECT borderRect = { 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    ::FrameRect(memDc, &borderRect, borderBrush);
    ::DeleteObject(borderBrush);

    // Create monospace font for the overlay
    HFONT font = ::CreateFontW(
        -14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    HFONT oldFont = reinterpret_cast<HFONT>(::SelectObject(memDc, font));

    // Text color: bright cyan on dark background
    ::SetTextColor(memDc, RGB(0x00, 0xD0, 0xFF));
    ::SetBkMode(memDc, TRANSPARENT);

    RECT textRect = { kPadding + kBorderWidth, kPadding + kBorderWidth, 0, 0 };
    for (int i = 0; i < 4; ++i) {
        if (lines_[i].empty()) continue;
        textRect.top = kPadding + kBorderWidth + i * kLineHeight;
        textRect.left = kPadding + kBorderWidth + 4;
        textRect.right = w;
        textRect.bottom = textRect.top + kLineHeight;
        ::DrawTextW(memDc, lines_[i].c_str(), -1, &textRect,
                    DT_LEFT | DT_TOP | DT_NOCLIP);
    }

    ::SelectObject(memDc, oldFont);
    ::DeleteObject(font);
    ::SelectObject(memDc, oldBmp);

    // Upload the GDI bitmap to the D3D11 texture via Map/Unmap (staging)
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) {
        ::DeleteObject(bmp);
        ::DeleteDC(memDc);
        ::ReleaseDC(nullptr, screenDc);
        return std::unexpected(L"debug overlay: no device context");
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        // Copy DIB rows to texture (DIB is top-down, D3D is also top-down)
        const UINT srcPitch = w * 4;
        const UINT dstPitch = mapped.RowPitch;
        const auto* src = static_cast<const uint8_t*>(bits);
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        for (UINT y = 0; y < h; ++y) {
            std::memcpy(dst + y * dstPitch, src + y * srcPitch, srcPitch);
        }
        ctx->Unmap(texture_.Get(), 0);
    }

    ctx->Release();
    ::DeleteObject(bmp);
    ::DeleteDC(memDc);
    ::ReleaseDC(nullptr, screenDc);

    textDirty_ = false;
    return {};
}

Result<void> DebugOverlay::resize(ID3D11Device* device, UINT width, UINT height) {
    release();
    return init(device, width, height);
}

void DebugOverlay::release() {
    srv_.Reset();
    texture_.Reset();
    width_ = height_ = 0;
    textDirty_ = true;
}

} // namespace vw::gfx
