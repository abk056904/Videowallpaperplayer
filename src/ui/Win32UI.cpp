#include "ui/Win32UI.h"

#include <commctrl.h>

#include "logging/Logger.h"

namespace vw::ui {

const wchar_t* Win32UI::kClassName = L"VideoWallpaper.MainWindow";

Win32UI::Win32UI(PostFn post, RefreshFn refreshPlaylist,
                 LibraryPanel::MetadataRequestFn requestMeta)
    : post_(std::move(post)),
      refreshPlaylist_(std::move(refreshPlaylist)),
      home_(std::make_unique<HomePanel>(post_)),
      library_(std::make_unique<LibraryPanel>(post_, std::move(requestMeta))),
      playlists_(std::make_unique<PlaylistsPanel>(post_, refreshPlaylist_)),
      monitors_(std::make_unique<MonitorsPanel>(post_)),
      performance_(std::make_unique<PerformancePanel>(post_)),
      settings_(std::make_unique<SettingsPanel>(post_)) {}

bool Win32UI::create() {
    if (hwnd_) {
        return true;
    }
    const HINSTANCE inst = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Win32UI::wndProc;
    wc.hInstance = inst;
    wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(101)); // IDI_APP_ICON
    wc.hIconSm = wc.hIcon;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);

    const int dpi = static_cast<int>(::GetDpiForSystem());
    hwnd_ = ::CreateWindowExW(0, kClassName, L"Video Wallpaper", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              ::MulDiv(900, dpi, 96), ::MulDiv(600, dpi, 96), nullptr, nullptr,
                              inst, this);
    if (!hwnd_) {
        log::Logger::instance().error(L"ui: main window creation failed ({})", ::GetLastError());
        return false;
    }

    // Tab control with six tabs.
    tabs_ = ::CreateWindowExW(0, WC_TABCONTROL, L"",
                              WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH, 0, 0, 0, 0, hwnd_, nullptr,
                              inst, nullptr);
    ::SendMessageW(tabs_, WM_SETFONT, reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)),
                   TRUE);
    const wchar_t* titles[] = {L"Home", L"Library", L"Playlists", L"Monitors",
                               L"Performance", L"Settings"};
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    for (int i = 0; i < 6; ++i) {
        item.pszText = const_cast<wchar_t*>(titles[i]);
        ::SendMessageW(tabs_, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&item));
    }

    // Panels (children of the main window, below the tab control).
    const Panel* panels[] = {home_.get(), library_.get(), playlists_.get(), monitors_.get(),
                             performance_.get(), settings_.get()};
    for (const Panel* p : panels) {
        const_cast<Panel*>(p)->create(hwnd_);
    }
    showTab(0);
    return true;
}

void Win32UI::showTab(int index) {
    if (!hwnd_) {
        return;
    }
    currentTab_ = index;
    ::SendMessageW(tabs_, TCM_SETCURSEL, index, 0);
    Panel* panels[] = {home_.get(), library_.get(), playlists_.get(), monitors_.get(),
                       performance_.get(), settings_.get()};
    for (int i = 0; i < 6; ++i) {
        if (i == index) {
            panels[i]->show();
        } else {
            panels[i]->hide();
        }
    }
}

void Win32UI::layout(int width, int height) {
    if (!hwnd_) {
        return;
    }
    const int tabH = ::GetSystemMetrics(SM_CYCAPTION) > 0 ? 22 : 22;
    ::MoveWindow(tabs_, 0, 0, width, tabH, TRUE);
    const int panelY = tabH + 2;
    const int panelH = height - panelY - 2;
    Panel* panels[] = {home_.get(), library_.get(), playlists_.get(), monitors_.get(),
                       performance_.get(), settings_.get()};
    for (int i = 0; i < 6; ++i) {
        ::MoveWindow(panels[i]->handle(), 0, panelY, width, panelH, TRUE);
    }
    // Panels with dynamic content re-flow.
    for (Panel* p : panels) {
        p->relayout();
    }
}

void Win32UI::show() {
    if (!hwnd_) {
        create();
    }
    if (!hwnd_) {
        return;
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    visible_ = true;
}

void Win32UI::hide() {
    if (hwnd_) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
}

void Win32UI::toggle() {
    if (visible_) {
        hide();
    } else {
        show();
    }
}

void Win32UI::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        tabs_ = nullptr;
        visible_ = false;
    }
}

void Win32UI::selectTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex > 5) {
        tabIndex = 0;
    }
    show();
    showTab(tabIndex);
}

void Win32UI::showFrameSnapshot(HBITMAP bitmap) {
    if (monitors_) {
        monitors_->onFrameSnapshot(bitmap);
    } else if (bitmap) {
        ::DeleteObject(bitmap); // nobody owns it — don't leak
    }
}

void Win32UI::refreshFromSnapshot(const UiSnapshot& s) {
    if (home_) home_->refreshFromSnapshot(s);
    if (library_) library_->refreshFromSnapshot(s);
    if (playlists_) playlists_->refreshFromSnapshot(s);
    if (monitors_) monitors_->refreshFromSnapshot(s);
    if (performance_) performance_->refreshFromSnapshot(s);
    if (settings_) settings_->refreshFromSnapshot(s);
}

void Win32UI::onTelemetry(const TelemetrySnapshot& t) {
    if (home_) home_->onTelemetry(t);
}

void Win32UI::onPlaybackState(const PlaybackStateNotification& s) {
    if (home_) home_->onPlaybackState(s);
}

void Win32UI::onMonitorEvent(const MonitorEvent& e) {
    if (monitors_) monitors_->onMonitorEvent(e);
}

void Win32UI::onLibraryChange(const LibraryChangeNotification& n) {
    if (library_) library_->onLibraryChange(n);
}

void Win32UI::onPlaylistChange(const PlaylistChangeNotification& n) {
    if (playlists_) playlists_->onPlaylistChange(n);
}

void Win32UI::onWallpaperAssignment(const WallpaperAssignmentNotification& n) {
    if (monitors_) monitors_->onWallpaperAssignment(n);
}

LRESULT CALLBACK Win32UI::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<Win32UI*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win32UI*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_SIZE: {
            self->layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
        case WM_GETMINMAXINFO: {
            const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ::MulDiv(720, dpi, 96);
            mmi->ptMinTrackSize.y = ::MulDiv(480, dpi, 96);
            return 0;
        }
        case WM_NOTIFY: {
            const auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->hwndFrom == self->tabs_ && nmhdr->code == TCN_SELCHANGE) {
                const int sel =
                    static_cast<int>(::SendMessageW(self->tabs_, TCM_GETCURSEL, 0, 0));
                self->showTab(sel);
            }
            return 0;
        }
        case WM_CLOSE:
            if (self->onClose_) {
                self->onClose_(); // the app hides or destroys per config
            }
            return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            self->tabs_ = nullptr;
            self->visible_ = false;
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
