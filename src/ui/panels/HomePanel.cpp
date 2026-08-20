#include "ui/panels/HomePanel.h"

#include <cwchar>

#include "ui/Theme.h"

namespace vw::ui {

const wchar_t* HomePanel::kClassName = L"VideoWallpaper.HomePanel";

bool HomePanel::create(HWND parent) {
    if (hwnd_) {
        return true;
    }
    hwnd_ = createPanelWindow(parent, kClassName, &HomePanel::wndProc, this);
    if (!hwnd_) {
        return false;
    }
    initPanelFont();
    const Layout L = Layout::from(hwnd_);

    // Create status rows
    for (int i = 0; i < kRowCount; ++i) {
        rows_[i] = ctl(hwnd_, L"STATIC", L"", WS_VISIBLE, 16, L.y(i), 600, L.cy, nullptr);
    }

    // Modern playback buttons
    btnPause_ = ctl(hwnd_, L"BUTTON", L"\u23F8  Pause", WS_VISIBLE | BS_PUSHBUTTON,
                    16, 0, 120, L.cy, reinterpret_cast<HMENU>(kBtnPause));
    btnNext_ = ctl(hwnd_, L"BUTTON", L"\u23ED  Next", WS_VISIBLE | BS_PUSHBUTTON,
                   0, 0, 100, L.cy, reinterpret_cast<HMENU>(kBtnNext));
    btnPrev_ = ctl(hwnd_, L"BUTTON", L"\u23EE  Previous", WS_VISIBLE | BS_PUSHBUTTON,
                   0, 0, 120, L.cy, reinterpret_cast<HMENU>(kBtnPrev));

    layout(600);
    updateRows();
    return true;
}

void HomePanel::layout(int width) {
    const Layout L = Layout::from(hwnd_);
    for (int i = 0; i < kRowCount; ++i) {
        ::MoveWindow(rows_[i], 16, L.y(i), width - 32, L.cy, TRUE);
    }
    const int btnY = L.y(kRowCount) + L.u;
    int x = 16;
    ::MoveWindow(btnPause_, x, btnY, 120, L.cy, TRUE);
    x += 120 + L.u;
    ::MoveWindow(btnNext_, x, btnY, 100, L.cy, TRUE);
    x += 100 + L.u;
    ::MoveWindow(btnPrev_, x, btnY, 120, L.cy, TRUE);
}

void HomePanel::relayout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    layout(rc.right - rc.left);
}

void HomePanel::onTelemetry(const TelemetrySnapshot& t) {
    telemetry_ = t;
    updateRows();
}

void HomePanel::onPlaybackState(const PlaybackStateNotification& s) {
    state_ = s;
    const bool paused = s.state != PlaybackState::Playing;
    if (btnPause_) {
        ::SetWindowTextW(btnPause_, paused ? L"\u25B6  Resume" : L"\u23F8  Pause");
    }
    updateRows();
}

void HomePanel::updateRows() {
    if (!hwnd_) {
        return;
    }
    const auto& t = telemetry_;
    const auto& s = state_;

    std::wstring stateText;
    switch (s.state) {
        case PlaybackState::Playing: stateText = L"\u25B6 PLAYING"; break;
        case PlaybackState::Paused: stateText = L"\u23F8 PAUSED"; break;
        case PlaybackState::Suspended: stateText = L"\u23F9 SUSPENDED"; break;
        case PlaybackState::NoWallpaper: stateText = L"No wallpaper"; break;
    }

    // Status display
    wchar_t buf[512];

    std::swprintf(buf, 512, L"\U0001F3AC  %ls",
                  s.videoName.empty() ? L"No video loaded" : s.videoName.c_str());
    ::SetWindowTextW(rows_[kRowWallpaper], buf);

    std::swprintf(buf, 512, L"\u25C9  %ls", stateText.c_str());
    ::SetWindowTextW(rows_[kRowState], buf);

    std::swprintf(buf, 512, L"\U0001F4F7  %ls", s.monitorId.c_str());
    ::SetWindowTextW(rows_[kRowMonitor], buf);

    std::swprintf(buf, 512, L"\U0001F4CA  %.1f fps  \u2022  %.1f ms decode  \u2022  %llu dropped",
                  t.presentedFps, t.decodeLatencyMs,
                  static_cast<unsigned long long>(t.droppedFrames));
    ::SetWindowTextW(rows_[kRowFps], buf);

    std::swprintf(buf, 512, L"\U0001F527  %ls", s.decoderMode.empty() ? L"software" : s.decoderMode.c_str());
    ::SetWindowTextW(rows_[kRowDecoder], buf);

    std::swprintf(buf, 512, L"\U0001F3AE  %ls", s.adapterName.empty() ? L"(unknown)" : s.adapterName.c_str());
    ::SetWindowTextW(rows_[kRowAdapter], buf);

    std::swprintf(buf, 512, L"\u26A1  CPU: %.1f%%  \u2022  GPU: %.1f%%  \u2022  RAM: %.0f MB",
                  t.cpuUsage, t.gpuUsage, t.systemMemoryUsed / (1024.0 * 1024.0));
    ::SetWindowTextW(rows_[kRowWorkload], buf);

    std::swprintf(buf, 512, L"\U0001F4C8  Render: %.1f ms  \u2022  Latency: %.1f ms",
                  t.renderTimeMs, t.decodeLatencyMs);
    ::SetWindowTextW(rows_[kRowLatency], buf);
}

LRESULT CALLBACK HomePanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<HomePanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HomePanel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kBtnPause: {
                    Command c;
                    c.id = CommandId::PlayPauseToggle;
                    self->post_(c);
                    return 0;
                }
                case kBtnNext: {
                    Command c;
                    c.id = CommandId::Next;
                    self->post_(c);
                    return 0;
                }
                case kBtnPrev: {
                    Command c;
                    c.id = CommandId::Previous;
                    self->post_(c);
                    return 0;
                }
            }
            return 0;
        case WM_SIZE: {
            const int w = static_cast<int>(LOWORD(lParam));
            self->layout(w);
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
