#pragma once

#include <functional>
#include <windows.h>

namespace vw::app {

// Hidden control window (docs/01 §6): receives display/session/power
// notifications and app messages. No taskbar presence, never visible.
// Future milestones hook WM_DISPLAYCHANGE / WM_DEVICECHANGE / WM_POWERBROADCAST
// / WM_WTSSESSION_CHANGE here.
class ControlWindow {
public:
    using Handler = std::function<void(UINT, WPARAM, LPARAM)>;

    ControlWindow() = default;
    ~ControlWindow();

    ControlWindow(const ControlWindow&) = delete;
    ControlWindow& operator=(const ControlWindow&) = delete;

    bool create();
    HWND handle() const { return hwnd_; }

    // Idempotent: destroys the window if present (deterministic shutdown).
    void destroy();

    // Post a quit to the message loop (deterministic shutdown).
    void requestShutdown();

    // Callbacks from the message loop for app messages.
    void setHandler(Handler h) { handler_ = std::move(h); }

    static const wchar_t* kClassName;
    // Registered message: a second instance asks us to focus.
    static UINT focusMessage();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    Handler handler_;
};

} // namespace vw::app
