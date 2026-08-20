#include "ui/panels/LibraryPanel.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>

namespace vw::ui {

const wchar_t* LibraryPanel::kClassName = L"VideoWallpaper.LibraryPanel";

namespace {
const wchar_t* kColumnTitles[7] = {L"Name", L"Duration", L"Resolution", L"FPS",
                                   L"Codec", L"HDR", L"Size"};
}

bool LibraryPanel::create(HWND parent) {
    if (hwnd_) {
        return true;
    }
    hwnd_ = createPanelWindow(parent, kClassName, &LibraryPanel::wndProc, this);
    if (!hwnd_) {
        return false;
    }
    initPanelFont();
    const Layout L = Layout::from(hwnd_);

    // Toolbar.
    makeButtons(hwnd_, font_,
                {{kBtnAddFiles, L"Add Files..."}, {kBtnAddFolder, L"Add Folder..."},
                 {kBtnRemove, L"Remove"}, {kBtnSetWallpaper, L"Set as wallpaper"},
                 {kBtnRefresh, L"Refresh"}},
                buttons_, 8, L.u, L.cy);

    // Details ListView.
    list_ = ctl(hwnd_, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | LVS_REPORT | LVS_SINGLESEL |
                                          LVS_SHOWSELALWAYS,
                8, L.y(1), 400, 300, nullptr);
    ::SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE,
                   LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (int c = 0; c < kColCount; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(kColumnTitles[c]);
        col.cx = (c == kColName) ? ::MulDiv(220, L.u, 5) : ::MulDiv(80, L.u, 5);
        col.iSubItem = c;
        ::SendMessageW(list_, LVM_INSERTCOLUMNW, c, reinterpret_cast<LPARAM>(&col));
    }
    // Status line.
    status_ = ctl(hwnd_, L"STATIC", L"", WS_VISIBLE, 8, 0, 400, L.cy, nullptr);
    return true;
}

void LibraryPanel::relayout() {
    if (!hwnd_) {
        return;
    }
    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    layout(rc.right - rc.left, rc.bottom - rc.top);
}

void LibraryPanel::layout(int width, int height) {
    const Layout L = Layout::from(hwnd_);
    const int btnY = 8;
    int x = 8;
    for (HWND b : buttons_) {
        RECT r{};
        ::GetWindowRect(b, &r);
        ::MoveWindow(b, x, btnY, r.right - r.left, L.cy, TRUE);
        x += (r.right - r.left) + L.u;
    }
    const int listY = btnY + L.cy + L.u;
    const int statusH = L.cy;
    const int statusY = height - statusH - 6;
    ::MoveWindow(list_, 8, listY, width - 16, statusY - listY - 4, TRUE);
    ::MoveWindow(status_, 8, statusY, width - 16, statusH, TRUE);
}

void LibraryPanel::applySort() {
    const auto& col = sortCol_;
    std::stable_sort(library_.begin(), library_.end(),
                     [&](const LibraryItem& a, const LibraryItem& b) {
                         int cmp = 0;
                         switch (col) {
                             case kColName:
                                 cmp = a.path.compare(b.path);
                                 break;
                             case kColDuration:
                                 cmp = (a.metadata.durationSeconds < b.metadata.durationSeconds)
                                           ? -1
                                           : (a.metadata.durationSeconds > b.metadata.durationSeconds);
                                 break;
                             case kColResolution:
                                 cmp = (a.metadata.width * a.metadata.height <
                                        b.metadata.width * b.metadata.height)
                                           ? -1
                                           : (a.metadata.width * a.metadata.height >
                                              b.metadata.width * b.metadata.height);
                                 break;
                             case kColFps:
                                 cmp = (a.metadata.frameRate < b.metadata.frameRate)
                                           ? -1
                                           : (a.metadata.frameRate > b.metadata.frameRate);
                                 break;
                             case kColCodec:
                                 cmp = (a.metadata.codec < b.metadata.codec);
                                 break;
                             case kColHdr:
                                 cmp = (a.metadata.hdr < b.metadata.hdr);
                                 break;
                             case kColSize:
                                 cmp = (a.fileSize < b.fileSize) ? -1
                                                                 : (a.fileSize > b.fileSize);
                                 break;
                         }
                         return sortAsc_ ? cmp < 0 : cmp > 0;
                     });
}

void LibraryPanel::rebuildList() {
    ::SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ::SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
    for (const auto& item : library_) {
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = INT_MAX;
        lv.pszText = const_cast<wchar_t*>(item.path.c_str());
        lv.lParam = static_cast<LPARAM>(item.id);
        const int row = static_cast<int>(
            ::SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lv)));
        // Subitems: duration / resolution / fps / codec / hdr / size.
        // LVM_SETITEMTEXT takes a pointer to an LVITEM (iSubItem + pszText).
        wchar_t buf[64];
        LVITEMW sub{};
        sub.pszText = buf;
        const auto setSub = [&](int col, const wchar_t* text) {
            ::wcscpy_s(buf, 64, text);
            sub.iSubItem = col;
            ::SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row),
                           reinterpret_cast<LPARAM>(&sub));
        };
        setSub(kColDuration, formatDuration(item.metadata.durationSeconds).c_str());
        if (item.metadata.width > 0) {
            std::swprintf(buf, 64, L"%ux%u", item.metadata.width, item.metadata.height);
            setSub(kColResolution, buf);
        } else {
            setSub(kColResolution, L"");
        }
        setSub(kColFps, formatFps(item.metadata.frameRate).c_str());
        static const wchar_t* kCodecNames[] = {L"-", L"H.264", L"HEVC", L"AV1", L"VP9"};
        setSub(kColCodec, kCodecNames[static_cast<int>(item.metadata.codec)]);
        setSub(kColHdr, item.metadata.hdr ? L"Yes" : L"No");
        setSub(kColSize, sizeText(item.fileSize).c_str());
    }
    ::SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(list_, nullptr, TRUE);
}

