#pragma once

#include "ui/panels/Panel.h"

namespace vw::ui {

// Settings panel (spec §10.7): Start with Windows (HKCU Run), Minimize to
// tray on close, logging level combo (INFO/DEBUG), and the About block
// (version, build config, toolchain, Open README). All writes are CONFIG_SET;
// the README button is UI-local (ShellExecute).
class SettingsPanel : public Panel {
public:
    explicit SettingsPanel(PostFn post) : Panel(std::move(post)) {}

    bool create(HWND parent) override;
    void refreshFromSnapshot(const UiSnapshot&) override;
    void relayout() override;

    static const wchar_t* kClassName;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    void updateFromConfig(const ConfigSnapshot&);

    enum Ctrl : UINT { kCkStartup = 501, kCkTray = 502, kCmLogLevel = 503, kBtnReadme = 504, kCmSpeed = 505, kCkAudio = 506, kSlVolume = 507, kCkFileAssoc = 508 };

    HWND checkStartup_ = nullptr, checkTray_ = nullptr;
    HWND logCombo_ = nullptr, btnReadme_ = nullptr, aboutText_ = nullptr;
    HWND speedCombo_ = nullptr;
    HWND checkAudio_ = nullptr, sliderVolume_ = nullptr, volumeLabel_ = nullptr;
    HWND checkFileAssoc_ = nullptr;
};

} // namespace vw::ui
