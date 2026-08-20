#pragma once

#include "ui/panels/Panel.h"

namespace vw::ui {

// Home panel (spec §10.2): status readouts refreshed at 1–2 Hz from the
// telemetry + playback-state pushes, plus Pause/Resume/Next/Previous buttons
// that post commands. Modern card-based layout with dark theme.
class HomePanel : public Panel {
public:
    explicit HomePanel(PostFn post) : Panel(std::move(post)) {}

    bool create(HWND parent) override;
    void onTelemetry(const TelemetrySnapshot&) override;
    void onPlaybackState(const PlaybackStateNotification&) override;
    void relayout() override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout(int width);
    void updateRows();

    enum Row : int { kRowWallpaper = 0, kRowState, kRowMonitor, kRowFps, kRowDecoder,
                     kRowAdapter, kRowWorkload, kRowLatency, kRowCount };
    enum Btn : UINT { kBtnPause = 101, kBtnNext = 102, kBtnPrev = 103 };

    HWND rows_[kRowCount] = {};
    HWND btnPause_ = nullptr, btnNext_ = nullptr, btnPrev_ = nullptr;
    TelemetrySnapshot telemetry_;
    PlaybackStateNotification state_;
};

} // namespace vw::ui
