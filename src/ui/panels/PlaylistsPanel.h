#pragma once

#include "ui/panels/Panel.h"

namespace vw::ui {

// Playlists panel (spec §10.4, v1 scope): the engine has ONE playlist
// (shared-playlist mode is v2), so this panel manages its items — add file,
// remove, move up/down, enable/disable — plus the mode combo (Single /
// Sequential / Loop playlist / Shuffle) and the Loop checkbox. Item changes
// persist via the engine's debounced PlaylistStore save. Reads arrive from
// the snapshot (playlistItems + playlists summaries) and PlaylistChange
// notifications.
class PlaylistsPanel : public Panel {
public:
    // RefreshFn re-pulls the playlist (items + mode + loop) from the engine
    // — the read path the panel uses when a PlaylistChange arrives (the
    // notification itself carries no item payload). Provided by the app.
    using RefreshFn = std::function<void()>;

    PlaylistsPanel(PostFn post, RefreshFn refresh)
        : Panel(std::move(post)), refresh_(std::move(refresh)) {}

    bool create(HWND parent) override;
    void refreshFromSnapshot(const UiSnapshot&) override;
    void onPlaylistChange(const PlaylistChangeNotification&) override;
    void relayout() override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void layout(int width, int height);
    void rebuildItems();

    RefreshFn refresh_;
    void updateBottom();
    void addFileDialog();
    void removeSelected();
    void moveSelected(int delta);
    void toggleSelected();
    void setModeFromCombo(int index);
    void setLoopFromCheck(bool on);
    int selectedRow() const;

    enum Btn : UINT { kBtnAddFile = 101, kBtnRemove = 102, kBtnUp = 103, kBtnDown = 104,
                      kBtnToggle = 105 };
    enum Ctrl : UINT { kCtlModeCombo = 201, kCtlLoopCheck = 202 };

    std::vector<PlaylistItemView> items_;
    PlaylistMode mode_ = PlaylistMode::Sequential;
    bool loop_ = true;
    HWND list_ = nullptr;
    HWND btnAdd_ = nullptr, btnRemove_ = nullptr, btnUp_ = nullptr, btnDown_ = nullptr,
         btnToggle_ = nullptr;
    HWND modeCombo_ = nullptr, loopCheck_ = nullptr;
    std::vector<HWND> buttons_;
};

} // namespace vw::ui
