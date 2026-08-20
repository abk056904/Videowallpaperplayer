#pragma once

// Dark theme palette, DPI helpers, and custom-paint utilities for the Win32 UI.
// All colors are ARGB COLORREF values (0x00BBGGRR). Fonts are Segoe UI Variable
// where available, falling back to Segoe UI. DPI scaling uses per-monitor awareness.

#include <windows.h>
#include <string>

namespace vw::ui::theme {

// ---- Color palette (dark mode) ----
// Background layers (darkest → lightest)
inline constexpr COLORREF kBgBase       = RGB(18, 18, 18);      // main window bg
inline constexpr COLORREF kBgSurface    = RGB(28, 28, 28);      // panel bg
inline constexpr COLORREF kBgCard       = RGB(38, 38, 38);      // card / raised surface
inline constexpr COLORREF kBgHover      = RGB(48, 48, 48);      // hover state
inline constexpr COLORREF kBgPressed    = RGB(55, 55, 55);      // pressed state
inline constexpr COLORREF kBgSelected   = RGB(0, 80, 150);      // selected item / tab

// Borders
inline constexpr COLORREF kBorder       = RGB(55, 55, 55);      // subtle separator
inline constexpr COLORREF kBorderFocus  = RGB(0, 120, 215);     // focused control

// Text
inline constexpr COLORREF kTextPrimary  = RGB(240, 240, 240);   // main text
inline constexpr COLORREF kTextSecondary= RGB(160, 160, 160);   // labels, captions
inline constexpr COLORREF kTextDisabled = RGB(100, 100, 100);   // disabled text
inline constexpr COLORREF kTextAccent   = RGB(0, 150, 255);     // accent / links

// Accent
inline constexpr COLORREF kAccent       = RGB(0, 120, 215);     // WinUI-style accent
inline constexpr COLORREF kAccentHover  = RGB(20, 140, 235);
inline constexpr COLORREF kAccentPressed= RGB(0, 100, 195);

// Status
inline constexpr COLORREF kStatusGreen  = RGB(0, 180, 80);      // playing / success
inline constexpr COLORREF kStatusYellow = RGB(255, 180, 0);     // paused / warning
inline constexpr COLORREF kStatusRed    = RGB(220, 50, 50);     // error / stopped

// ---- Font creation ----

// Creates the standard UI font at the given DPI (9 pt Segoe UI).
// Caller owns the HFONT; destroy with DeleteObject.
inline HFONT createFont(int dpi) {
    const int px = -::MulDiv(9, dpi, 72);
    return ::CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
}

// Bold variant for headings / emphasis.
inline HFONT createFontBold(int dpi) {
    const int px = -::MulDiv(9, dpi, 72);
    return ::CreateFontW(px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
}

// Larger font for status displays / headings (12 pt).
inline HFONT createFontLarge(int dpi) {
    const int px = -::MulDiv(12, dpi, 72);
    return ::CreateFontW(px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
}

// Small font for captions / secondary info (8 pt).
inline HFONT createFontSmall(int dpi) {
    const int px = -::MulDiv(8, dpi, 72);
    return ::CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");
}

// ---- DPI helpers ----

inline int dpiScale(int value, int dpi) {
    return ::MulDiv(value, dpi, 96);
}

// ---- Painting helpers ----

// Fills a rect with a solid color.
inline void fillRect(HDC hdc, const RECT& rc, COLORREF color) {
    HBRUSH br = ::CreateSolidBrush(color);
    ::FillRect(hdc, &rc, br);
    ::DeleteObject(br);
}

// Draws a rounded rect with fill.
inline void drawRoundedRect(HDC hdc, const RECT& rc, int radius, COLORREF fill) {
    HBRUSH br = ::CreateSolidBrush(fill);
    HPEN pen = ::CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBr = ::SelectObject(hdc, br);
    HGDIOBJ oldPen = ::SelectObject(hdc, pen);
    ::RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    ::SelectObject(hdc, oldBr);
    ::SelectObject(hdc, oldPen);
    ::DeleteObject(br);
    ::DeleteObject(pen);
}

// Draws a rounded rect with fill + border.
inline void drawRoundedRectBorder(HDC hdc, const RECT& rc, int radius,
                                  COLORREF fill, COLORREF border) {
    HBRUSH br = ::CreateSolidBrush(fill);
    HPEN pen = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = ::SelectObject(hdc, br);
    HGDIOBJ oldPen = ::SelectObject(hdc, pen);
    ::RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    ::SelectObject(hdc, oldBr);
    ::SelectObject(hdc, oldPen);
    ::DeleteObject(br);
    ::DeleteObject(pen);
}

// Draws centered text in a rect.
inline void drawTextCentered(HDC hdc, const RECT& rc, const wchar_t* text,
                             HFONT font, COLORREF color) {
    HGDIOBJ oldFont = ::SelectObject(hdc, font);
    int oldBk = ::SetBkMode(hdc, TRANSPARENT);
    COLORREF oldClr = ::SetTextColor(hdc, color);
    ::DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SetTextColor(hdc, oldClr);
    ::SetBkMode(hdc, oldBk);
    ::SelectObject(hdc, oldFont);
}

// Draws left-aligned text in a rect.
inline void drawTextLeft(HDC hdc, const RECT& rc, const wchar_t* text,
                         HFONT font, COLORREF color) {
    HGDIOBJ oldFont = ::SelectObject(hdc, font);
    int oldBk = ::SetBkMode(hdc, TRANSPARENT);
    COLORREF oldClr = ::SetTextColor(hdc, color);
    RECT drawRc = rc;
    ::DrawTextW(hdc, text, -1, &drawRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    ::SetTextColor(hdc, oldClr);
    ::SetBkMode(hdc, oldBk);
    ::SelectObject(hdc, oldFont);
}

// ---- Tab metrics ----

inline constexpr int kTabHeight = 36;  // height of the custom tab bar
inline constexpr int kTabPadding = 16; // horizontal padding per tab item
inline constexpr int kTabIconSize = 16;// optional icon size in tab

// ---- Button metrics ----

inline constexpr int kButtonHeight = 32;
inline constexpr int kButtonRadius  = 4;
inline constexpr int kButtonPadding = 12;

} // namespace vw::ui::theme
