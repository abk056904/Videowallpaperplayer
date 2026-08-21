#include "ui/Win32UI.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <shellapi.h>

#include "logging/Logger.h"
#include "ui/Theme.h"

#pragma comment(lib, "dwmapi.lib")

namespace vw::ui {

const wchar_t* Win32UI::kClassName = L"VideoWallpaper.MainWindow";
const wchar_t* Win32UI::kTabNames[] = {L"Home", L"Library", L"Playlists",
                                        L"Monitors", L"Performance", L"Settings"};

Win32UI::Win32UI(PostFn post, RefreshFn refreshPlaylist,
                 LibraryPanel::MetadataRequestFn requestMeta,
                 LibraryPanel::ThumbnailRequestFn requestThumb)
    : post_(std::move(post)),
      refreshPlaylist_(std::move(refreshPlaylist)),
      home_(std::make_unique<HomePanel>(post_)),
      library_(std::make_unique<LibraryPanel>(post_, std::move(requestMeta),
                                              std::move(requestThumb))),
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
    wc.hIcon = ::LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = ::CreateSolidBrush(theme::kBgBase);
    ::RegisterClassExW(&wc);

    const int dpi = static_cast<int>(::GetDpiForSystem());
    hwnd_ = ::CreateWindowExW(0, kClassName, L"Video Wallpaper",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              ::MulDiv(900, dpi, 96), ::MulDiv(600, dpi, 96),
                              nullptr, nullptr, inst, this);
    if (!hwnd_) {
        log::Logger::instance().error(L"ui: main window creation failed ({})",
                                      ::GetLastError());
        return false;
    }

    // Enable dark mode title bar (Windows 10 1903+)
    BOOL darkMode = TRUE;
    ::DwmSetWindowAttribute(hwnd_, 19 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/,
                            &darkMode, sizeof(darkMode));
    // Dark menu/title bar caption
    BOOL darkCaption = TRUE;
    ::DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE (v2)*/,
                            &darkCaption, sizeof(darkCaption));

    // Create fonts
    fontTab_ = theme::createFontBold(dpi);
    fontPanel_ = theme::createFont(dpi);

    // Initialize common controls for ListView, ComboBox, etc.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES;
    ::InitCommonControlsEx(&icc);

    // Enable drag & drop on the main window.
    ::DragAcceptFiles(hwnd_, TRUE);

    // Create panels (children of main window, positioned below tab bar)
    Panel* panels[] = {home_.get(), library_.get(), playlists_.get(),
                       monitors_.get(), performance_.get(), settings_.get()};
    for (Panel* p : panels) {
        const_cast<Panel*>(p)->create(hwnd_);
    }
    showTab(0);
    return true;
}

void Win32UI::showTab(int index) {
    if (!hwnd_ || index < 0 || index >= kTabCount) {
        return;
    }
    currentTab_ = index;
    Panel* panels[] = {home_.get(), library_.get(), playlists_.get(),
                       monitors_.get(), performance_.get(), settings_.get()};
    for (int i = 0; i < kTabCount; ++i) {
        if (i == index) {
            panels[i]->show();
        } else {
            panels[i]->hide();
        }
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE); // repaint tabs
}

void Win32UI::layout(int width, int height) {
    if (!hwnd_) {
        return;
    }
    const int dpi = static_cast<int>(::GetDpiForWindow(hwnd_));
    const int tabH = theme::dpiScale(theme::kTabHeight, dpi);
    const int panelY = tabH;
    const int panelH = height - panelY;

    Panel* panels[] = {home_.get(), library_.get(), playlists_.get(),
                       monitors_.get(), performance_.get(), settings_.get()};
    for (int i = 0; i < kTabCount; ++i) {
        ::MoveWindow(panels[i]->handle(), 0, panelY, width, panelH, TRUE);
    }
    for (Panel* p : panels) {
        p->relayout();
    }
}

// ---- Custom tab bar painting ----

RECT Win32UI::tabRect(int index) const {
    const int dpi = static_cast<int>(::GetDpiForWindow(hwnd_));
    const int tabH = theme::dpiScale(theme::kTabHeight, dpi);

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int totalW = rc.right;
    const int tabW = totalW / kTabCount;
    rc.left = index * tabW;
    rc.right = (index + 1) * tabW;
    rc.top = 0;
    rc.bottom = tabH;
    return rc;
}

