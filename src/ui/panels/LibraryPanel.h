#pragma once

#include <functional>
#include <map>
#include <string>

#include "ui/panels/Panel.h"

namespace vw::ui {

// Library panel (spec §10.3, v1 minimal): details ListView (Name · Duration ·
// Resolution · FPS · Codec · HDR · Size), toolbar (Add Files… / Add Folder… /
// Remove / Set as wallpaper / Refresh), click-to-sort columns, lazy metadata
// on selection, double-click = set as wallpaper, and a selection summary in
// the status line. Reads arrive via LibraryChange notifications + the
// pull-on-open snapshot; writes are commands. The file/folder dialogs run on
// the UI thread (spec §10.9 allows this).
class LibraryPanel : public Panel {
public:
    using MetadataRequestFn = std::function<void(vw::ui::LibraryItemId)>;
    using ThumbnailRequestFn = std::function<void(const std::wstring& path)>;

    LibraryPanel(PostFn post, MetadataRequestFn requestMetadata,
                 ThumbnailRequestFn requestThumbnail = nullptr)
        : Panel(std::move(post)), requestMetadata_(std::move(requestMetadata)),
          requestThumbnail_(std::move(requestThumbnail)) {}

    bool create(HWND parent) override;
    void onLibraryChange(const LibraryChangeNotification&) override;
    void refreshFromSnapshot(const UiSnapshot&) override;
    void relayout() override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout(int width, int height);
    void rebuildList();
    void applySort();
    void updateSelectionStatus();
    void requestMetadataForSelected();
    void addFilesDialog();
    void addFolderDialog();
    void removeSelected();
    void setWallpaperForSelection();
public:
    void setThumbnail(const std::wstring& videoPath, const std::wstring& bmpPath);
private:
    static std::wstring sizeText(uint64_t bytes);

    enum Btn : UINT { kBtnAddFiles = 101, kBtnAddFolder = 102, kBtnRemove = 103,
                      kBtnSetWallpaper = 104, kBtnRefresh = 105 };
    enum Col : int { kColName = 0, kColDuration, kColResolution, kColFps, kColCodec,
                     kColHdr, kColSize, kColCount };

    MetadataRequestFn requestMetadata_;
    ThumbnailRequestFn requestThumbnail_;
    std::vector<LibraryItem> library_; // local copy (sorted)
    void* imageList_ = nullptr; // HIMAGELIST (opaque; commctrl.h in .cpp only)
    std::map<std::wstring, std::wstring> thumbnailCache_; // path -> cached BMP path
    int sortCol_ = kColName;
    bool sortAsc_ = true;
    HWND list_ = nullptr;
    HWND status_ = nullptr;
    std::vector<HWND> buttons_;
    std::vector<HWND> listColumns_; // header columns (kept for the leak gate)
};

} // namespace vw::ui