std::wstring LibraryPanel::sizeText(uint64_t bytes) {
    if (bytes == 0) {
        return L"-";
    }
    wchar_t buf[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::swprintf(buf, 32, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        std::swprintf(buf, 32, L"%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::swprintf(buf, 32, L"%.0f KB", bytes / 1024.0);
    }
    return buf;
}

void LibraryPanel::refreshFromSnapshot(const UiSnapshot& s) {
    library_ = s.libraryItems;
    applySort();
    rebuildList();
    updateSelectionStatus();
}

void LibraryPanel::onLibraryChange(const LibraryChangeNotification& n) {
    switch (n.kind) {
        case LibraryChangeKind::Added:
            for (const auto id : n.ids) {
                const auto src = std::find_if(library_.begin(), library_.end(),
                                              [&](const LibraryItem& i) { return i.id == id; });
                if (src == library_.end()) {
                    // Pull the new item from the engine snapshot on the next
                    // refresh; the list self-heals via refreshFromSnapshot.
                    continue;
                }
                library_.push_back(*src);
            }
            break;
        case LibraryChangeKind::Removed:
            library_.erase(std::remove_if(library_.begin(), library_.end(),
                                          [&](const LibraryItem& i) {
                                              return std::find(n.ids.begin(), n.ids.end(),
                                                               i.id) != n.ids.end();
                                          }),
                           library_.end());
            break;
        case LibraryChangeKind::Updated:
            // Metadata may have arrived — mark the panel to re-read on the
            // next snapshot; simple: rebuild now with local data unchanged.
            break;
        case LibraryChangeKind::RescanStarted:
        case LibraryChangeKind::RescanFinished:
            break;
    }
    applySort();
    rebuildList();
    updateSelectionStatus();
}

void LibraryPanel::updateSelectionStatus() {
    wchar_t buf[256];
    std::swprintf(buf, 256, L"%zu items", library_.size());
    ::SetWindowTextW(status_, buf);
}

void LibraryPanel::requestMetadataForSelected() {
    const int sel = static_cast<int>(::SendMessageW(
        list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (sel < 0) {
        return;
    }
    LVITEMW lv{};
    lv.mask = LVIF_PARAM;
    lv.iItem = sel;
    if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lv))) {
        if (requestMetadata_) {
            requestMetadata_(static_cast<vw::ui::LibraryItemId>(lv.lParam));
        }
    }
}

void LibraryPanel::addFilesDialog() {
    wchar_t files[16384] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Video files\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.ts;*.mpg;*.mpeg\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = files;
    ofn.nMaxFile = 16384;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Add videos to library";
    if (!::GetOpenFileNameW(&ofn)) {
        return;
    }
    std::vector<std::wstring> paths;
    // OFN_EXPLORER multi-select: directory first, then NUL-separated names.
    const std::wstring dir(files);
    if (!dir.empty()) {
        wchar_t* p = files + dir.size() + 1;
        if (*p) {
            while (*p) {
                paths.push_back(dir + L"\\" + p);
                p += std::wcslen(p) + 1;
            }
        } else {
            paths.push_back(dir);
        }
    }
    if (!paths.empty()) {
        Command c;
        c.id = CommandId::LibraryAddFiles;
        c.paths = std::move(paths);
        post_(c);
    }
}

void LibraryPanel::addFolderDialog() {
    wchar_t folder[MAX_PATH] = {};
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd_;
    bi.lpszTitle = L"Add a folder to the library (watched for changes)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    if (const auto* pidl = ::SHBrowseForFolderW(&bi)) {
        if (::SHGetPathFromIDListW(pidl, folder)) {
            Command c;
            c.id = CommandId::LibraryAddFolder;
            c.s1 = folder;
            post_(c);
        }
        ::CoTaskMemFree(const_cast<ITEMIDLIST*>(pidl));
    }
}

