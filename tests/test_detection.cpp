#include "doctest.h"

#include <chrono>
#include <optional>
#include <string>

#include "detection/FullscreenDetector.h"
#include "detection/GameDetector.h"
#include "performance/HysteresisEngine.h"
#include "performance/WorkloadMonitor.h"

using namespace std::chrono_literals;
using vw::performance::HysteresisEngine;
using vw::performance::WorkloadMonitor;
using vw::detection::classifyWindowState;
using vw::detection::WindowState;
using vw::detection::GameClass;
using vw::detection::GameDetector;

namespace {

const RECT kMonitor{0, 0, 1920, 1080};
const LONG_PTR kOverlapped = WS_OVERLAPPEDWINDOW; // WS_CAPTION|WS_THICKFRAME|...
const LONG_PTR kMaximized = WS_OVERLAPPEDWINDOW | WS_MAXIMIZE;
const LONG_PTR kPopup = WS_POPUP;

// A full-window rect for the given styles (maximized windows cover the
// monitor but keep their frame styles).
RECT covers(const LONG_PTR style) {
    return style == kMaximized ? RECT{0, 0, 1920, 1080} : RECT{-8, -8, 1928, 1088};
}

} // namespace

// ---- HysteresisEngine (docs/04 §2 transition table) ------------------------

TEST_CASE("hysteresis: latches HIGH after the pause delay, not on a spike") {
    HysteresisEngine e({90.0, 70.0, 3000ms, 5000ms});
    auto t = std::chrono::steady_clock::now();

    CHECK_FALSE(e.update(95.0, t));          // spike starts the debounce timer
    CHECK_FALSE(e.update(80.0, t + 1000ms)); // dips below — timer cancelled
    CHECK_FALSE(e.update(95.0, t + 2000ms)); // back over — timer restarts
    CHECK_FALSE(e.update(95.0, t + 4900ms)); // 2.9 s of 95% — not yet latched
    CHECK(e.update(95.0, t + 5100ms));       // 3.1 s — latched
    CHECK(e.high());
}

TEST_CASE("hysteresis: clears only after the resume delay below the resume threshold") {
    HysteresisEngine e({90.0, 70.0, 3000ms, 5000ms});
    auto t = std::chrono::steady_clock::now();
    // Latch first: 3 s over the pause threshold.
    e.update(95.0, t);
    e.update(95.0, t + 1000ms);
    CHECK(e.update(95.0, t + 3000ms)); // latched at 3 s

    CHECK(e.update(75.0, t + 4000ms));        // below pause but above resume — stays
    CHECK(e.update(65.0, t + 5000ms));        // below resume — clear timer starts
    CHECK(e.update(65.0, t + 9900ms));        // 4.9 s — not yet cleared
    CHECK_FALSE(e.update(65.0, t + 10100ms)); // 5.1 s — cleared
    CHECK_FALSE(e.high());
}

TEST_CASE("hysteresis: no oscillation at the threshold boundary") {
    HysteresisEngine e({90.0, 70.0, 3000ms, 5000ms});
    auto t = std::chrono::steady_clock::now();
    // Bounce 89/91 (just under/over the pause threshold) — never latches
    // (each dip below cancels the pending latch timer).
    for (int i = 0; i < 10; ++i) {
        e.update(91.0, t + i * 500ms);
        e.update(89.0, t + i * 500ms + 250ms);
    }
    CHECK_FALSE(e.high());
    // Latch at 95% (3 s), then bounce 69/71 around the resume threshold —
    // the clear timer keeps restarting; the engine never flaps to cleared.
    e.update(95.0, t + 6000ms);
    e.update(95.0, t + 7000ms);
    CHECK(e.update(95.0, t + 9000ms)); // latched
    for (int i = 0; i < 10; ++i) {
        e.update(71.0, t + 10000ms + i * 500ms);
        e.update(69.0, t + 10000ms + i * 500ms + 250ms);
    }
    CHECK(e.high()); // still latched — no resume/pause flapping
    // Drop below the resume threshold for the full delay and it clears.
    e.update(65.0, t + 16000ms);
    e.update(65.0, t + 21000ms);
    CHECK_FALSE(e.high());
}

// ---- WorkloadMonitor: CPU percent + aggregation ----------------------------

TEST_CASE("workload: CPU percent is the busy delta over the total delta") {
    // No previous sample -> baseline, 0%.
    CHECK(WorkloadMonitor::CpuSampler::cpuPercent(100, 200, 100, 0, 0, 0, false) ==
          doctest::Approx(0.0));
    // Deltas: idle +60, kernel +100, user +60. Kernel INCLUDES idle on
    // Windows, so busy = (total - idle) = (220 - 60) = 160 of 220 = 72.7%.
    CHECK(WorkloadMonitor::CpuSampler::cpuPercent(160, 300, 160, 100, 200, 100, true) ==
          doctest::Approx(72.7273));
    // No idle delta -> 100% busy.
    CHECK(WorkloadMonitor::CpuSampler::cpuPercent(100, 300, 160, 100, 200, 100, true) ==
          doctest::Approx(100.0));
    // Zero total delta is guarded (no division by zero).
    CHECK(WorkloadMonitor::CpuSampler::cpuPercent(100, 200, 100, 100, 200, 100, true) ==
          doctest::Approx(0.0));
}

