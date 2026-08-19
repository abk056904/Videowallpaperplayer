#pragma once

#include <windows.h>

#include <string>

// System tray integration (docs/02 §2.x, spec §10.8, M11). Holds ONLY the
// icon + menu resources — never keeps the UI alive. Menu clicks are returned
// to the caller (showMenu returns the chosen MenuId); the app maps them to
// commands. The tray's mouse events arrive as the registered callback message
// on the notification window; the app routes WM_CONTEXTMENU/NIN_SELECT there.
//
// Tooltip reflects the current playback state (setTooltip); the read-only
// "Current wallpaper" menu item is updated via setCurrentVideo before showing
// the menu.
namespace vw::ui {

class TrayController {
public:
    enum MenuId : UINT_PTR {
        kMenuNone = 0,
        kMenuResume,
        kMenuPause,
        kMenuNext,
        kMenuPrevious,
        kMenuCurrent, // read-only info item
        kMenuSpeed05,
        kMenuSpeed10,
        kMenuSpeed15,
        kMenuSpeed20,
        kMenuOpen,
        kMenuSettings,
        kMenuExit,
    };

    // Registered callback message (lParam = mouse event; WM_CONTEXTMENU and
    // NIN_SELECT are the interesting ones).
    static UINT callbackMessage();

    // Adds the icon (embedded app icon). Idempotent.
    void create(HWND notificationWindow);
    void destroy(); // removes the icon (idempotent; safe from shutdown)
    bool visible() const { return added_; }

    void setTooltip(const std::wstring& text);      // e.g. L"Video Wallpaper — Playing"
    void setCurrentVideo(const std::wstring& name); // read-only menu item text
    void setCurrentSpeed(double speed); // speed submenu checkmark

    // Shows the context menu at the cursor. Returns the chosen MenuId
    // (kMenuNone when dismissed). Blocks while the menu is up.
    MenuId showMenu();

private:
    HWND window_ = nullptr;
    bool added_ = false;
    std::wstring tooltip_ = L"Video Wallpaper";
    std::wstring currentVideo_; // empty = "no wallpaper"
    double currentSpeed_ = 1.0; // for submenu checkmark
};

} // namespace vw::ui
