#include "doctest.h"

#include <chrono>

#include "config/ConfigurationManager.h"
#include "governor/ResourceGovernor.h"
#include "playback/PlaybackController.h"
#include "system/SystemStateMonitor.h"

using namespace std::chrono_literals;
using vw::governor::ResourceGovernor;
using vw::governor::State;
using vw::governor::Reason;
using vw::governor::PausePolicy;

namespace {

// A governor over a real (unopened) PlaybackController: pause()/stop() are
// safe no-ops on a fresh session; resume() fails gracefully (logged). The
// transition OBSERVER is the assertion surface.
struct Harness {
    vw::playback::PlaybackController playback;
    ResourceGovernor governor{playback, PausePolicy::Config{.longPauseReleaseSeconds = 5}};
    std::vector<std::pair<State, State>> transitions;

    Harness() {
        governor.setActionObserver([this](State from, State to, uint32_t) {
            transitions.emplace_back(from, to);
        });
    }
};

} // namespace

// ---- PausePolicy transition table (docs/03 §95) ----------------------------

TEST_CASE("governor: ACTIVE + game -> PAUSED; clear -> ACTIVE") {
    Harness h;
    CHECK(h.governor.state() == State::Active);

    h.governor.setReason(Reason::Game, true);
    CHECK(h.governor.state() == State::Paused);
    REQUIRE(h.transitions.size() == 1);
    CHECK(h.transitions[0].first == State::Active);
    CHECK(h.transitions[0].second == State::Paused);

    h.governor.setReason(Reason::Game, false);
    CHECK(h.governor.state() == State::Active);
}

TEST_CASE("governor: high-GPU persists -> stays PAUSED until hysteresis clears") {
    Harness h;
    h.governor.setReason(Reason::HighGPU, true);
    CHECK(h.governor.state() == State::Paused);

    // More reasons pile on; the session stays paused.
    h.governor.setReason(Reason::HighCPU, true);
    CHECK(h.governor.state() == State::Paused);
    h.transitions.clear();

    // One workload reason clears; the OTHER is still latched -> still paused.
    h.governor.setReason(Reason::HighCPU, false);
    CHECK(h.governor.state() == State::Paused);
    CHECK(h.transitions.empty());

    // The last reason clears -> ACTIVE.
    h.governor.setReason(Reason::HighGPU, false);
    CHECK(h.governor.state() == State::Active);
}

TEST_CASE("governor: PAUSED long -> SUSPENDED (decoder released)") {
    Harness h;
    auto t = std::chrono::steady_clock::now();
    h.governor.setClockForTest([&t] { return t; });

    h.governor.setReason(Reason::Locked, true);
    CHECK(h.governor.state() == State::Paused);

    // 4 s of pause — not yet the 5 s release.
    t += 4s;
    h.governor.onTick();
    CHECK(h.governor.state() == State::Paused);

    // Past the 5 s release -> SUSPENDED.
    t += 2s;
    h.governor.onTick();
    CHECK(h.governor.state() == State::Suspended);
    REQUIRE(h.transitions.size() >= 1);
    CHECK(h.transitions.back().second == State::Suspended);
}

TEST_CASE("governor: SUSPENDED + all clear -> ACTIVE via the resume handler") {
    Harness h;
    auto t = std::chrono::steady_clock::now();
    h.governor.setClockForTest([&t] { return t; });
    bool resumed = false;
    h.governor.setResumeHandler([&] { resumed = true; });

    h.governor.setReason(Reason::Locked, true);
    t += 6s;
    h.governor.onTick();
    CHECK(h.governor.state() == State::Suspended);
    CHECK_FALSE(resumed);

    h.governor.setReason(Reason::Locked, false);
    CHECK(h.governor.state() == State::Active);
    CHECK(resumed); // the app's handler reopened the current item
}

