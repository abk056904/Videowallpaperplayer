#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <windows.h>

#include "app/UiContract.h"
#include "ui/panels/HomePanel.h"
#include "ui/panels/LibraryPanel.h"
#include "ui/panels/MonitorsPanel.h"
#include "ui/panels/PerformancePanel.h"
#include "ui/panels/PlaylistsPanel.h"
#include "ui/panels/SettingsPanel.h"

namespace vw::ui {

// Main UI window (spec §10.1, M11): dark-mode themed, resizable (default ≈900×600,
// min ≈720×480), PerMonitorV2 aware, a custom six-tab bar (Home · Library ·
// Playlists · Monitors · Performance · Settings), and one Panel per tab.
// Constructed lazily on first open; destroyed on close (the tray persists and
// the engine keeps running). The UI is a thin client: it posts Command and
// receives INotificationSink pushes; it never touches engine state directly.
class Win32UI : public INotificationSink {
public:
    using PostFn = std::function<void(Command)>;
    using RefreshFn = std::function<void()>;
    using CloseFn = std::function<void()>;

    Win32UI(PostFn post, RefreshFn refreshPlaylist, LibraryPanel::MetadataRequestFn requestMeta);

    bool create();
    void show();
    void hide();
    void toggle();
    void destroy();
    bool isVisible() const { return visible_; }
    bool exists() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }
    void selectTab(int tabIndex);
    void setOnClose(CloseFn fn) { onClose_ = std::move(fn); }

    void showFrameSnapshot(HBITMAP bitmap);
    void refreshFromSnapshot(const UiSnapshot&);

    // INotificationSink
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

    // Custom tab bar painting
    void paintTabBar(HDC hdc, int width);
    int tabHitTest(int x, int y) const;
    RECT tabRect(int index) const;

    PostFn post_;
    RefreshFn refreshPlaylist_;
    CloseFn onClose_;
    HWND hwnd_ = nullptr;
    bool visible_ = false;
    int currentTab_ = 0;
    int hoverTab_ = -1;

    // Fonts
    HFONT fontTab_ = nullptr;
    HFONT fontPanel_ = nullptr;

    // Tab names
    static constexpr int kTabCount = 6;
    static const wchar_t* kTabNames[kTabCount];

    // Panels
    std::unique_ptr<HomePanel> home_;
    std::unique_ptr<LibraryPanel> library_;
    std::unique_ptr<PlaylistsPanel> playlists_;
    std::unique_ptr<MonitorsPanel> monitors_;
    std::unique_ptr<PerformancePanel> performance_;
    std::unique_ptr<SettingsPanel> settings_;
};

} // namespace vw::ui
