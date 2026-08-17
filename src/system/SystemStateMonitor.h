#pragma once

#include <windows.h>

#include "governor/PausePolicy.h"

// System-state notifications for the ResourceGovernor (docs/03 §3.12, M10):
// session lock/unlock (WTSRegisterSessionNotification), suspend/resume +
// monitor power (WM_POWERBROADCAST / RegisterPowerSettingNotification), and
// AC/battery (GetSystemPowerStatus — read on notification, not polled).
//
// The app routes its control-window messages through translate(), which
// MUTATES the caller's reason mask (set or clear the Locked/DisplayOff/
// SystemSuspended bits) and returns the bits that CHANGED (for logging). The
// caller (ResourceGovernor) owns the authoritative mask. Battery state is
// queried lazily (on power-status change and on demand) — never polled; the
// query is injectable for unit tests.

namespace vw::system {

class SystemStateMonitor {
public:
    // Battery query: true = running on battery (AC offline). Default impl
    // uses GetSystemPowerStatus; tests substitute a fake.
    using BatteryQuery = bool (*)();

    explicit SystemStateMonitor(HWND notificationWindow);

    // Registers WTS session notifications + the monitor-power setting
    // notification. Must be called with a window that lives on a message
    // pump. Idempotent; failures are non-fatal (the monitor degrades to the
    // fallback reads below).
    void start();
    void stop();

    // Routes one control-window message into `reasons` (set/clear the
    // Locked / DisplayOff / SystemSuspended bits). Returns the bits that
    // changed, for logging. Handles WM_WTSSESSION_CHANGE, WM_POWERBROADCAST
    // (suspend/resume + monitor power); everything else returns 0 untouched.
    uint32_t translate(UINT msg, WPARAM wParam, LPARAM lParam, uint32_t& reasons);

    // Reads the CURRENT battery state into the Battery reason bit. Called on
    // power-status changes (and at governor startup); cheap, never polled.
    // Returns the bits that changed.
    uint32_t updateBatteryReason(uint32_t& reasons, BatteryQuery query = defaultBatteryQuery);

    static bool defaultBatteryQuery();

private:
    HWND window_ = nullptr;
    bool sessionRegistered_ = false;
    HANDLE monitorPowerNotify_ = nullptr;
};

} // namespace vw::system
