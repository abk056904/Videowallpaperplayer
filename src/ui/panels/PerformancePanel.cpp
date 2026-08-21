#include "ui/panels/PerformancePanel.h"

#include <windows.h>
#include <commctrl.h>

#include <cwchar>
#include <cstdlib>
#include <vector>

namespace vw::ui {

const wchar_t* PerformancePanel::kClassName = L"VideoWallpaper.PerformancePanel";

HWND PerformancePanel::edit(const wchar_t* label, int row, int xCol, const wchar_t* /*key*/) {
    const Layout L = Layout::from(hwnd_);
    const int y = L.y(row);
    ctl(hwnd_, L"STATIC", label, WS_VISIBLE, L.x(xCol), y, ::MulDiv(140, L.u, 5), L.cy, nullptr);
    return ctl(hwnd_, L"EDIT", L"", WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
               L.x(xCol) + ::MulDiv(145, L.u, 5), y, ::MulDiv(50, L.u, 5), L.cy, nullptr);
}

bool PerformancePanel::create(HWND parent) {
    if (hwnd_) {
        return true;
    }
    hwnd_ = createPanelWindow(parent, kClassName, &PerformancePanel::wndProc, this);
    if (!hwnd_) {
        return false;
    }
    initPanelFont();
    const Layout L = Layout::from(hwnd_);

    // ---- Row 0-2: Pause toggles (2 columns, DPI-scaled) ----
    // Col 0: game, CPU, RAM  |  Col 4: fullscreen, GPU
    struct Toggle { UINT id; const wchar_t* text; int row; int col; };
    const Toggle toggles[] = {
        {kCkGame,       L"Pause on game",       0, 0},
        {kCkCpu,        L"Pause on high CPU",    1, 0},
        {kCkRam,        L"Pause on high RAM",    2, 0},
        {kCkFullscreen, L"Pause on fullscreen",  0, 4},
        {kCkGpu,        L"Pause on high GPU",    1, 4},
    };
    for (const auto& t : toggles) {
        checks_[t.id - kCkGame] = ctl(hwnd_, L"BUTTON", t.text,
                                       WS_VISIBLE | BS_AUTOCHECKBOX,
                                       L.x(t.col), L.y(t.row),
                                       ::MulDiv(160, L.u, 5), L.cy,
                                       reinterpret_cast<HMENU>(static_cast<UINT_PTR>(t.id)));
    }

    // ---- Row 3: CPU threshold pair ----
    // Left:  "CPU threshold :" [pause val]
    // Right: "CPU resume    :" [resume val]
    edits_[0] = edit(L"CPU threshold :", 3, 0, L"cpuPauseThreshold");
    edits_[1] = edit(L"CPU resume :", 3, 2, L"cpuResumeThreshold");

    // ---- Row 4: GPU threshold pair ----
    edits_[2] = edit(L"GPU threshold :", 4, 0, L"gpuPauseThreshold");
    edits_[3] = edit(L"GPU resume :", 4, 2, L"gpuResumeThreshold");

    // ---- Row 5: RAM threshold pair ----
    edits_[4] = edit(L"RAM threshold :", 5, 0, L"memoryPauseThreshold");
    edits_[5] = edit(L"RAM resume :", 5, 2, L"memoryResumeThreshold");

    // ---- Row 6: Delays ----
    edits_[6] = edit(L"Pause delay (s) :", 6, 0, L"pauseDelaySeconds");
    edits_[7] = edit(L"Resume delay (s) :", 6, 2, L"resumeDelaySeconds");

    // ---- Row 7: Battery mode ----
    ctl(hwnd_, L"STATIC", L"Battery mode :", WS_VISIBLE, L.x(0), L.y(7),
        ::MulDiv(140, L.u, 5), L.cy, nullptr);
    batteryCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                        L.x(0) + ::MulDiv(145, L.u, 5), L.y(7), ::MulDiv(120, L.u, 5), 200,
                        reinterpret_cast<HMENU>(kCmBattery));
    for (const wchar_t* b : {L"Continue", L"Reduce quality", L"Pause"}) {
        ::SendMessageW(batteryCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b));
    }

    // ---- Row 8: Advanced Performance button (DPI-scaled) ----
    btnAdvanced_ = ctl(hwnd_, L"BUTTON", L"Advanced Performance", WS_VISIBLE | BS_PUSHBUTTON,
                       L.x(0), L.y(8), ::MulDiv(160, L.u, 5), L.cy,
                       reinterpret_cast<HMENU>(kBtnAdvanced));

    // ---- Row 9+: Advanced section (initially hidden) ----
    const int advRow = 9;
    // Perf mode label + combo — labels are ALWAYS visible, combo hidden when collapsed
    ctl(hwnd_, L"STATIC", L"Mode :", WS_VISIBLE, L.x(0), L.y(advRow),
        ::MulDiv(140, L.u, 5), L.cy, nullptr);
    perfModeCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                         L.x(0) + ::MulDiv(145, L.u, 5), L.y(advRow), ::MulDiv(160, L.u, 5), 200,
                         reinterpret_cast<HMENU>(kCmPerfMode));
    for (const wchar_t* p : {L"Performance", L"Balanced", L"Quality", L"Ultra Low Resource"}) {
        ::SendMessageW(perfModeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p));
    }

    frameQueueEdit_ = edit(L"Frame queue :", advRow + 1, 0, L"frameQueue");
    longPauseEdit_ = edit(L"Long-pause release (s) :", advRow + 2, 0, L"longPauseReleaseSeconds");

    // Advanced section starts collapsed — hide all advanced controls.
    advancedOpen_ = false;
    {
        const Layout L2 = Layout::from(hwnd_);
        auto hideRow = [&](int row) {
            const int targetY = L2.y(row);
            HWND child = ::GetWindow(hwnd_, GW_CHILD);
            while (child) {
                RECT rc;
                ::GetWindowRect(child, &rc);
                ::MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<LPPOINT>(&rc), 2);
                if (std::abs(rc.top - targetY) < L2.cy / 2) {
                    ::ShowWindow(child, SW_HIDE);
                }
                child = ::GetWindow(child, GW_HWNDNEXT);
            }
        };
        hideRow(advRow);      // "Mode :" label + combo
        hideRow(advRow + 1);  // "Frame queue :" label + edit
        hideRow(advRow + 2);  // "Long-pause release" label + edit
    }

    return true;
}

