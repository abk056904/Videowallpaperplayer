#pragma once

#include "ui/panels/Panel.h"

namespace vw::ui {

// Monitors panel (spec §10.5, v1): mode radio (Independent default / Clone),
// live monitor list (name · resolution · refresh · primary), wallpaper source
// combo (playlist or library file), scaling combo (Fill default), a one-time
// frame-snapshot preview, and a "Set as wallpaper" button. Reads arrive from
// MonitorEvent / WallpaperAssignmentNotification + the snapshot; writes are
// commands. On this single-display machine the list has one row and the
// multi-monitor assignment paths are the same commands (per-monitor fan-out
// is exercised by the engine's simulated-topology tests).
class MonitorsPanel : public Panel {
public:
    explicit MonitorsPanel(PostFn post) : Panel(std::move(post)) {}

    bool create(HWND parent) override;
    void refreshFromSnapshot(const UiSnapshot&) override;
    void onMonitorEvent(const MonitorEvent&) override;
    void onWallpaperAssignment(const WallpaperAssignmentNotification&) override;
    void onFrameSnapshot(HBITMAP bitmap) override;
    void relayout() override;
    ~MonitorsPanel() override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout(int width, int height);
    void rebuildMonitors();
    void rebuildSourceCombo(const UiSnapshot&);
    void updateReadback(const WallpaperAssignmentNotification&);
    void postSourceSelection();
    void previewClicked();
    void setWallpaperClicked();
    static std::wstring monitorLabel(const MonitorInfo& m);
    void setPreviewBitmap(HBITMAP bmp);

    enum Btn : UINT { kBtnPreview = 101, kBtnSetWallpaper = 102 };
    enum Ctrl : UINT { kCtlIndependent = 201, kCtlClone = 202, kCtlSourceCombo = 203,
                       kCtlScalingCombo = 204 };

    std::vector<MonitorInfo> monitors_;
    std::vector<LibraryItem> library_; // for the source combo (files)
    bool clone_ = false;
    ScalingMode scaling_ = ScalingMode::Fill;
    WallpaperSource source_ = WallpaperSource::None;
    std::wstring sourceId_;
    HWND list_ = nullptr;
    HWND radioInd_ = nullptr, radioClone_ = nullptr;
    HWND sourceCombo_ = nullptr, scalingCombo_ = nullptr;
    HWND btnPreview_ = nullptr, btnSet_ = nullptr;
    HWND preview_ = nullptr; // static showing the snapshot
    HBITMAP previewBitmap_ = nullptr;
    std::vector<HWND> buttons_;
};

} // namespace vw::ui