void LibraryPanel::removeSelected() {
    std::vector<vw::ui::LibraryItemId> ids;
    int sel = static_cast<int>(::SendMessageW(
        list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    while (sel >= 0) {
        LVITEMW lv{};
        lv.mask = LVIF_PARAM;
        lv.iItem = sel;
        if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lv))) {
            ids.push_back(static_cast<vw::ui::LibraryItemId>(lv.lParam));
        }
        sel = static_cast<int>(::SendMessageW(
            list_, LVM_GETNEXTITEM, static_cast<WPARAM>(sel), LVNI_SELECTED));
    }
    if (!ids.empty()) {
        Command c;
        c.id = CommandId::LibraryRemove;
        c.itemIds = std::move(ids);
        post_(c);
    }
}

void LibraryPanel::setWallpaperForSelection() {
    const int sel = static_cast<int>(::SendMessageW(
        list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    if (sel < 0) {
        return;
    }
    LVITEMW lv{};
    lv.mask = LVIF_PARAM;
    lv.iItem = sel;
    if (::SendMessageW(list_, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&lv))) {
        Command c;
        c.id = CommandId::SetWallpaperFile;
        c.s1.clear(); // monitor id: empty = primary (v1 single display)
        const auto src = std::find_if(library_.begin(), library_.end(),
                                      [&](const LibraryItem& i) {
                                          return i.id == static_cast<vw::ui::LibraryItemId>(
                                                             lv.lParam);
                                      });
        if (src != library_.end()) {
            c.s2 = src->path;
            post_(c);
        }
    }
}

LRESULT CALLBACK LibraryPanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<LibraryPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<LibraryPanel*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kBtnAddFiles:
                    self->addFilesDialog();
                    return 0;
                case kBtnAddFolder:
                    self->addFolderDialog();
                    return 0;
                case kBtnRemove:
                    self->removeSelected();
                    return 0;
                case kBtnSetWallpaper:
                    self->setWallpaperForSelection();
                    return 0;
                case kBtnRefresh: {
                    Command c;
                    c.id = CommandId::LibraryRefresh;
                    self->post_(c);
                    return 0;
                }
            }
            return 0;
        case WM_NOTIFY: {
            const auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->hwndFrom == self->list_) {
                switch (nmhdr->code) {
                    case LVN_COLUMNCLICK: {
                        const auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                        if (nmlv->iSubItem == self->sortCol_) {
                            self->sortAsc_ = !self->sortAsc_;
                        } else {
                            self->sortCol_ = nmlv->iSubItem;
                            self->sortAsc_ = true;
                        }
                        self->applySort();
                        self->rebuildList();
                        return TRUE;
                    }
                    case LVN_ITEMCHANGED:
                        self->requestMetadataForSelected();
                        self->updateSelectionStatus();
                        return TRUE;
                    case NM_DBLCLK:
                        self->setWallpaperForSelection();
                        return TRUE;
                }
            }
            return 0;
        }
        case WM_SIZE: {
            self->layout(LOWORD(lParam), HIWORD(lParam));
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vw::ui
