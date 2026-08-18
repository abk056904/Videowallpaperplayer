#include "ui/panels/MonitorsPanel.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <filesystem>

namespace vw::ui {

const wchar_t* MonitorsPanel::kClassName = L"VideoWallpaper.MonitorsPanel";

MonitorsPanel::~MonitorsPanel() {
    setPreviewBitmap(nullptr);
}

std::wstring MonitorsPanel::monitorLabel(const MonitorInfo& m) {
    wchar_t buf[128];
    std::swprintf(buf, 128, L"%ls  %dx%d @ %d Hz%ls", m.id.c_str(), m.width, m.height,
                  m.refreshDen ? m.refreshNum / m.refreshDen : 0, m.primary ? L" (primary)" : L"");
    return buf;
}

bool MonitorsPanel::create(HWND parent) {
    if (hwnd_) {
        return true;
    }
    hwnd_ = createPanelWindow(parent, kClassName, &MonitorsPanel::wndProc, this);
    if (!hwnd_) {
        return false;
    }
    initPanelFont();
    const Layout L = Layout::from(hwnd_);

    // Mode radios.
    radioInd_ = ctl(hwnd_, L"BUTTON", L"Independent", WS_VISIBLE | BS_AUTORADIOBUTTON,
                    12, 8, ::MulDiv(120, L.u, 5), L.cy, reinterpret_cast<HMENU>(kCtlIndependent));
    radioClone_ = ctl(hwnd_, L"BUTTON", L"Clone", WS_VISIBLE | BS_AUTORADIOBUTTON,
                      12 + ::MulDiv(130, L.u, 5), 8, ::MulDiv(80, L.u, 5), L.cy,
                      reinterpret_cast<HMENU>(kCtlClone));
    ::SendMessageW(radioInd_, BM_SETCHECK, BST_CHECKED, 0);

    // Monitor list.
    list_ = ctl(hwnd_, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
                8, L.y(1), 500, ::MulDiv(120, L.u, 5), nullptr);
    ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* titles[] = {L"Monitor", L"Resolution", L"Refresh", L"Primary"};
    for (int c = 0; c < 4; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(titles[c]);
        col.cx = ::MulDiv(c == 0 ? 160 : 70, L.u, 5);
        ::SendMessageW(list_, LVM_INSERTCOLUMNW, c, reinterpret_cast<LPARAM>(&col));
    }

    // Wallpaper source combo: "Playlist" + one entry per library file.
    const int row2 = L.y(3);
    ctl(hwnd_, L"STATIC", L"Wallpaper source :", WS_VISIBLE, 12, row2, ::MulDiv(120, L.u, 5),
        L.cy, nullptr);
    sourceCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                       12 + ::MulDiv(125, L.u, 5), row2, ::MulDiv(260, L.u, 5), 300,
                       reinterpret_cast<HMENU>(kCtlSourceCombo));

    const int row3 = row2 + L.cy + L.u;
    ctl(hwnd_, L"STATIC", L"Scaling          :", WS_VISIBLE, 12, row3, ::MulDiv(120, L.u, 5),
        L.cy, nullptr);
    scalingCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                        12 + ::MulDiv(125, L.u, 5), row3, ::MulDiv(120, L.u, 5), 200,
                        reinterpret_cast<HMENU>(kCtlScalingCombo));
    for (const wchar_t* s : {L"Fill", L"Fit", L"Stretch", L"Center"}) {
        ::SendMessageW(scalingCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }
    ::SendMessageW(scalingCombo_, CB_SETCURSEL, 0, 0);

    // Preview area + buttons.
    preview_ = ctl(hwnd_, L"STATIC", L"(no preview)", WS_VISIBLE | SS_CENTERIMAGE | SS_BITMAP,
                   12, row3 + L.cy + L.u, ::MulDiv(320, L.u, 5), ::MulDiv(180, L.u, 5), nullptr);
    btnPreview_ = ctl(hwnd_, L"BUTTON", L"Preview", WS_VISIBLE | BS_PUSHBUTTON,
                      12 + ::MulDiv(330, L.u, 5), row3 + L.cy + L.u, ::MulDiv(110, L.u, 5), L.cy,
                      reinterpret_cast<HMENU>(kBtnPreview));
    btnSet_ = ctl(hwnd_, L"BUTTON", L"Set as wallpaper", WS_VISIBLE | BS_PUSHBUTTON,
                  12 + ::MulDiv(330, L.u, 5), row3 + L.cy + L.u + L.cy + L.u,
                  ::MulDiv(130, L.u, 5), L.cy, reinterpret_cast<HMENU>(kBtnSetWallpaper));
    return true;
}

void MonitorsPanel::relayout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    layout(rc.right - rc.left, rc.bottom - rc.top);
}

void MonitorsPanel::layout(int width, int /*height*/) {
    const Layout L = Layout::from(hwnd_);
    ::MoveWindow(list_, 8, L.y(1), width - 16, ::MulDiv(120, L.u, 5), TRUE);
    // Bottom controls were positioned at create() relative to fixed rows; the
    // panel is large enough at the minimum window size. Stretch the preview.
    RECT r{};
    ::GetWindowRect(preview_, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    ::MoveWindow(preview_, 12, 8 + L.y(5), width - 40 > w ? width - 40 : w, h, TRUE);
}

void MonitorsPanel::rebuildMonitors() {
    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    int row = 0;
    for (const auto& m : monitors_) {
        wchar_t buf[64];
        std::swprintf(buf, 64, L"%dx%d", m.width, m.height);
        LVITEMW lv{};
        lv.mask = LVIF_TEXT;
        lv.iItem = row;
        lv.pszText = const_cast<wchar_t*>(m.id.c_str());
        ::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lv));
        LVITEMW sub{};
        sub.pszText = buf;
        sub.iSubItem = 1;
        ::SendMessageW(list_, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&sub));
        std::swprintf(buf, 64, L"%d Hz", m.refreshDen ? m.refreshNum / m.refreshDen : 0);
        sub.iSubItem = 2;
        ::SendMessageW(list_, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&sub));
        std::swprintf(buf, 64, L"%ls", m.primary ? L"✔" : L"");
        sub.iSubItem = 3;
        ::SendMessageW(list_, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&sub));
        ++row;
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
}

