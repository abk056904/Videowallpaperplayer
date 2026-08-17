#include "app/ControlWindow.h"

#include "app/resource.h"

namespace vw::app {

const wchar_t* ControlWindow::kClassName = L"VideoWallpaper.ControlWindow";

UINT ControlWindow::focusMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"VideoWallpaper.FocusMessage");
    return msg;
}

UINT ControlWindow::pauseMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"VideoWallpaper.PlaybackPause");
    return msg;
}

UINT ControlWindow::resumeMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"VideoWallpaper.PlaybackResume");
    return msg;
}

UINT ControlWindow::stopMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"VideoWallpaper.PlaybackStop");
    return msg;
}

ControlWindow::~ControlWindow() {
    destroy();
}

void ControlWindow::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool ControlWindow::create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ControlWindow::wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // App icon from the embedded resource (src/app/app.rc).
    wc.hIcon = ::LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = wc.hIcon;
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, // no taskbar entry, no activation
        kClassName, L"Video Wallpaper", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    return hwnd_ != nullptr;
}

void ControlWindow::requestShutdown() {
    if (hwnd_) ::PostMessageW(hwnd_, WM_APP, 0, 0);
}

LRESULT CALLBACK ControlWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ControlWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ControlWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ControlWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        if (msg == WM_APP) { // shutdown request
            ::PostQuitMessage(0);
            return 0;
        }
        if (self->handler_) {
            self->handler_(msg, wParam, lParam);
        }
    }

    if (msg == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::app
