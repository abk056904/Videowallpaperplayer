#include "ui/panels/Panel.h"

#include <cstdio>
#include <cwchar>

namespace vw::ui {

Panel::Layout Panel::Layout::from(HWND hwnd) {
    Layout out;
    const UINT dpi = ::GetDpiForWindow(hwnd);
    out.u = ::MulDiv(5, dpi, 96);
    out.cy = ::MulDiv(24, dpi, 96);
    return out;
}

void Panel::initPanelFont() {
    dpi_ = static_cast<int>(::GetDpiForWindow(hwnd_));
    // Segoe UI 9pt scaled to the monitor DPI (per-monitor DPI aware app).
    const int px = -::MulDiv(9, dpi_, 72);
    font_ = ::CreateFontW(px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

HWND Panel::ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                int w, int h, HMENU id) {
    HWND ctlHwnd = ::CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                     parent, id, ::GetModuleHandleW(nullptr), nullptr);
    if (ctlHwnd) {
        setFont(ctlHwnd);
    }
    return ctlHwnd;
}

void Panel::setFont(HWND control) {
    if (font_) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

HWND createPanelWindow(HWND parent, const wchar_t* cls, WNDPROC wndProc, void* userData) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    ::RegisterClassExW(&wc);
    return ::CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr,
                             ::GetModuleHandleW(nullptr), userData);
}

void makeButtons(HWND parent, HFONT font,
                 const std::vector<std::pair<UINT, std::wstring>>& items, std::vector<HWND>& out,
                 int y, int u, int cy) {
    int x = 8;
    const int bw = ::MulDiv(110, u, 5);
    for (const auto& [id, text] : items) {
        HWND b = ::CreateWindowExW(0, L"BUTTON", text.c_str(),
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, y, bw, cy, parent,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),
                                   ::GetModuleHandleW(nullptr), nullptr);
        if (b) {
            ::SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        out.push_back(b);
        x += bw + u;
    }
}

std::wstring formatDuration(double seconds) {
    if (seconds <= 0) {
        return L"--:--";
    }
    const int total = static_cast<int>(seconds + 0.5);
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%d:%02d", total / 60, total % 60);
    return buf;
}

std::wstring formatFps(double fps) {
    if (fps <= 0) {
        return L"-";
    }
    wchar_t buf[16];
    std::swprintf(buf, 16, L"%.0f", fps);
    return buf;
}

} // namespace vw::ui
