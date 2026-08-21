#include "ui/panels/MonitorsPanel.h"

#include <windows.h>
#include <commctrl.h>

#include <cwchar>
#include <filesystem>

#include "ui/Theme.h"

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

    // Mode radios
    radioInd_ = ctl(hwnd_, L"BUTTON", L"Independent", WS_VISIBLE | BS_AUTORADIOBUTTON,
                    12, 8, ::MulDiv(120, L.u, 5), L.cy, reinterpret_cast<HMENU>(kCtlIndependent));
    radioClone_ = ctl(hwnd_, L"BUTTON", L"Clone", WS_VISIBLE | BS_AUTORADIOBUTTON,
                      12 + ::MulDiv(130, L.u, 5), 8, ::MulDiv(80, L.u, 5), L.cy,
                      reinterpret_cast<HMENU>(kCtlClone));
    ::SendMessageW(radioInd_, BM_SETCHECK, BST_CHECKED, 0);

    // Monitor list
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

    // Source combo
    const int row2 = L.y(3);
    ctl(hwnd_, L"STATIC", L"Wallpaper source :", WS_VISIBLE, 12, row2,
        ::MulDiv(120, L.u, 5), L.cy, nullptr);
    sourceCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                       12 + ::MulDiv(125, L.u, 5), row2, ::MulDiv(260, L.u, 5), 300,
                       reinterpret_cast<HMENU>(kCtlSourceCombo));

    // Scaling combo
    const int row3 = row2 + L.cy + L.u;
    ctl(hwnd_, L"STATIC", L"Scaling :", WS_VISIBLE, 12, row3, ::MulDiv(120, L.u, 5),
        L.cy, nullptr);
    scalingCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                        12 + ::MulDiv(125, L.u, 5), row3, ::MulDiv(120, L.u, 5), 200,
                        reinterpret_cast<HMENU>(kCtlScalingCombo));
    for (const wchar_t* s : {L"Fill", L"Fit", L"Stretch", L"Center"}) {
        ::SendMessageW(scalingCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    }
    ::SendMessageW(scalingCombo_, CB_SETCURSEL, 0, 0);

    // #18: Per-monitor volume slider
    const int row4 = row3 + L.cy + L.u;
    ctl(hwnd_, L"STATIC", L"Volume :", WS_VISIBLE, 12, row4,
        ::MulDiv(80, L.u, 5), L.cy, nullptr);
    sliderVolume_ = ::CreateWindowExW(0, TRACKBAR_CLASS, L"",
                                      WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS,
                                      12 + ::MulDiv(85, L.u, 5), row4,
                                      ::MulDiv(200, L.u, 5), L.cy, hwnd_,
                                      reinterpret_cast<HMENU>(kSlVolume),
                                      ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(sliderVolume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    ::SendMessageW(sliderVolume_, TBM_SETPOS, TRUE, 80);
    ::SendMessageW(sliderVolume_, TBM_SETTICFREQ, 10, 0);
    if (font_) ::SendMessageW(sliderVolume_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    volumeLabel_ = ctl(hwnd_, L"STATIC", L"80%", WS_VISIBLE,
                       12 + ::MulDiv(290, L.u, 5), row4,
                       ::MulDiv(50, L.u, 5), L.cy, nullptr);

    // Preview + buttons
    preview_ = ctl(hwnd_, L"STATIC", L"(no preview)", WS_VISIBLE | SS_CENTERIMAGE,
                   12, row3 + L.cy + L.u, ::MulDiv(320, L.u, 5), ::MulDiv(180, L.u, 5), nullptr);
    btnPreview_ = ctl(hwnd_, L"BUTTON", L"Preview", WS_VISIBLE | BS_PUSHBUTTON,
                      12 + ::MulDiv(330, L.u, 5), row3 + L.cy + L.u,
                      ::MulDiv(110, L.u, 5), L.cy, reinterpret_cast<HMENU>(kBtnPreview));
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
        std::swprintf(buf, 64, L"%ls", m.primary ? L"\u2714" : L"");
        sub.iSubItem = 3;
        ::SendMessageW(list_, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&sub));
        ++row;
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
}

void MonitorsPanel::rebuildSourceCombo(const UiSnapshot& s) {
    library_ = s.libraryItems;
    ::SendMessageW(sourceCombo_, CB_RESETCONTENT, 0, 0);
    ::SendMessageW(sourceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Playlist (default)"));
    for (const auto& item : library_) {
        const std::wstring name = std::filesystem::path(item.path).filename().wstring();
        ::SendMessageW(sourceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
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
    perMonitorScaling_ = s.config.perMonitorScaling; // #23
    rebuildMonitors();
    rebuildSourceCombo(s);
    if (!s.assignments.empty()) {
        updateReadback(s.assignments.front());
    }
    // #23: initialize scaling combo from per-monitor config or global default.
    ScalingMode selScaling = s.config.scaling; // global default
    if (!monitors_.empty()) {
        const auto it = perMonitorScaling_.find(monitors_[0].id);
        if (it != perMonitorScaling_.end()) {
            selScaling = it->second;
        }
    }
    {
        int sIdx = 0;
        switch (selScaling) {
            case ScalingMode::Fill: sIdx = 0; break;
            case ScalingMode::Fit: sIdx = 1; break;
            case ScalingMode::Stretch: sIdx = 2; break;
            case ScalingMode::Center: sIdx = 3; break;
        }
        ::SendMessageW(scalingCombo_, CB_SETCURSEL, sIdx, 0);
    }
    // #18: initialize volume slider from per-monitor config or global default.
    int vol = s.config.volume; // global default
    if (!monitors_.empty()) {
        const auto& pmv = s.config.perMonitorVolume;
        if (const auto it = pmv.find(monitors_[0].id); it != pmv.end()) {
            vol = it->second;
        }
    }
    ::SendMessageW(sliderVolume_, TBM_SETPOS, TRUE, vol);
    wchar_t vBuf[16];
    std::swprintf(vBuf, 16, L"%d%%", vol);
    ::SetWindowTextW(volumeLabel_, vBuf);
}

void MonitorsPanel::onMonitorEvent(const MonitorEvent&) {}

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
    c.s1.clear();
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
                        c.scaling = static_cast<ScalingMode>(
                            ::SendMessageW(self->scalingCombo_, CB_GETCURSEL, 0, 0));
                        // #23: send per-monitor scaling for the selected monitor.
                        const int sel = static_cast<int>(
                            ::SendMessageW(self->list_, LVM_GETSELECTIONMARK, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(self->monitors_.size())) {
                            c.s1 = self->monitors_[sel].id;
                        } else {
                            c.s1.clear();
                        }
                        self->post_(c);
                    }
                    return 0;
            }
            return 0;
        case WM_HSCROLL: {
            if (reinterpret_cast<HWND>(lParam) == self->sliderVolume_) {
                const int pos = static_cast<int>(::SendMessageW(self->sliderVolume_, TBM_GETPOS, 0, 0));
                wchar_t vBuf[16];
                std::swprintf(vBuf, 16, L"%d%%", pos);
                ::SetWindowTextW(self->volumeLabel_, vBuf);
                // #18: Post CONFIG_SET for the selected monitor's volume.
                if (!self->monitors_.empty()) {
                    const int sel = static_cast<int>(
                        ::SendMessageW(self->list_, LVM_GETSELECTIONMARK, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(self->monitors_.size())) {
                        Command c;
                        c.id = CommandId::ConfigSet;
                        c.s1 = L"pmvolume:" + self->monitors_[sel].id;
                        c.s2 = std::to_wstring(pos);
                        self->post_(c);
                    }
                }
            }
            return 0;
        }
        case WM_NOTIFY: {
            const auto* nmhdr = reinterpret_cast<LPNMHDR>(lParam);
            if (nmhdr->hwndFrom == self->list_ && nmhdr->code == LVN_ITEMCHANGED) {
                const auto* plv = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if ((plv->uChanged & LVIF_STATE) && (plv->uNewState & LVIS_SELECTED) &&
                    plv->iItem >= 0 && plv->iItem < static_cast<int>(self->monitors_.size())) {
                    // #23: update scaling combo to reflect the selected monitor's scaling.
                    const auto& monId = self->monitors_[plv->iItem].id;
                    ScalingMode selScaling = self->scaling_; // global default
                    const auto it = self->perMonitorScaling_.find(monId);
                    if (it != self->perMonitorScaling_.end()) {
                        selScaling = it->second;
                    }
                    int sIdx = 0;
                    switch (selScaling) {
                        case ScalingMode::Fill: sIdx = 0; break;
                        case ScalingMode::Fit: sIdx = 1; break;
                        case ScalingMode::Stretch: sIdx = 2; break;
                        case ScalingMode::Center: sIdx = 3; break;
                    }
                    ::SendMessageW(self->scalingCombo_, CB_SETCURSEL, sIdx, 0);
                }
            }
            break;
        }
        case WM_SIZE: {
            self->layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