void PerformancePanel::relayout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    layout(rc.right - rc.left, rc.bottom - rc.top);
}

void PerformancePanel::layout(int /*width*/, int /*height*/) {
    // Controls are positioned at create() from the panel font; nothing needs
    // re-flowing on resize (the panel content fits the minimum window size).
}

void PerformancePanel::updateFromConfig(const ConfigSnapshot& c) {
    cfg_ = c;
    ::SendMessageW(checks_[0], BM_SETCHECK, c.pauseOnGame ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(checks_[1], BM_SETCHECK, c.pauseOnFullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(checks_[2], BM_SETCHECK, c.pauseOnHighCPU ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(checks_[3], BM_SETCHECK, c.pauseOnHighGPU ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(checks_[4], BM_SETCHECK, c.pauseOnHighRAM ? BST_CHECKED : BST_UNCHECKED, 0);

    const int values[] = {c.cpuPauseThreshold, c.cpuResumeThreshold, c.gpuPauseThreshold,
                          c.gpuResumeThreshold, c.memoryPauseThreshold, c.memoryResumeThreshold,
                          c.pauseDelaySeconds, c.resumeDelaySeconds};
    wchar_t buf[16];
    for (int i = 0; i < 8; ++i) {
        std::swprintf(buf, 16, L"%d", values[i]);
        ::SetWindowTextW(edits_[i], buf);
    }

    int b = 0;
    switch (c.batteryMode) {
        case BatteryMode::Continue: b = 0; break;
        case BatteryMode::ReduceQuality: b = 1; break;
        case BatteryMode::Pause: b = 2; break;
    }
    ::SendMessageW(batteryCombo_, CB_SETCURSEL, b, 0);

    int p = 0; // perf mode not in ConfigSnapshot; leave combo at Balanced
    (void)p;
    std::swprintf(buf, 16, L"%d", c.frameQueue);
    ::SetWindowTextW(frameQueueEdit_, buf);
    std::swprintf(buf, 16, L"%d", c.longPauseReleaseSeconds);
    ::SetWindowTextW(longPauseEdit_, buf);
}

void PerformancePanel::refreshFromSnapshot(const UiSnapshot& s) {
    updateFromConfig(s.config);
}

void PerformancePanel::postBool(const wchar_t* key, bool on) {
    Command c;
    c.id = CommandId::ConfigSet;
    c.s1 = key;
    c.s2 = on ? L"true" : L"false";
    post_(c);
}

void PerformancePanel::postInt(const wchar_t* key, int value) {
    Command c;
    c.id = CommandId::ConfigSet;
    c.s1 = key;
    c.s2 = std::to_wstring(value);
    post_(c);
}

void PerformancePanel::postEnum(const wchar_t* key, const wchar_t* value) {
    Command c;
    c.id = CommandId::ConfigSet;
    c.s1 = key;
    c.s2 = value;
    post_(c);
}

void PerformancePanel::readEdits() {
    auto getInt = [&](HWND h, int def) {
        wchar_t buf[32];
        ::GetWindowTextW(h, buf, 32);
        try {
            return std::stoi(buf);
        } catch (...) {
            return def;
        }
    };
    postInt(L"cpuPauseThreshold", getInt(edits_[0], cfg_.cpuPauseThreshold));
    postInt(L"cpuResumeThreshold", getInt(edits_[1], cfg_.cpuResumeThreshold));
    postInt(L"gpuPauseThreshold", getInt(edits_[2], cfg_.gpuPauseThreshold));
    postInt(L"gpuResumeThreshold", getInt(edits_[3], cfg_.gpuResumeThreshold));
    postInt(L"memoryPauseThreshold", getInt(edits_[4], cfg_.memoryPauseThreshold));
    postInt(L"memoryResumeThreshold", getInt(edits_[5], cfg_.memoryResumeThreshold));
    postInt(L"pauseDelaySeconds", getInt(edits_[6], cfg_.pauseDelaySeconds));
    postInt(L"resumeDelaySeconds", getInt(edits_[7], cfg_.resumeDelaySeconds));
    postInt(L"frameQueue", getInt(frameQueueEdit_, cfg_.frameQueue));
    postInt(L"longPauseReleaseSeconds", getInt(longPauseEdit_, cfg_.longPauseReleaseSeconds));
}

void PerformancePanel::toggleAdvanced() {
    advancedOpen_ = !advancedOpen_;
    ::SetWindowTextW(btnAdvanced_, advancedOpen_ ? L"Advanced Performance (hide)"
                                                 : L"Advanced Performance");

    const int advRow = 9;
    const Layout L = Layout::from(hwnd_);

    // Helper: show/hide all child controls at a given grid row.
    auto toggleRow = [&](int row, bool show) {
        const int targetY = L.y(row);
        HWND child = ::GetWindow(hwnd_, GW_CHILD);
        while (child) {
            RECT rc;
            ::GetWindowRect(child, &rc);
            ::MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<LPPOINT>(&rc), 2);
            if (std::abs(rc.top - targetY) < L.cy / 2) {
                ::ShowWindow(child, show ? SW_SHOW : SW_HIDE);
            }
            child = ::GetWindow(child, GW_HWNDNEXT);
        }
    };

    // Toggle Mode label + combo (row 9), frame queue (row 10), long-pause (row 11)
    toggleRow(advRow, advancedOpen_);
    toggleRow(advRow + 1, advancedOpen_);
    toggleRow(advRow + 2, advancedOpen_);
}

LRESULT CALLBACK PerformancePanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PerformancePanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PerformancePanel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_COMMAND: {
            const UINT id = LOWORD(wParam);
            const UINT code = HIWORD(wParam);
            switch (id) {
                case kCkGame:
                    if (code == BN_CLICKED) {
                        self->postBool(L"pauseOnGame",
                                       ::SendMessageW(self->checks_[0], BM_GETCHECK, 0, 0) ==
                                           BST_CHECKED);
                    }
                    return 0;
                case kCkFullscreen:
                    if (code == BN_CLICKED) {
                        self->postBool(L"pauseOnFullscreen",
                                       ::SendMessageW(self->checks_[1], BM_GETCHECK, 0, 0) ==
                                           BST_CHECKED);
                    }
                    return 0;
                case kCkCpu:
                    if (code == BN_CLICKED) {
                        self->postBool(L"pauseOnHighCPU",
                                       ::SendMessageW(self->checks_[2], BM_GETCHECK, 0, 0) ==
                                           BST_CHECKED);
                    }
                    return 0;
                case kCkGpu:
                    if (code == BN_CLICKED) {
                        self->postBool(L"pauseOnHighGPU",
                                       ::SendMessageW(self->checks_[3], BM_GETCHECK, 0, 0) ==
                                           BST_CHECKED);
                    }
                    return 0;
                case kCkRam:
                    if (code == BN_CLICKED) {
                        self->postBool(L"pauseOnHighRAM",
                                       ::SendMessageW(self->checks_[4], BM_GETCHECK, 0, 0) ==
                                           BST_CHECKED);
                    }
                    return 0;
                case kCmBattery:
                    if (code == CBN_SELCHANGE) {
                        const int b = static_cast<int>(
                            ::SendMessageW(self->batteryCombo_, CB_GETCURSEL, 0, 0));
                        self->postEnum(L"batteryMode",
                                       b == 0 ? L"continue" : (b == 1 ? L"reduce" : L"pause"));
                    }
                    return 0;
                case kCmPerfMode:
                    if (code == CBN_SELCHANGE) {
                        const int p = static_cast<int>(
                            ::SendMessageW(self->perfModeCombo_, CB_GETCURSEL, 0, 0));
                        self->postEnum(L"perfMode",
                                       p == 0 ? L"performance"
                                              : (p == 1 ? L"balanced"
                                                        : (p == 2 ? L"quality" : L"ultra-low-resource")));
                    }
                    return 0;
                case kBtnAdvanced:
                    self->toggleAdvanced();
                    return 0;
                default:
                    // Any edit losing focus applies ALL edit fields at once
                    // (single CONFIG_SET burst, debounced engine-side).
                    if (code == EN_KILLFOCUS &&
                        id >= kEdCpuPause && id <= kEdResumeDelay) {
                        self->readEdits();
                    }
                    if (code == EN_KILLFOCUS && (id == kEdFrameQueue || id == kEdLongPause)) {
                        self->readEdits();
                    }
                    return 0;
            }
        }
        case WM_SIZE: {
            self->layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
