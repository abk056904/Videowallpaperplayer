#include "ui/panels/HomePanel.h"

#include <cstdio>
#include <cwchar>

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
    for (int i = 0; i < kRowCount; ++i) {
        rows_[i] = ctl(hwnd_, L"STATIC", L"", WS_VISIBLE, 12, L.y(i), 600, L.cy, nullptr);
    }
    btnPause_ = ctl(hwnd_, L"BUTTON", L"Pause", WS_VISIBLE | BS_PUSHBUTTON, 12, 0, 100, L.cy,
                    reinterpret_cast<HMENU>(kBtnPause));
    btnNext_ = ctl(hwnd_, L"BUTTON", L"Next", WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 100, L.cy,
                   reinterpret_cast<HMENU>(kBtnNext));
    btnPrev_ = ctl(hwnd_, L"BUTTON", L"Previous", WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 100, L.cy,
                   reinterpret_cast<HMENU>(kBtnPrev));
    layout(600);
    updateRows();
    return true;
}

void HomePanel::layout(int width) {
    const Layout L = Layout::from(hwnd_);
    for (int i = 0; i < kRowCount; ++i) {
        ::MoveWindow(rows_[i], 12, L.y(i), width - 24, L.cy, TRUE);
    }
    const int btnY = L.y(kRowCount) + 4;
    int x = 12;
    const int bw = 100;
    ::MoveWindow(btnPause_, x, btnY, bw, L.cy, TRUE);
    x += bw + L.u;
    ::MoveWindow(btnNext_, x, btnY, bw, L.cy, TRUE);
    x += bw + L.u;
    ::MoveWindow(btnPrev_, x, btnY, bw, L.cy, TRUE);
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
        ::SetWindowTextW(btnPause_, paused ? L"Resume" : L"Pause");
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
        case PlaybackState::Playing: stateText = L"PLAYING"; break;
        case PlaybackState::Paused: stateText = L"PAUSED"; break;
        case PlaybackState::Suspended: stateText = L"SUSPENDED"; break;
        case PlaybackState::NoWallpaper: stateText = L"no wallpaper"; break;
    }
    wchar_t buf[512];

    std::swprintf(buf, 512, L"Current wallpaper : %ls",
                  s.videoName.empty() ? L"(none)" : s.videoName.c_str());
    ::SetWindowTextW(rows_[kRowWallpaper], buf);

    std::swprintf(buf, 512, L"Playback state    : %ls (reasons: 0x%X)", stateText.c_str(),
                  s.pauseReasons);
    ::SetWindowTextW(rows_[kRowState], buf);

    std::swprintf(buf, 512, L"Current monitor   : %ls", s.monitorId.c_str());
    ::SetWindowTextW(rows_[kRowMonitor], buf);

    std::swprintf(buf, 512, L"Presented FPS     : %.1f        Dropped frames : %llu",
                  t.presentedFps, static_cast<unsigned long long>(t.droppedFrames));
    ::SetWindowTextW(rows_[kRowFps], buf);

    std::swprintf(buf, 512, L"Decode latency    : %.1f ms        Decoded FPS : %.1f",
                  t.decodeLatencyMs, t.decodedFps);
    ::SetWindowTextW(rows_[kRowLatency], buf);

    std::swprintf(buf, 512, L"Decoder           : %ls",
                  s.decoderMode.empty() ? L"software" : s.decoderMode.c_str());
    ::SetWindowTextW(rows_[kRowDecoder], buf);

    std::swprintf(buf, 512, L"GPU adapter       : %ls",
                  s.adapterName.empty() ? L"(unknown)" : s.adapterName.c_str());
    ::SetWindowTextW(rows_[kRowAdapter], buf);

    std::swprintf(buf, 512, L"CPU: %.1f%%   GPU: %.1f%%   RAM: %.0f MB   VRAM: %.0f / %.0f MB",
                  t.cpuUsage, t.gpuUsage, t.systemMemoryUsed / (1024.0 * 1024.0),
                  t.gpuMemoryUsed / (1024.0 * 1024.0), t.gpuMemoryBudget / (1024.0 * 1024.0));
    ::SetWindowTextW(rows_[kRowWorkload], buf);
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