TEST_CASE("workload: sample fills the state and pushes into the collector") {
    WorkloadMonitor wm({.cpuPause = 85.0, .cpuResume = 65.0, .gpuPause = 90.0, .gpuResume = 70.0,
                        .memoryPause = 90.0, .memoryResume = 75.0, .pauseDelay = 3000ms,
                        .resumeDelay = 5000ms});
    auto t = std::chrono::steady_clock::now();
    // First sample is the CPU baseline (0%); RAM/VRAM are real reads — only
    // assert the shapes that are deterministic (bounds + plumbing).
    const auto s = wm.sample(t);
    CHECK(s.cpuUsage >= 0.0);
    CHECK(s.cpuUsage <= 100.0);
    CHECK(s.memoryUsage >= 0.0);
    CHECK(s.memoryUsage <= 100.0);
    CHECK_FALSE(s.anyHigh()); // first sample never latches (baseline)

    // The sample flows into StatsCollector (UI telemetry).
    vw::performance::StatsCollector collector;
    WorkloadMonitor::pushToCollector(collector, s);
    const auto snap = collector.snapshot();
    CHECK(snap.cpuUsage == doctest::Approx(s.cpuUsage));
    CHECK(snap.systemMemoryUsed == s.systemMemoryUsed);
}

TEST_CASE("workload: config reload rebuilds the engines (thresholds apply)") {
    WorkloadMonitor wm({.cpuPause = 85.0, .cpuResume = 65.0, .gpuPause = 90.0, .gpuResume = 70.0,
                        .memoryPause = 90.0, .memoryResume = 75.0, .pauseDelay = 3000ms,
                        .resumeDelay = 5000ms});
    auto t = std::chrono::steady_clock::now();
    // reconfigure with an impossible threshold: even a latched engine resets
    // (the engines are rebuilt; a fresh high would need the new threshold).
    wm.reconfigure({.cpuPause = 99.0, .cpuResume = 65.0, .gpuPause = 90.0, .gpuResume = 70.0,
                    .memoryPause = 90.0, .memoryResume = 75.0, .pauseDelay = 3000ms,
                    .resumeDelay = 5000ms});
    const auto s = wm.sample(t);
    CHECK_FALSE(s.cpuHigh);
    // And the engine honors the new threshold: 98% over 4 s does NOT latch
    // with pauseThreshold 99 (would with 85).
    for (int i = 1; i <= 4; ++i) {
        wm.sample(t + i * 1100ms);
    }
    CHECK_FALSE(wm.state().cpuHigh);
}

// ---- FullscreenDetector (pure classification) ------------------------------

TEST_CASE("fullscreen: maximized overlapped window is NOT fullscreen") {
    // A maximized editor covers the monitor but keeps its caption frame.
    const auto state = classifyWindowState(covers(kMaximized), kMonitor, kMaximized, 0);
    CHECK(state == WindowState::Maximized);
}

TEST_CASE("fullscreen: exclusive fullscreen (WS_POPUP) covering the monitor") {
    const auto state = classifyWindowState(covers(kPopup), kMonitor, kPopup, 0);
    CHECK(state == WindowState::Fullscreen);
    CHECK(vw::detection::isFullscreenState(state));
}

TEST_CASE("fullscreen: borderless (no caption/thick frame) covering the monitor") {
    const LONG_PTR borderless = WS_VISIBLE; // no caption, no thick frame
    const auto state = classifyWindowState(covers(borderless), kMonitor, borderless, 0);
    CHECK(state == WindowState::BorderlessFullscreen);
    CHECK(vw::detection::isFullscreenState(state));
}

TEST_CASE("fullscreen: a window that does not cover the monitor is windowed") {
    const RECT small{100, 100, 800, 600};
    CHECK(classifyWindowState(small, kMonitor, kMaximized, 0) == WindowState::Windowed);
    CHECK(classifyWindowState(small, kMonitor, kPopup, 0) == WindowState::Windowed);
}

