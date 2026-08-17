#pragma once

#include <functional>
#include <memory>

#include <windows.h>

#include "app/UiContract.h"
#include "ui/panels/HomePanel.h"
#include "ui/panels/LibraryPanel.h"
#include "ui/panels/MonitorsPanel.h"
#include "ui/panels/PerformancePanel.h"
#include "ui/panels/PlaylistsPanel.h"
#include "ui/panels/SettingsPanel.h"

namespace vw::ui {

// Main UI window (spec §10.1, M11): resizable (default ≈900×600, min
// ≈720×480), PerMonitorV2 aware, a six-tab control (Home · Library ·
// Playlists · Monitors · Performance · Settings), and one Panel per tab.
// Constructed lazily on first open; destroyed on close (the tray persists and
// the engine keeps running). The UI is a thin client: it posts Command and
// receives INotificationSink pushes; it never touches engine state directly.
class Win32UI : public INotificationSink {
public:
    using PostFn = std::function<void(Command)>;
    using RefreshFn = std::function<void()>; // re-pull the playlist (app-side)
    using CloseFn = std::function<void()>;   // app decides hide vs destroy

    Win32UI(PostFn post, RefreshFn refreshPlaylist, LibraryPanel::MetadataRequestFn requestMeta);

    // Lazily creates the window + panels. Returns false only on create failure.
    bool create();
    void show();   // create if needed, show + focus
    void hide();
    void toggle();
    // Destroys the window + panels (engine + tray continue). Idempotent.
    void destroy();
    bool isVisible() const { return visible_; }
    bool exists() const { return hwnd_ != nullptr; }
    // Message boxes / owner windows (spec §10.9: errors surfaced to the user).
    HWND hwnd() const { return hwnd_; }
    void selectTab(int tabIndex);
    void setOnClose(CloseFn fn) { onClose_ = std::move(fn); }

    // Delivers a GRAB_FRAME_SNAPSHOT result (HBITMAP ownership transfers to
    // the Monitors panel, which deletes it on replacement/destroy).
    void showFrameSnapshot(HBITMAP bitmap);

    // Pull-on-open: gives every panel immediate values before the first push.
    void refreshFromSnapshot(const UiSnapshot&);

    // INotificationSink — routes to the owning panels.
    void onTelemetry(const TelemetrySnapshot&) override;
    void onPlaybackState(const PlaybackStateNotification&) override;
    void onMonitorEvent(const MonitorEvent&) override;
    void onLibraryChange(const LibraryChangeNotification&) override;
    void onPlaylistChange(const PlaylistChangeNotification&) override;
    void onWallpaperAssignment(const WallpaperAssignmentNotification&) override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout(int width, int height);
    void showTab(int index);

    PostFn post_;
    RefreshFn refreshPlaylist_;
    CloseFn onClose_;
    HWND hwnd_ = nullptr;
    HWND tabs_ = nullptr;
    bool visible_ = false;
    int currentTab_ = 0;

    std::unique_ptr<HomePanel> home_;
    std::unique_ptr<LibraryPanel> library_;
    std::unique_ptr<PlaylistsPanel> playlists_;
    std::unique_ptr<MonitorsPanel> monitors_;
    std::unique_ptr<PerformancePanel> performance_;
    std::unique_ptr<SettingsPanel> settings_;
};

} // namespace vw::ui
