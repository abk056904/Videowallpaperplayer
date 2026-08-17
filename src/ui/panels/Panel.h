#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "app/UiContract.h"

namespace vw::ui {

// Base for the six tab panels (spec §10.2–10.7, M11). THIN-CLIENT rule: a
// panel never touches engine state — it posts Command via `post_` and reads
// through push notifications (onTelemetry/onPlaybackState/…) plus the
// pull-on-open snapshot (refreshFromSnapshot). Panels own their controls and
// destroy them with their window; repeated UI open/close must not leak
// (M11 leak gate).
class Panel {
public:
    using PostFn = std::function<void(Command)>;

    explicit Panel(PostFn post) : post_(std::move(post)) {}
    virtual ~Panel() = default;

    // Creates the panel window (WS_CHILD) + its controls. Idempotent.
    virtual bool create(HWND parent) = 0;

    HWND handle() const { return hwnd_; }
    void show() {
        if (hwnd_) {
            ::ShowWindow(hwnd_, SW_SHOW);
        }
    }
    void hide() {
        if (hwnd_) {
            ::ShowWindow(hwnd_, SW_HIDE);
        }
    }

    // Push notifications (Win32UI routes them to every panel; panels update
    // only the fields they own).
    virtual void onTelemetry(const TelemetrySnapshot&) {}
    virtual void onPlaybackState(const PlaybackStateNotification&) {}
    virtual void onMonitorEvent(const MonitorEvent&) {}
    virtual void onLibraryChange(const LibraryChangeNotification&) {}
    virtual void onPlaylistChange(const PlaylistChangeNotification&) {}
    virtual void onWallpaperAssignment(const WallpaperAssignmentNotification&) {}
    // One-time pull on window open (values before the first push).
    virtual void refreshFromSnapshot(const UiSnapshot&) {}
    // Delivered GRAB_FRAME_SNAPSHOT result (Monitors preview).
    virtual void onFrameSnapshot(HBITMAP /*bitmap, owned by the panel*/) {}
    // Re-runs the layout (WM_DPICHANGED / initial sizing).
    virtual void relayout() {}

protected:
    // ---- DPI-aware layout helpers ----
    struct Layout {
        int u = 0;  // base grid unit (≈5 px at 96 DPI)
        int cy = 0; // standard control height

        static Layout from(HWND hwnd);
        int x(int col) const { return 8 + col * 4 * u; }
        int y(int row) const { return 8 + row * (cy + u); }
    };

    // Creates the panel font from the window DPI (call after hwnd_ exists).
    void initPanelFont();

    // Creates a control with the panel font. `style` is the WS_CHILD base;
    // returns nullptr on failure.
    HWND ctl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
             int x, int y, int w, int h, HMENU id);
    void setFont(HWND control);

    PostFn post_;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
};

// Shared panel window-class helper: registers `cls` with `wndProc` once and
// creates a WS_CHILD window (the panel object rides in GWLP_USERDATA).
HWND createPanelWindow(HWND parent, const wchar_t* cls, WNDPROC wndProc, void* userData);

// A horizontally-stacked button bar row (used by several panels).
void makeButtons(HWND parent, HFONT font, const std::vector<std::pair<UINT, std::wstring>>& items,
                 std::vector<HWND>& out, int y, int u, int cy);

// Formats helpers shared by panels.
std::wstring formatDuration(double seconds);
std::wstring formatFps(double fps);

} // namespace vw::ui