int Win32UI::tabHitTest(int x, int y) const {
    const int dpi = static_cast<int>(::GetDpiForWindow(hwnd_));
    const int tabH = theme::dpiScale(theme::kTabHeight, dpi);
    if (y < 0 || y >= tabH) {
        return -1;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    const int tabW = rc.right / kTabCount;
    if (tabW <= 0) return -1;
    const int idx = x / tabW;
    return (idx >= 0 && idx < kTabCount) ? idx : -1;
}

void Win32UI::paintTabBar(HDC hdc, int width) {
    const int dpi = static_cast<int>(::GetDpiForWindow(hwnd_));
    const int tabH = theme::dpiScale(theme::kTabHeight, dpi);

    // Tab bar background
    RECT rcBar = {0, 0, width, tabH};
    theme::fillRect(hdc, rcBar, theme::kBgSurface);

    const int tabW = width / kTabCount;
    HFONT font = fontTab_ ? fontTab_ : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));

    for (int i = 0; i < kTabCount; ++i) {
        RECT rc = {i * tabW, 0, (i + 1) * tabW, tabH};

        // Active tab: accent underline + lighter bg
        if (i == currentTab_) {
            theme::fillRect(hdc, rc, theme::kBgCard);
            // Accent underline
            RECT rcLine = {rc.left, tabH - 3, rc.right, tabH};
            theme::fillRect(hdc, rcLine, theme::kAccent);
        }
        // Hover
        else if (i == hoverTab_) {
            theme::fillRect(hdc, rc, theme::kBgHover);
        }

        // Tab text
        COLORREF textColor = (i == currentTab_) ? theme::kTextPrimary
                             : theme::kTextSecondary;
        RECT textRc = {rc.left + 4, rc.top, rc.right - 4, rc.bottom - 3};
        theme::drawTextCentered(hdc, textRc, kTabNames[i], font, textColor);
    }

    // Bottom border line
    RECT rcBorder = {0, tabH - 1, width, tabH};
    theme::fillRect(hdc, rcBorder, theme::kBorder);
}

// ---- Window procedure ----

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
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = ::BeginPaint(hwnd, &ps);
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            // Fill background
            theme::fillRect(hdc, rc, theme::kBgBase);
            // Paint tab bar
            self->paintTabBar(hdc, rc.right);
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const int x = LOWORD(lParam);
            const int y = HIWORD(lParam);
            const int tab = self->tabHitTest(x, y);
            if (tab >= 0) {
                self->showTab(tab);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int x = LOWORD(lParam);
            const int y = HIWORD(lParam);
            const int tab = self->tabHitTest(x, y);
            if (tab != self->hoverTab_) {
                self->hoverTab_ = tab;
                ::InvalidateRect(hwnd, nullptr, FALSE);
                // Track mouse for hover leave
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                ::TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            self->hoverTab_ = -1;
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: {
            const int w = LOWORD(lParam);
            const int h = HIWORD(lParam);
            self->layout(w, h);
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ::MulDiv(720, dpi, 96);
            mmi->ptMinTrackSize.y = ::MulDiv(480, dpi, 96);
            return 0;
        }
        case WM_DROPFILES: {
            // Drag & drop: add dropped files to the playlist.
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            const UINT count = ::DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (::DragQueryFileW(hDrop, i, path, MAX_PATH)) {
                    // Check if it's a video file by extension.
                    const wchar_t* ext = PathFindExtensionW(path);
                    if (ext && (*ext == L'.')) {
                        Command c;
                        c.id = CommandId::PlaylistAddFiles;
                        c.paths.push_back(path);
                        self->post_(c);
                    }
                }
            }
            ::DragFinish(hDrop);
            return 0;
        }
        case WM_CLOSE:
            if (self->onClose_) {
                self->onClose_();
            }
            return 0;
        case WM_DESTROY:
            if (self->fontTab_) { ::DeleteObject(self->fontTab_); self->fontTab_ = nullptr; }
            if (self->fontPanel_) { ::DeleteObject(self->fontPanel_); self->fontPanel_ = nullptr; }
            self->hwnd_ = nullptr;
            self->visible_ = false;
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- Public API ----

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
        visible_ = false;
    }
}

void Win32UI::selectTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= kTabCount) {
        tabIndex = 0;
    }
    show();
    showTab(tabIndex);
}

void Win32UI::showFrameSnapshot(HBITMAP bitmap) {
    if (monitors_) {
        monitors_->onFrameSnapshot(bitmap);
    } else if (bitmap) {
        ::DeleteObject(bitmap);
    }
}

void Win32UI::setThumbnail(const std::wstring& videoPath, const std::wstring& bmpPath) {
    if (library_) {
        library_->setThumbnail(videoPath, bmpPath);
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

} // namespace vw::ui