TEST_CASE("governor: stop-then-resume within the release window reopens "
          "(app stop drops the session)") {
    Harness h;
    auto t = std::chrono::steady_clock::now();
    h.governor.setClockForTest([&t] { return t; });
    int reopens = 0;
    h.governor.setResumeHandler([&] { ++reopens; });

    // The app's stop handler feeds the User reason (governor -> PAUSED, keeps
    // the decoder) and THEN calls playback_->stop() directly, dropping the
    // session. The governor must not assume the session survived the pause.
    h.governor.setReason(Reason::User, true);
    CHECK(h.governor.state() == State::Paused);
    h.playback.stop();
    CHECK_FALSE(h.playback.isOpen());

    // Resume within the 5 s release window: the session is gone, so a plain
    // resume() would fail ("not paused (stopped)") — the reopen handler must
    // run instead (this was the pre-fix bug: state said ACTIVE, nothing played).
    t += 3s;
    h.governor.setReason(Reason::User, false);
    CHECK(h.governor.state() == State::Active);
    CHECK(reopens == 1);
}

TEST_CASE("governor: battery pause latches and clears with the power state") {
    Harness h;
    // Battery mode Pause: the monitor's battery query feeds the reason.
    vw::system::SystemStateMonitor mon(reinterpret_cast<HWND>(0x1));
    uint32_t reasons = h.governor.reasons();

    // On battery -> Battery reason set -> paused.
    CHECK(mon.updateBatteryReason(reasons, [] { return true; }) == Reason::Battery);
    h.governor.setReasons(reasons);
    CHECK(h.governor.state() == State::Paused);

    // Back on AC -> cleared -> active.
    CHECK(mon.updateBatteryReason(reasons, [] { return false; }) == Reason::Battery);
    h.governor.setReasons(reasons);
    CHECK(h.governor.state() == State::Active);
}

// ---- SystemStateMonitor message routing ------------------------------------

TEST_CASE("system: lock/unlock messages map to the Locked reason") {
    vw::system::SystemStateMonitor mon(reinterpret_cast<HWND>(0x1));
    uint32_t reasons = 0;

    CHECK((mon.translate(WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0, reasons) &
           Reason::Locked) != 0);
    CHECK((reasons & Reason::Locked) != 0);

    CHECK((mon.translate(WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0, reasons) &
           Reason::Locked) != 0);
    CHECK((reasons & Reason::Locked) == 0);
}

TEST_CASE("system: suspend/resume maps to SystemSuspended; unrelated ignored") {
    vw::system::SystemStateMonitor mon(reinterpret_cast<HWND>(0x1));
    uint32_t reasons = 0;

    CHECK((mon.translate(WM_POWERBROADCAST, PBT_APMSUSPEND, 0, reasons) &
           Reason::SystemSuspended) != 0);
    CHECK((reasons & Reason::SystemSuspended) != 0);

    CHECK((mon.translate(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0, reasons) &
           Reason::SystemSuspended) != 0);
    CHECK((reasons & Reason::SystemSuspended) == 0);

    // Unrelated messages are ignored.
    CHECK(mon.translate(WM_TIMER, 0, 0, reasons) == 0);
    CHECK(reasons == 0);
}

// ---- Config threshold-pair cross-validation (spec §9) ----------------------

TEST_CASE("config: pause >= resume is validated at load (violations clamped)") {
    vw::config::Config cfg;
    cfg.cpuPauseThreshold = 85;
    cfg.cpuResumeThreshold = 90; // violation: resume > pause
    cfg.gpuPauseThreshold = 90;
    cfg.gpuResumeThreshold = 70; // fine
    cfg.memoryPauseThreshold = 50;
    cfg.memoryResumeThreshold = 80; // violation

    vw::config::ConfigurationManager::validateThresholdPairs(cfg);
    CHECK(cfg.cpuPauseThreshold == 85);
    CHECK(cfg.cpuResumeThreshold == 85); // clamped UP to pause
    CHECK(cfg.gpuPauseThreshold == 90);
    CHECK(cfg.gpuResumeThreshold == 70); // untouched
    CHECK(cfg.memoryPauseThreshold == 50);
    CHECK(cfg.memoryResumeThreshold == 50); // clamped UP to pause
}
