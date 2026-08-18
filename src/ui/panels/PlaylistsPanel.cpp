#include "ui/panels/PlaylistsPanel.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cstdio>
#include <cwchar>
#include <filesystem>

namespace vw::ui {

const wchar_t* PlaylistsPanel::kClassName = L"VideoWallpaper.PlaylistsPanel";

bool PlaylistsPanel::create(HWND parent) {
    if (hwnd_) {
        return true;
    }
    hwnd_ = createPanelWindow(parent, kClassName, &PlaylistsPanel::wndProc, this);
    if (!hwnd_) {
        return false;
    }
    initPanelFont();
    const Layout L = Layout::from(hwnd_);

    makeButtons(hwnd_, font_,
                {{kBtnAddFile, L"Add File..."}, {kBtnRemove, L"Remove"}, {kBtnUp, L"Up"},
                 {kBtnDown, L"Down"}, {kBtnToggle, L"Enable/Disable"}},
                buttons_, 8, L.u, L.cy);

    // Items ListView: # / Name / Start / End / On.
    list_ = ctl(hwnd_, WC_LISTVIEW, L"",
                WS_VISIBLE | WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 8, L.y(1),
                400, 300, nullptr);
    ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* titles[] = {L"#", L"Name", L"Start", L"End", L"On"};
    for (int c = 0; c < 5; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(titles[c]);
        col.cx = (c == 1) ? ::MulDiv(280, L.u, 5) : ::MulDiv(60, L.u, 5);
        ::SendMessageW(list_, LVM_INSERTCOLUMNW, c, reinterpret_cast<LPARAM>(&col));
    }

    // Bottom bar: mode combo + loop checkbox.
    modeCombo_ = ctl(hwnd_, WC_COMBOBOX, L"", WS_VISIBLE | CBS_DROPDOWNLIST,
                     L.x(0) + 8, 0, ::MulDiv(150, L.u, 5), 200, reinterpret_cast<HMENU>(kCtlModeCombo));
    for (const wchar_t* m : {L"Single", L"Sequential", L"Loop playlist", L"Shuffle"}) {
        ::SendMessageW(modeCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m));
    }
    loopCheck_ = ctl(hwnd_, L"BUTTON", L"Loop", WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0,
                     ::MulDiv(80, L.u, 5), L.cy, reinterpret_cast<HMENU>(kCtlLoopCheck));
    ::SendMessageW(loopCheck_, BM_SETCHECK, loop_ ? BST_CHECKED : BST_UNCHECKED, 0);
    return true;
}

void PlaylistsPanel::relayout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    layout(rc.right - rc.left, rc.bottom - rc.top);
}

void PlaylistsPanel::layout(int width, int height) {
    const Layout L = Layout::from(hwnd_);
    int x = 8;
    for (HWND b : buttons_) {
        RECT r{};
        ::GetWindowRect(b, &r);
        ::MoveWindow(b, x, 8, r.right - r.left, L.cy, TRUE);
        x += (r.right - r.left) + L.u;
    }
    const int barY = height - L.cy - 6;
    ::MoveWindow(modeCombo_, 12, barY, ::MulDiv(150, L.u, 5), 200, TRUE);
    ::MoveWindow(loopCheck_, 12 + ::MulDiv(160, L.u, 5), barY, ::MulDiv(80, L.u, 5), L.cy, TRUE);
    const int listY = 8 + L.cy + L.u;
    ::MoveWindow(list_, 8, listY, width - 16, barY - listY - 4, TRUE);
}

void PlaylistsPanel::rebuildItems() {
    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    int row = 0;
    for (const auto& item : items_) {
        wchar_t buf[64];
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = row;
        // Name column (1) shows the video's FILE NAME (the full path is too
        // wide for the column and is what the row's tooltip would need — not
        // the display text). Column 0 (#) is set right after via setSub(0).
        const std::wstring name = std::filesystem::path(item.path).filename().wstring();
        lv.pszText = const_cast<wchar_t*>(name.c_str());
        lv.lParam = row;
        ::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lv));
        LVITEMW sub{};
        sub.pszText = buf;
        const auto setSub = [&](int col, const wchar_t* text) {
            // Truncate (never fail-empty): filenames can exceed the 64-char
            // buffer and wcscpy_s would leave the cell blank on overflow.
            ::wcsncpy_s(buf, 64, text, _TRUNCATE);
            sub.iSubItem = col;
            ::SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                           reinterpret_cast<LPARAM>(&sub));
        };
        std::swprintf(buf, 64, L"%d", row + 1);
        setSub(0, buf);
        setSub(1, name.c_str()); // was never populated — the list showed an empty Name column
        std::swprintf(buf, 64, L"%ls", item.start100ns > 0 ? L"trim" : L"-");
        setSub(2, buf);
        std::swprintf(buf, 64, L"%ls", item.end100ns > 0 ? L"trim" : L"-");
        setSub(3, buf);
        std::swprintf(buf, 64, L"%ls", item.enabled ? L"☑" : L"☐");
        setSub(4, buf);
        ++row;
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list_, nullptr, TRUE);
}