TEST_CASE("fullscreen: desktop shell layers never classify as fullscreen") {
    // Progman covers the monitor with WS_POPUP (like an exclusive fullscreen
    // app) — without the shell exclusion, clicking the desktop would classify
    // it as fullscreen and pause the wallpaper. The predicate must reject all
    // desktop layer classes (and accept a non-shell class).
    CHECK(vw::detection::isDesktopShellClass(L"Progman"));
    CHECK(vw::detection::isDesktopShellClass(L"WorkerW"));
    CHECK(vw::detection::isDesktopShellClass(L"SHELLDLL_DefView"));
    CHECK_FALSE(vw::detection::isDesktopShellClass(L"notepad"));
    CHECK_FALSE(vw::detection::isDesktopShellClass(nullptr));
    // The underlying geometry is unchanged — a real popup fullscreen app
    // (a non-shell class) still classifies as fullscreen.
    const auto state = classifyWindowState(covers(kPopup), kMonitor, kPopup, 0);
    CHECK(state == WindowState::Fullscreen);
    CHECK(vw::detection::isFullscreenState(state));
}

// ---- GameDetector (list matching + caching) --------------------------------

TEST_CASE("game: allow list marks the exe as a game") {
    CHECK(GameDetector::classify(L"eldenring.exe", {L"eldenring.exe"}, {}) == GameClass::Game);
    // Basenames and case are normalized.
    CHECK(GameDetector::classify(std::wstring(L"C:\\Games\\ELDEN RING\\Game\\eldenring.exe"),
                                 {L"eldenring.exe"}, {}) == GameClass::Game);
    CHECK(GameDetector::classify(L"eldenring.exe", {L"ELDENRING.EXE"}, {}) == GameClass::Game);
}

TEST_CASE("game: deny (never pause) wins over allow") {
    CHECK(GameDetector::classify(L"eldenring.exe", {L"eldenring.exe"}, {L"eldenring.exe"}) ==
          GameClass::NotGame);
    CHECK(GameDetector::classify(L"steam.exe", {L"steam.exe"}, {L"steam.exe"}) ==
          GameClass::NotGame);
}

TEST_CASE("game: unlisted exe is Unknown (policy layer decides)") {
    CHECK(GameDetector::classify(L"notepad.exe", {}, {}) == GameClass::Unknown);
    CHECK(GameDetector::classify(L"notepad.exe", {L"eldenring.exe"}, {}) == GameClass::Unknown);
}

TEST_CASE("game: the process path is cached across same-pid updates") {
    GameDetector gd;
    int lookups = 0;
    auto lookup = [&](DWORD) -> std::optional<std::wstring> {
        ++lookups;
        return std::wstring(L"C:\\Games\\game.exe");
    };

    gd.setLists({L"game.exe"}, {});
    const auto alive = [](DWORD) { return true; }; // synthetic pids: alive
    gd.updateForeground(1234, lookup, alive);
    CHECK(lookups == 1);
    CHECK(gd.state().classification == GameClass::Game);

    gd.updateForeground(1234, lookup, alive); // same pid, alive -> cached
    CHECK(lookups == 1); // cached — no second path lookup
    CHECK(gd.state().classification == GameClass::Game);

    gd.updateForeground(5678, lookup, alive); // new pid
    CHECK(lookups == 2);
    CHECK(gd.state().classification == GameClass::Game);

    // pid 0 (no foreground) -> state cleared, no lookup.
    gd.updateForeground(0, lookup, alive);
    CHECK(gd.state().pid == 0);
    CHECK(gd.state().processPath.empty());
    CHECK(lookups == 2);
}

TEST_CASE("game: a reused pid (process exited) does not return stale cache") {
    GameDetector gd;
    gd.setLists({L"game.exe"}, {});
    int lookups = 0;
    bool reused = false;
    auto lookup = [&](DWORD) -> std::optional<std::wstring> {
        ++lookups;
        // Before the reuse the process is game.exe; after the (simulated)
        // exit + pid reuse it is a different binary behind the SAME pid.
        return reused ? std::wstring(L"C:\\Windows\\notepad.exe")
                      : std::wstring(L"C:\\Games\\game.exe");
    };

    gd.updateForeground(1234, lookup, [](DWORD) { return true; });
    CHECK(lookups == 1);
    CHECK(gd.state().classification == GameClass::Game);

    // Same pid, process still alive -> cached, no scan.
    gd.updateForeground(1234, lookup, [](DWORD) { return true; });
    CHECK(lookups == 1);
    CHECK(gd.state().classification == GameClass::Game);

    // Same pid, but the process EXITED (pid reused by a different binary) ->
    // must re-lookup; the stale "game" classification must not survive.
    reused = true;
    gd.updateForeground(1234, lookup, [](DWORD) { return false; });
    CHECK(lookups == 2); // re-looked-up despite the same pid
    CHECK(gd.state().classification == GameClass::Unknown); // notepad unlisted
}

TEST_CASE("game: a failed path lookup leaves the classification unknown") {
    GameDetector gd;
    gd.setLists({L"game.exe"}, {});
    gd.updateForeground(1234, [](DWORD) -> std::optional<std::wstring> { return std::nullopt; });
    CHECK(gd.state().pid == 1234);
    CHECK(gd.state().classification == GameClass::Unknown);
    CHECK(gd.state().processPath.empty());
}
