#include "ui/panels/SettingsPanel.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <cstdio>
#include <filesystem>

#include "logging/Logger.h"

namespace vw::ui {

const wchar_t* SettingsPanel::kClassName = L"VideoWallpaper.SettingsPanel";

bool SettingsPanel::create(HWND parent) {
    if (hwnd_) {
        return true;
    }
    hwnd_ = createPanelWindow(parent, kClassName, &SettingsPanel::wndProc, this);
    if (!hwnd_) {
        return false;
    }
    initPanelFont();
    const Layout L = Layout::from(hwnd_);

    checkStartup_ = ctl(hwnd_, L"BUTTON", L"Start with Windows", WS_VISIBLE | BS_AUTOCHECKBOX,
                        L.x(0), L.y(0), ::MulDiv(200, L.u, 5), L.cy,
                        reinterpret_cast<HMENU>(kCkStartup));
    checkTray_ = ctl(hwnd_, L"BUTTON", L"Minimize to tray on close", WS_VISIBLE | BS_AUTOCHECKBOX,
                     L.x(0), L.y(1), ::MulDiv(220, L.u, 5), L.cy,
                     reinterpret_cast<HMENU>(kCkTray));

    ctl(hwnd_, L"STATIC", L"Logging level :", WS_VISIBLE, L.x(0), L.y(2),
        ::MulDiv(140, L.u, 5), L.cy, nullptr);
    logCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                    L.x(0) + ::MulDiv(145, L.u, 5), L.y(2), ::MulDiv(120, L.u, 5), 200,
                    reinterpret_cast<HMENU>(kCmLogLevel));
    for (const wchar_t* l : {L"INFO", L"DEBUG", L"WARN", L"ERROR"}) {
        ::SendMessageW(logCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(l));
    }

    aboutText_ = ctl(hwnd_, L"STATIC", L"", WS_VISIBLE, L.x(0), L.y(4), ::MulDiv(420, L.u, 5),
                     3 * L.cy, nullptr);

    // Playback speed selector (spec §23: Playback speed).
    ctl(hwnd_, L"STATIC", L"Playback speed :", WS_VISIBLE, L.x(0), L.y(7),
        ::MulDiv(140, L.u, 5), L.cy, nullptr);
    speedCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                      L.x(0) + ::MulDiv(145, L.u, 5), L.y(7), ::MulDiv(80, L.u, 5), 200,
                      reinterpret_cast<HMENU>(kCmSpeed));
    for (const wchar_t* s : {L"0.5x", L"0.75x", L"1.0x", L"1.5x", L"2.0x"}) {
        ::SendMessageW(speedCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }

    btnReadme_ = ctl(hwnd_, L"BUTTON", L"Open README", WS_VISIBLE | BS_PUSHBUTTON,
                     L.x(0), L.y(9), ::MulDiv(120, L.u, 5), L.cy,
                     reinterpret_cast<HMENU>(kBtnReadme));
    return true;
}

void SettingsPanel::relayout() {
    // Fixed layout from the panel font; nothing to re-flow.
}

void SettingsPanel::updateFromConfig(const ConfigSnapshot& c) {
    ::SendMessageW(checkStartup_, BM_SETCHECK, c.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(checkTray_, BM_SETCHECK, c.minimizeToTray ? BST_CHECKED : BST_UNCHECKED, 0);
    int idx = 0;
    if (c.logLevel == L"debug") idx = 1;
    else if (c.logLevel == L"warn") idx = 2;
    else if (c.logLevel == L"error") idx = 3;
    ::SendMessageW(logCombo_, CB_SETCURSEL, idx, 0);

    wchar_t buf[512];
    std::swprintf(buf, 512, L"Video Wallpaper v0.1.0%s\nMSVC 14.44 - Windows SDK 10.0.26100 - x64",
#ifdef VW_DEBUG
                  L" (Debug)"
#else
                  L" (Release)"
#endif
    );
    ::SetWindowTextW(aboutText_, buf);
}

void SettingsPanel::refreshFromSnapshot(const UiSnapshot& s) {
    updateFromConfig(s.config);
    // Playback speed: map 0.5/0.75/1.0/1.5/2.0 to combo index 0–4.
    int spdIdx = 2; // default 1.0x
    if (s.config.playbackSpeed <= 0.5) spdIdx = 0;
    else if (s.config.playbackSpeed <= 0.75) spdIdx = 1;
    else if (s.config.playbackSpeed <= 1.0) spdIdx = 2;
    else if (s.config.playbackSpeed <= 1.5) spdIdx = 3;
    else spdIdx = 4;
    ::SendMessageW(speedCombo_, CB_SETCURSEL, spdIdx, 0);
}

LRESULT CALLBACK SettingsPanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsPanel*>(cs->lpCreateParams);
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
                case kCkStartup:
                    if (code == BN_CLICKED) {
                        Command c;
                        c.id = CommandId::ConfigSet;
                        c.s1 = L"startWithWindows";
                        c.s2 = ::SendMessageW(self->checkStartup_, BM_GETCHECK, 0, 0) ==
                                       BST_CHECKED
                                   ? L"true"
                                   : L"false";
                        self->post_(c);
                    }
                    return 0;
                case kCkTray:
                    if (code == BN_CLICKED) {
                        Command c;
                        c.id = CommandId::ConfigSet;
                        c.s1 = L"minimizeToTray";
                        c.s2 = ::SendMessageW(self->checkTray_, BM_GETCHECK, 0, 0) == BST_CHECKED
                                   ? L"true"
                                   : L"false";
                        self->post_(c);
                    }
                    return 0;
                case kCmLogLevel:
                    if (code == CBN_SELCHANGE) {
                        const int i = static_cast<int>(
                            ::SendMessageW(self->logCombo_, CB_GETCURSEL, 0, 0));
                        Command c;
                        c.id = CommandId::ConfigSet;
                        c.s1 = L"logLevel";
                        c.s2 = i == 1 ? L"debug" : (i == 2 ? L"warn" : (i == 3 ? L"error" : L"info"));
                        self->post_(c);
                    }
                    return 0;
                case kCmSpeed:
                    if (code == CBN_SELCHANGE) {
                        static constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.5, 2.0};
                        const int i = static_cast<int>(
                            ::SendMessageW(self->speedCombo_, CB_GETCURSEL, 0, 0));
                        if (i >= 0 && i < 5) {
                            Command c;
                            c.id = CommandId::ConfigSet;
                            c.s1 = L"playbackSpeed";
                            // Format as string (e.g. "1" or "1.5")
                            const double spd = kSpeeds[i];
                            if (spd == static_cast<int>(spd)) {
                                c.s2 = std::to_wstring(static_cast<int>(spd));
                            } else {
                                wchar_t buf[16];
                                std::swprintf(buf, 16, L"%.2f", spd);
                                c.s2 = buf;
                            }
                            self->post_(c);
                        }
                    }
                    return 0;
                case kBtnReadme: {
                    // UI-local action (spec §10.11): open the README beside
                    // the executable in the default viewer.
                    wchar_t exe[MAX_PATH] = {};
                    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    std::filesystem::path readme = std::filesystem::path(exe).parent_path() /
                                                   L"README.md";
                    const HINSTANCE hr =
                        ::ShellExecuteW(nullptr, L"open", readme.c_str(), nullptr, nullptr,
                                        SW_SHOWNORMAL);
                    if (reinterpret_cast<INT_PTR>(hr) <= 32) {
                        log::Logger::instance().warn(L"settings: cannot open README ({})",
                                                     readme.wstring());
                    }
                    return 0;
                }
            }
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