void PlaylistsPanel::updateBottom() {
    const int idx = static_cast<int>(::SendMessageW(modeCombo_, CB_GETCURSEL, 0, 0));
    int want = 0;
    switch (mode_) {
        case PlaylistMode::Single: want = 0; break;
        case PlaylistMode::Sequential: want = 1; break;
        case PlaylistMode::Loop: want = 2; break;
        case PlaylistMode::Shuffle: want = 3; break;
    }
    if (idx != want) {
        ::SendMessageW(modeCombo_, CB_SETCURSEL, want, 0);
    }
    ::SendMessageW(loopCheck_, BM_SETCHECK, loop_ ? BST_CHECKED : BST_UNCHECKED, 0);
}

void PlaylistsPanel::refreshFromSnapshot(const UiSnapshot& s) {
    items_ = s.playlistItems;
    if (!s.playlists.empty()) {
        mode_ = s.playlists.front().mode;
        loop_ = s.playlists.front().loop;
    }
    rebuildItems();
    updateBottom();
}

void PlaylistsPanel::onPlaylistChange(const PlaylistChangeNotification&) {
    // The notification carries no item payload — re-pull the playlist from
    // the engine (read path; the panel never polls on a timer).
    if (refresh_) {
        refresh_();
    }
}

int PlaylistsPanel::selectedRow() const {
    return static_cast<int>(
        ::SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

void PlaylistsPanel::addFileDialog() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter =
        L"Video files\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.ts;*.mpg;*.mpeg\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Add video to playlist";
    if (::GetOpenFileNameW(&ofn)) {
        Command c;
        c.id = CommandId::PlaylistAddFiles;
        c.paths.push_back(file);
        post_(c);
    }
}

void PlaylistsPanel::removeSelected() {
    const int row = selectedRow();
    if (row < 0) {
        return;
    }
    Command c;
    c.id = CommandId::PlaylistRemoveItem;
    c.i1 = row;
    post_(c);
}

void PlaylistsPanel::moveSelected(int delta) {
    const int row = selectedRow();
    if (row < 0) {
        return;
    }
    Command c;
    c.id = CommandId::PlaylistMoveItem;
    c.i1 = row;
    c.i2 = delta;
    post_(c);
}

void PlaylistsPanel::toggleSelected() {
    const int row = selectedRow();
    if (row < 0 || row >= static_cast<int>(items_.size())) {
        return;
    }
    Command c;
    c.id = CommandId::PlaylistToggleItem;
    c.i1 = row;
    c.b1 = !items_[row].enabled;
    post_(c);
}

void PlaylistsPanel::setModeFromCombo(int index) {
    Command c;
    c.id = CommandId::PlaylistSetMode;
    c.mode = static_cast<PlaylistMode>(index);
    post_(c);
}

void PlaylistsPanel::setLoopFromCheck(bool on) {
    Command c;
    c.id = CommandId::PlaylistSetLoop;
    c.b1 = on;
    post_(c);
}

LRESULT CALLBACK PlaylistsPanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PlaylistsPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PlaylistsPanel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kBtnAddFile:
                    self->addFileDialog();
                    return 0;
                case kBtnRemove:
                    self->removeSelected();
                    return 0;
                case kBtnUp:
                    self->moveSelected(-1);
                    return 0;
                case kBtnDown:
                    self->moveSelected(+1);
                    return 0;
                case kBtnToggle:
                    self->toggleSelected();
                    return 0;
                case kCtlModeCombo:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        self->setModeFromCombo(
                            static_cast<int>(::SendMessageW(self->modeCombo_, CB_GETCURSEL, 0, 0)));
                    }
                    return 0;
                case kCtlLoopCheck:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        const LRESULT on = ::SendMessageW(self->loopCheck_, BM_GETCHECK, 0, 0);
                        self->setLoopFromCheck(on == BST_CHECKED);
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
