#include "ui/TrayController.h"

#include <shellapi.h>

#include "app/resource.h"

namespace vw::ui {

UINT TrayController::callbackMessage() {
    static const UINT msg = ::RegisterWindowMessageW(L"VideoWallpaper.TrayCallback");
    return msg;
}

void TrayController::create(HWND notificationWindow) {
    if (added_) {
        return;
    }
    window_ = notificationWindow;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = callbackMessage();
    nid.hIcon = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    // Tooltip is capped at 128 chars by the shell.
    const std::wstring tip = tooltip_.substr(0, 127);
    ::lstrcpynW(nid.szTip, tip.c_str(), static_cast<int>(tip.size() + 1));
    added_ = ::Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!added_) {
        return;
    }
    // NOTIFYICON_VERSION_4 gives NIN_SELECT on left-click (reliable toggle).
    nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayController::destroy() {
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window_;
    nid.uID = 1;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void TrayController::setTooltip(const std::wstring& text) {
    tooltip_ = text;
    if (!added_ || !window_) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = window_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    const std::wstring tip = tooltip_.substr(0, 127);
    ::lstrcpynW(nid.szTip, tip.c_str(), static_cast<int>(tip.size() + 1));
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayController::setCurrentVideo(const std::wstring& name) {
    currentVideo_ = name;
}

TrayController::MenuId TrayController::showMenu() {
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, kMenuResume, L"Resume");
    ::AppendMenuW(menu, MF_STRING, kMenuPause, L"Pause");
    ::AppendMenuW(menu, MF_STRING, kMenuNext, L"Next");
    ::AppendMenuW(menu, MF_STRING, kMenuPrevious, L"Previous");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const std::wstring current =
        currentVideo_.empty() ? L"Current wallpaper: (none)" : L"Current wallpaper: " + currentVideo_;
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, kMenuCurrent, current.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Video Wallpaper");
    ::AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT pt{};
    ::GetCursorPos(&pt);
    ::SetForegroundWindow(window_); // required for the menu to dismiss properly
    const UINT_PTR chosen = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                             pt.x, pt.y, 0, window_, nullptr);
    ::DestroyMenu(menu);
    if (chosen == 0) {
        return kMenuNone;
    }
    return static_cast<MenuId>(chosen);
}

} // namespace vw::ui
