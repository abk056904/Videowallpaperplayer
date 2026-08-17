#include "system/SystemStateMonitor.h"

#include <wtsapi32.h>

#include "logging/Logger.h"

namespace vw::system {

SystemStateMonitor::SystemStateMonitor(HWND notificationWindow)
    : window_(notificationWindow) {}

void SystemStateMonitor::start() {
    if (!window_) {
        return;
    }
    // Session lock/unlock (WM_WTSSESSION_CHANGE). The window must live on a
    // message pump; notifications are delivered as window messages.
    if (::WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) != FALSE) {
        sessionRegistered_ = true;
    } else {
        log::Logger::instance().warn(L"WTSRegisterSessionNotification failed — "
                                     L"lock/unlock detection degraded");
    }
    // Monitor power-off/on (WM_POWERBROADCAST with PBT_POWERSETTINGCHANGE).
    // Non-fatal if unavailable (the DisplayOff reason simply never latches —
    // the honest signal for unsupported setups).
    monitorPowerNotify_ = ::RegisterPowerSettingNotification(
        window_, &GUID_MONITOR_POWER_ON, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!monitorPowerNotify_) {
        log::Logger::instance().warn(L"RegisterPowerSettingNotification failed — "
                                     L"display-off detection degraded");
    }
}

void SystemStateMonitor::stop() {
    if (monitorPowerNotify_) {
        ::UnregisterPowerSettingNotification(monitorPowerNotify_);
        monitorPowerNotify_ = nullptr;
    }
    if (sessionRegistered_) {
        ::WTSUnRegisterSessionNotification(window_);
        sessionRegistered_ = false;
    }
}

bool SystemStateMonitor::defaultBatteryQuery() {
    SYSTEM_POWER_STATUS sps{};
    if (::GetSystemPowerStatus(&sps) == FALSE) {
        return false; // unknown — treat as AC (no battery pause)
    }
    // ACLineStatus==0 (offline) AND a battery exists (BatteryFlag != 128 =
    // no system battery). Desktops report unknown/128 — never "on battery".
    return sps.ACLineStatus == 0 && (sps.BatteryFlag & 0x80) == 0;
}

uint32_t SystemStateMonitor::updateBatteryReason(uint32_t& reasons, BatteryQuery query) {
    const bool battery = query ? query() : defaultBatteryQuery();
    const bool wasSet = (reasons & governor::Reason::Battery) != 0;
    if (battery && !wasSet) {
        reasons |= governor::Reason::Battery;
        return governor::Reason::Battery;
    }
    if (!battery && wasSet) {
        reasons &= ~governor::Reason::Battery;
        return governor::Reason::Battery;
    }
    return 0;
}

uint32_t SystemStateMonitor::translate(UINT msg, WPARAM wParam, LPARAM lParam,
                                       uint32_t& reasons) {
    uint32_t changed = 0;
    const auto set = [&](uint32_t bit, bool on) {
        const bool was = (reasons & bit) != 0;
        if (on && !was) {
            reasons |= bit;
            changed |= bit;
        } else if (!on && was) {
            reasons &= ~bit;
            changed |= bit;
        }
    };

    switch (msg) {
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) {
                set(governor::Reason::Locked, true);
            } else if (wParam == WTS_SESSION_UNLOCK) {
                set(governor::Reason::Locked, false);
            }
            break;
        case WM_POWERBROADCAST:
            switch (wParam) {
                case PBT_APMSUSPEND:
                    set(governor::Reason::SystemSuspended, true);
                    break;
                case PBT_APMRESUMEAUTOMATIC:
                case PBT_APMRESUMESUSPEND:
                    set(governor::Reason::SystemSuspended, false);
                    break;
                case PBT_POWERSETTINGCHANGE: {
                    // GUID_MONITOR_POWER_ON: display on/off. The setting data
                    // is a DWORD (1 = on, 0 = off).
                    const auto* data = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
                    if (data && data->DataLength >= sizeof(DWORD)) {
                        const DWORD on = *reinterpret_cast<const DWORD*>(data->Data);
                        set(governor::Reason::DisplayOff, on == 0);
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return changed;
}

} // namespace vw::system
