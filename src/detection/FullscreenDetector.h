#pragma once

#include <windows.h>

#include <cwchar>

// Fullscreen detection (docs/02 §2.9 / docs/03 §3.11, M9): classifies the
// FOREGROUND window as true fullscreen / borderless fullscreen / maximized /
// windowed — maximized is deliberately NOT fullscreen unless configured.
//
// The classification itself is a PURE function of (window rect, monitor rect,
// window styles) so it is unit-testable without a real display. The detector
// caches per window handle and is fed by WinEventHook foreground-change events
// (SetWinEventHook — no polling) + WM_DISPLAYCHANGE, wired by the app.
//
// Rule (docs/03 §3.11): the window covers the ENTIRE monitor bounds AND is
// either WS_POPUP (true exclusive fullscreen) or borderless (no caption /
// resize borders). A maximized WS_OVERLAPPEDWINDOW also covers the monitor —
// but it keeps its caption frame, so it classifies as Maximized, not
// Fullscreen. The app decides whether Maximized counts (config
// `pauseOnFullscreen` semantics; default: only true fullscreen pauses).

namespace vw::detection {

enum class WindowState { Windowed, Maximized, Fullscreen, BorderlessFullscreen };

// Pure classification (M9). `windowRect` = the window's rect; `monitorRect` =
// the monitor it sits on (work area is NOT used — fullscreen covers the whole
// monitor, taskbar included). `style`/`exStyle` = GetWindowLongPtr values.
// Returns Maximized for a maximized overlapped window even though its rect
// covers the monitor — a maximized editor must NOT classify as fullscreen.
inline WindowState classifyWindowState(const RECT& windowRect, const RECT& monitorRect,
                                       LONG_PTR style, LONG_PTR exStyle) {
    (void)exStyle; // reserved (e.g. WS_EX_TOPMOST check) — keep the call-site shape
    const LONG ww = windowRect.right - windowRect.left;
    const LONG wh = windowRect.bottom - windowRect.top;
    const LONG mw = monitorRect.right - monitorRect.left;
    const LONG mh = monitorRect.bottom - monitorRect.top;
    const bool covers = ww >= mw && wh >= mh;

    const bool popup = (style & WS_POPUP) != 0;
    const bool hasCaption = (style & WS_CAPTION) != 0;
    const bool hasThickFrame = (style & WS_THICKFRAME) != 0;
    const bool maximized = (style & WS_MAXIMIZE) != 0;

    if (!covers) {
        return WindowState::Windowed;
    }
    if (popup) {
        return WindowState::Fullscreen; // exclusive (games, media players)
    }
    // Borderless fullscreen: covers the monitor but has no caption frame.
    if (!hasCaption && !hasThickFrame) {
        return WindowState::BorderlessFullscreen;
    }
    // Maximized OVERLAPPED window covers the monitor with its frame intact.
    if (maximized || hasCaption || hasThickFrame) {
        return WindowState::Maximized;
    }
    return WindowState::Windowed;
}

inline bool isFullscreenState(WindowState s) {
    return s == WindowState::Fullscreen || s == WindowState::BorderlessFullscreen;
}

// Desktop/shell layer windows (Progman, its WorkerW layers, SHELLDLL_DefView)
// must NEVER classify as fullscreen: they cover the monitor with WS_POPUP
// style, and clicking the desktop makes one of them the foreground window — a
// naive classification would pause the wallpaper on every desktop click.
// Pure predicate on the class name (unit-testable; the app feeds it the
// foreground window's class from classifyForegroundFullscreen).
inline bool isDesktopShellClass(const wchar_t* className) {
    return className && (std::wcscmp(className, L"Progman") == 0 ||
                         std::wcscmp(className, L"WorkerW") == 0 ||
                         std::wcscmp(className, L"SHELLDLL_DefView") == 0);
}

} // namespace vw::detection