void MonitorsPanel::rebuildSourceCombo(const UiSnapshot& s) {
    library_ = s.libraryItems;
    ::SendMessageW(sourceCombo_, CB_RESETCONTENT, 0, 0);
    ::SendMessageW(sourceCombo_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"Playlist (default)"));
    for (const auto& item : library_) {
        const std::wstring name = std::filesystem::path(item.path).filename().wstring();
        ::SendMessageW(sourceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    // The combo row i (i>=1) maps to library_[i-1] — look up by index, never
    // by stored pointers (library_ is reassigned on each snapshot).
    int sel = 0;
    if (source_ == WallpaperSource::File) {
        for (size_t i = 0; i < library_.size(); ++i) {
            if (library_[i].path == sourceId_) {
                sel = static_cast<int>(i) + 1;
                break;
            }
        }
    }
    ::SendMessageW(sourceCombo_, CB_SETCURSEL, sel, 0);
}

void MonitorsPanel::updateReadback(const WallpaperAssignmentNotification& n) {
    clone_ = n.clone;
    source_ = n.source;
    sourceId_ = n.sourceId;
    scaling_ = n.scaling;
    ::SendMessageW(radioInd_, BM_SETCHECK, clone_ ? BST_UNCHECKED : BST_CHECKED, 0);
    ::SendMessageW(radioClone_, BM_SETCHECK, clone_ ? BST_CHECKED : BST_UNCHECKED, 0);
    int s = 0;
    switch (scaling_) {
        case ScalingMode::Fill: s = 0; break;
        case ScalingMode::Fit: s = 1; break;
        case ScalingMode::Stretch: s = 2; break;
        case ScalingMode::Center: s = 3; break;
    }
    ::SendMessageW(scalingCombo_, CB_SETCURSEL, s, 0);
}

void MonitorsPanel::refreshFromSnapshot(const UiSnapshot& s) {
    monitors_ = s.monitors;
    rebuildMonitors();
    rebuildSourceCombo(s);
    if (!s.assignments.empty()) {
        updateReadback(s.assignments.front());
    }
}

void MonitorsPanel::onMonitorEvent(const MonitorEvent&) {
    // The app re-pushes the monitor list on the next snapshot; to stay live
    // without polling, the panel re-reads via the snapshot on open and on
    // display change. Acceptable v1 behavior (single display here).
}

void MonitorsPanel::onWallpaperAssignment(const WallpaperAssignmentNotification& n) {
    updateReadback(n);
}

void MonitorsPanel::postSourceSelection() {
    const int sel = static_cast<int>(::SendMessageW(sourceCombo_, CB_GETCURSEL, 0, 0));
    Command c;
    if (sel <= 0) {
        c.id = CommandId::SetWallpaperPlaylist;
        c.s1.clear();
        c.s2 = L"default";
    } else if (sel - 1 < static_cast<int>(library_.size())) {
        c.id = CommandId::SetWallpaperFile;
        c.s1.clear();
        c.s2 = library_[sel - 1].path;
    } else {
        return;
    }
    post_(c);
}

void MonitorsPanel::previewClicked() {
    Command c;
    c.id = CommandId::GrabFrameSnapshot;
    c.s1.clear(); // primary monitor
    post_(c);
}

void MonitorsPanel::setWallpaperClicked() {
    postSourceSelection();
}

void MonitorsPanel::setPreviewBitmap(HBITMAP bmp) {
    if (previewBitmap_) {
        ::DeleteObject(previewBitmap_);
        previewBitmap_ = nullptr;
    }
    if (bmp) {
        ::SendMessageW(preview_, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bmp));
        previewBitmap_ = bmp;
    } else {
        ::SendMessageW(preview_, STM_SETIMAGE, IMAGE_BITMAP, 0);
        ::SetWindowTextW(preview_, L"(no preview)");
    }
}

void MonitorsPanel::onFrameSnapshot(HBITMAP bitmap) {
    setPreviewBitmap(bitmap);
}

LRESULT CALLBACK MonitorsPanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<MonitorsPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MonitorsPanel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kBtnPreview:
                    self->previewClicked();
                    return 0;
                case kBtnSetWallpaper:
                    self->setWallpaperClicked();
                    return 0;
                case kCtlIndependent:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        Command c;
                        c.id = CommandId::SetGlobalMode;
                        c.b1 = false;
                        self->post_(c);
                    }
                    return 0;
                case kCtlClone:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        Command c;
                        c.id = CommandId::SetGlobalMode;
                        c.b1 = true;
                        self->post_(c);
                    }
                    return 0;
                case kCtlSourceCombo:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        self->postSourceSelection();
                    }
                    return 0;
                case kCtlScalingCombo:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        Command c;
                        c.id = CommandId::SetScaling;
                        c.s1.clear();
                        c.scaling =
                            static_cast<ScalingMode>(
                                ::SendMessageW(self->scalingCombo_, CB_GETCURSEL, 0, 0));
                        self->post_(c);
                    }
                    return 0;
            }
            return 0;
        case WM_SIZE: {
            self->layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
