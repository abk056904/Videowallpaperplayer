#pragma once

#include "ui/panels/Panel.h"

namespace vw::ui {

// Performance panel (spec §10.6): pause toggles (5), threshold pairs with
// pause + resume values (cpu/gpu/ram), pause/resume delays, battery mode, and
// a collapsible "Advanced Performance" section (perf mode, frame-queue depth,
// long-pause release). Every change posts CONFIG_SET — validated/clamped
// engine-side, debounced save, live effect. Values are read once from the
// snapshot on open (spec: writes only, no live subscription).
class PerformancePanel : public Panel {
public:
    explicit PerformancePanel(PostFn post) : Panel(std::move(post)) {}

    bool create(HWND parent) override;
    void refreshFromSnapshot(const UiSnapshot&) override;
    void relayout() override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout(int width, int height);
    void updateFromConfig(const ConfigSnapshot&);
    void postBool(const wchar_t* key, bool on);
    void postInt(const wchar_t* key, int value);
    void postEnum(const wchar_t* key, const wchar_t* value);
    void readEdits(); // applied on focus-loss of any edit
    void toggleAdvanced();
    HWND edit(const wchar_t* label, int row, int xCol, const wchar_t* key);

    enum Ctrl : UINT {
        kCkGame = 301, kCkFullscreen, kCkCpu, kCkGpu, kCkRam,
        kEdCpuPause, kEdCpuResume, kEdGpuPause, kEdGpuResume, kEdRamPause, kEdRamResume,
        kEdPauseDelay, kEdResumeDelay,
        kCmBattery = 401, kCmPerfMode = 402, kEdFrameQueue = 403, kEdLongPause = 404,
        kBtnAdvanced = 501,
    };

    ConfigSnapshot cfg_;
    HWND checks_[5] = {};
    HWND edits_[12] = {};
    HWND batteryCombo_ = nullptr, perfModeCombo_ = nullptr;
    HWND frameQueueEdit_ = nullptr, longPauseEdit_ = nullptr;
    HWND btnAdvanced_ = nullptr;
    bool advancedOpen_ = false;
};

} // namespace vw::ui
