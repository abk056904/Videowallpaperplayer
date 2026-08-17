# 2. Architecture

Target design for the wallpaper engine. The architecture favors **few components with clear ownership**, not dozens of abstraction layers.

---

## 2.1 High-level diagram

```text
                    ┌──────────────────────┐
                    │     Tray / UI        │   Win32 UI (lazy: created only when opened)
                    └──────────┬───────────┘
                               │
                    ┌──────────▼───────────┐
                    │ ApplicationController│   single instance guard, lifecycle,
                    └──────────┬───────────┘   wiring, shutdown sequence
                               │
        ┌──────────────────────┼─────────────────────┐
        │                      │                     │
┌───────▼────────┐    ┌────────▼────────┐   ┌────────▼─────────┐
│ MonitorManager │    │ ResourceGovernor│   │ ConfigurationMgr │
└───────┬────────┘    └────────┬────────┘   └──────────────────┘
        │               input: pause reasons from:
        │               │   GameDetector / FullscreenDetector /
        │               │   WorkloadMonitor / SystemStateMonitor /
        │               │   UserCommand (tray/UI/hotkeys)
        │               └────────▼────────┐
        └──────────────┬──────────────────┘
                       │
              ┌────────▼─────────┐
              │ WallpaperManager │   owns one WallpaperHost per monitor
              └────────┬─────────┘
                       │
              ┌────────▼─────────┐
              │ PlaybackManager  │   per-monitor playback session
              └────────┬─────────┘
                       │
              ┌────────▼─────────┐
              │ PlaylistManager  │   PlaybackController (loop/shuffle/next/prev)
              └────────┬─────────┘
                       │
              ┌────────▼─────────┐
              │ VideoPlayer /    │   FrameScheduler + bounded FrameQueue
              │ DecoderManager   │   MF Source Reader + hardware MFT
              └────────┬─────────┘
                       │ GPU decoded frames (ID3D11Texture2D NV12/P010)
              ┌────────▼─────────┐
              │ D3D11 Renderer   │   shared D3D11 device; shader scaling + YUV→RGB
              └────────┬─────────┘
                       │
              ┌────────▼─────────┐
              │ Desktop Surface  │   WorkerW-hosted per-monitor windows (behind icons)
              └──────────────────┘
```

Supporting subsystems (cross-cutting): `Logger`, `PerformanceStats`/`StatsCollector`, `LibraryManager` + `ThumbnailCache` (UI-only), `TrayController`, `Win32UI`.

---

## 2.2 Module catalog

| Module | Class(es) | Responsibility |
|---|---|---|
| **app** | `ApplicationController`, `WinMain`/`wWinMain`, control window | Message pump, hidden control window, notifications, wiring, deterministic shutdown |
| **monitors** | `MonitorManager`, `MonitorInfo` | Enumerate/observe monitors (`EnumDisplayMonitors`, `GetMonitorInfo`, `WM_DISPLAYCHANGE`, `WM_DEVICECHANGE`, DXGI outputs), bounds/refresh info, add/remove events |
| **wallpaper** | `WallpaperManager`, `WallpaperHost` | WorkerW/Progman discovery, per-monitor host windows behind icons, Explorer-restart recovery, visibility state per monitor |
| **graphics** | `D3D11DeviceManager`, `D3D11Renderer`, `TextureManager` | Device/context/factory/adapter/feature level; fullscreen-triangle rendering; NV12/P010→RGB shader; scaling modes; RAII resource pools; device-loss handling |
| **video** | `DecoderManager`, `VideoPlayer` | MF Source Reader, metadata, hardware MFT via DXGI manager, software fallback, decode worker, bounded `FrameQueue` |
| **playback** | `FrameScheduler`, `PlaybackController` | Source-FPS-aware timing, waitable-timer-driven presentation, pause/resume/seek/loop, frame dropping |
| **playlist** | `PlaylistManager` | Items, modes (single/sequential/loop/shuffle), next/prev/current, persistence, next-video preparation hook |
| **detection** | `GameDetector`, `FullscreenDetector` | Foreground-window-driven classification with caching, allow/deny lists |
| **performance** | `WorkloadMonitor`, `StatsCollector` | Low-frequency CPU/GPU/RAM sampling (PDH/DXGI counters), hysteresis/debounce state, `PerformanceStats` |
| **system** | `SystemStateMonitor` | Lock/unlock, display off/on, power/battery, suspend/resume via notifications |
| **governor** | `ResourceGovernor` | Central authority over state: ACTIVE/REDUCED/PAUSED/SUSPENDED; pause-reason bitmask; transition actions |
| **config** | `ConfigurationManager` | Load/validate/persist JSON (see D-06), defaults, corrupt-file recovery, per-user path |
| **library** | `LibraryManager` (+ `ThumbnailCache` in v2) | V1: incremental library scan (notifications), lazy metadata. V2: async bounded thumbnails (LRU + disk) |
| **ui** | `Win32UI`, `TrayController` | Home/Library/Playlists/Monitors/Performance/Settings; tray menu. V2: hotkeys, drag & drop, debug overlay |
| **logging** | `Logger` | Levels, rotation (≤10 MB), aggregate events only |
| **util** | — | QPC clock, waitable timer wrapper, `std::expected`-style results, small JSON parser (D-06) |

---

## 2.3 Threading model

Fixed, small set of workers (≈4–6 threads total). **No per-monitor, per-video, or per-decoder threads.**

```text
Main/UI thread
  ├── message loop (control window: display/session/power/device notifications)
  ├── UI + tray (only when open; UI never decodes/renders/polls)
  │
  ├── Decode worker            (one per active distinct video source; blocked on demand)
  ├── Render/control worker    (frame pacing; waits on waitable timer / events / queue)
  └── Monitoring worker        (1–2 s samples; blocked between samples; skips work in
                                GAME/FULLSCREEN/LOCKED/DISPLAY_OFF states)
```

Notes:

- The render worker is shared; with multiple monitors, one worker iterates all active monitor sessions per scheduled tick (presentation times computed per monitor refresh rate).
- The decode worker count equals the number of **distinct** videos currently playing (1 for clone mode; N only for N independent videos). All other infrastructure is shared.
- Thumbnail generation runs on a low-priority cancellable task only while the Library UI is open; stops immediately on game/load.
- Idle threads **block** (condition variables, waitable timers, events). `Sleep(1)` polling loops are banned (see §2.8).

---

## 2.4 Resource state machine (ResourceGovernor)

States: **ACTIVE → REDUCED → PAUSED → SUSPENDED** (+ TERMINATED on exit).

```text
                    ┌───────────────┐
                    │    ACTIVE     │   normal playback
                    └───────┬───────┘
          visible + high load│  │ game/fullscreen/locked/display-off/user/battery
                            │  └──────────────► ┌──────────┐
                            │                   │  PAUSED  │
                            ▼                   └────┬─────┘
                    ┌───────────────┐                │ long pause (> config, e.g. 5 s)
                    │    REDUCED    │                ▼
                    └───────┬───────┘         ┌────────────┐
                            │                 │ SUSPENDED  │  decoder + temp GPU released
                            └── conditions    └─────┬──────┘
                                clear               │ resume event
                                    ◄───────────────┘
                                    (recreate minimal resources → ACTIVE)
```

Transition actions (explicit, centralized — subsystems must never start/stop the decoder directly):

| Transition | Actions |
|---|---|
| → PAUSED | stop decoder advancement; cancel frame timer; stop render scheduling; clear obsolete frame queue; retain playback position + playlist index |
| PAUSED → SUSPENDED | release decoder, next-video preparation, temporary GPU textures/frame buffers; keep config, playlist state, current path |
| SUSPENDED → ACTIVE | recreate decoder; obtain current frame (seek to saved position); restart timing; render |
| ACTIVE → REDUCED | keep playback if possible; disable optional work (thumbnails, verbose stats, next-video prep) |

Pause-reason logic:

- Any **required** reason set ⇒ PAUSED. Resume only when **all** reasons clear.
- Workload reasons (HighCPU/HighGPU/HighMemory) use hysteresis + debounce (e.g. GPU > 90% for 3 s ⇒ pause; GPU < 70% for 5 s ⇒ resume; configurable).
- Immediate reasons (User, Game, Fullscreen, Locked, DisplayOff, SystemSuspended) bypass debounce.

---

## 2.5 Frame pipeline

### Primary path (hardware, GPU-resident)

```text
File
 ↓  IMFSourceReader (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING off;
                    MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS on)
 ↓
Hardware decoder MFT  (IMFDXGIDeviceManager bound to shared D3D11 device)
 ↓
ID3D11Texture2D NV12 (8-bit) / P010 (10-bit)   ← stays on GPU
 ↓
Decode worker publishes into bounded FrameQueue (shared_ptr<DecodedFrame> holding
  ComPtr<ID3D11Texture2D> + timestamp; no CPU copies)
 ↓
FrameScheduler selects current frame per monitor (source-FPS pacing)
 ↓
D3D11Renderer: bind video SRVs (Y plane R8 / UV plane R8G8 from NV12 texture)
  → fullscreen triangle → minimal pixel shader: YUV→RGB (BT.709) + scaling/crop UV math
 ↓
Present to the monitor's wallpaper host swap chain (only when a new frame is due)
```

Copies audit (documented per requirement): decoder→queue = **reference**; queue→renderer = **reference**; render = **GPU shader read**; no CPU round trip in the happy path.

### Fallback path (software decode)

Only when hardware decoding is unavailable/fails for a codec: MF software decoder → `IMF2DBuffer` → upload to staging texture → GPU (or CPU YUV→RGB only if GPU conversion is impossible). Reported as `Software fallback` in diagnostics. Never crash; retry with controlled delay.

### Frame queue design

- `FrameQueue`: fixed capacity **2–3** (configurable), `std::mutex` + `std::condition_variable`.
- Producer (decode worker) blocks when full (backpressure) — or discards oldest for wallpaper freshness when latency grows.
- Consumer (render worker) takes newest frame whose timestamp ≤ deadline; drops stale frames. `droppedFrames` counter feeds stats.
- Queue cleared on pause/seek/loop/video change; never grows unbounded.

### Frame timing

- Master clock: `QueryPerformanceCounter`-based monotonic clock.
- Render worker waits on a **waitable timer** armed to the next frame deadline (source FPS), plus events for: new frame, pause, shutdown, monitor change, video transition.
- Per-monitor presentation times account for each monitor's refresh rate (mixed refresh rates supported); if a monitor cannot present yet, skip and re-arm.
- Looping: reset playback position only; reuse decoder/GPU resources where possible.

---

## 2.6 Wallpaper hosting (WorkerW technique)

Runtime discovery, not hardcoded assumptions (per spec §7):

1. Find top-level window of class `Progman` (desktop icon host).
2. Send `WM_SPAWNWORKERW` (0x052C, `SendMessageTimeout`) to force Explorer to spawn a `WorkerW` window.
3. Enumerate top-level `WorkerW` windows; find the one whose child is `SHELLDLL_DefView` (the "behind icons" layer).
4. The wallpaper layer is the sibling `WorkerW` (or `Progman` itself when no `DefView` exists — Windows version dependent). **Verify at runtime** which arrangement is present.
5. Create one `WallpaperHost` child window per monitor, positioned at the monitor's bounds, parented into the wallpaper layer, so it:
   - stays behind desktop icons;
   - never receives keyboard focus (`WS_EX_NOACTIVATE`);
   - doesn't intercept clicks (child of the wallpaper layer; no `WS_EX_TRANSPARENT` needed unless input issues appear — test);
   - doesn't appear above normal applications.
6. **Explorer restart recovery:** watch for desktop host invalidation (e.g. `Progman`/`WorkerW` disappearing or recreated — detected via a low-frequency check or `WM_SETTINGCHANGE`/shell events); on invalidation: rediscover → destroy old hosts → recreate hosts → reattach monitor assignments → resume. No app restart; playlist/config untouched.
7. Swap chains: one DXGI swap chain per host window (`CreateSwapChainForHwnd`), presented only when a new frame is due (`Present(0/1, 0)`); when idle/paused, no presents.

---

## 2.7 Monitor manager

```cpp
struct MonitorInfo {
    std::wstring id;                 // stable device id (e.g. \\.\DISPLAY1 + adapter LUID)
    HMONITOR handle;
    RECT bounds;
    RECT workArea;
    UINT  width, height;
    UINT  refreshRateNumerator, refreshRateDenominator;   // from DXGI output desc
    bool  primary;
    bool  active;                    // currently connected
    // adapter association (LUID) for multi-GPU locality
};
```

- Refresh: `EnumDisplayMonitors` + `GetMonitorInfo` on `WM_DISPLAYCHANGE`/`WM_DEVICECHANGE`; DXGI output enumeration for refresh rates and adapter/output relationships.
- Emits events: `monitorAdded(id)`, `monitorRemoved(id)`, `monitorChanged(id)` → `WallpaperManager` and `ResourceGovernor` react (no restart).
- DPI awareness: `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` at startup so bounds are physical pixels.

---

## 2.8 Synchronization & concurrency rules

- Ownership-first: every COM object, texture, thread, window, file handle has one owner and a documented release point (see `05-decisions-risks.md` §Resource lifetime map).
- Primitives: `std::mutex`, `std::shared_mutex` (config/state reads), `std::condition_variable`, `std::atomic` for flags/counters; waitable timers + Win32 events for cross-thread wakeups.
- Lock-free structures only where profiling proves a bottleneck; never trade correctness to remove a mutex.
- No busy waits anywhere: workers block. Polling only where no event-driven API exists (documented exceptions: Explorer-host invalidation check at ~1 Hz when idle, config watch, file watch via `ReadDirectoryChangesW` which is event-driven).

---

## 2.9 Configuration

- Location: `%APPDATA%\WallpaperEngine\config.json` (see D-06 for format decision; JSON example in spec §44).
- `ConfigurationManager` loads at startup, validates every field (clamp ranges, enum whitelists), writes defaults on first run, backs up + regenerates on corruption.
- Writes are batched/debounced; persisted only on meaningful change, video transition, or shutdown — **not** every second.
- Live mutation: UI/tray/hotkeys go through `ConfigurationManager` which notifies subscribers (e.g. thresholds → governor, mode → playback) without restart.

Schema (target, matching spec example):

```json
{
  "general":    { "startWithWindows": true, "minimizeToTray": true },
  "playback":   { "mode": "playlist", "shuffle": false, "loop": true, "scaling": "fill",
                  "frameQueue": 3, "audio": false },
  "performance":{ "pauseOnGame": true, "pauseOnFullscreen": true,
                  "cpuPauseThreshold": 85, "cpuResumeThreshold": 65,
                  "gpuPauseThreshold": 90, "gpuResumeThreshold": 70,
                  "memoryPauseThreshold": 90,
                  "pauseDelaySeconds": 3, "resumeDelaySeconds": 5,
                  "mode": "balanced", "ultraLowResource": false,
                  "longPauseReleaseSeconds": 5 },
  "battery":    { "mode": "pause" },
  "detection":  { "alwaysPause": ["game.exe"], "neverPause": ["my-editor.exe"] },
  "ui":         { "hotkeys": { "pause": "Ctrl+Alt+P", "next": "Ctrl+Alt+N", "prev": "Ctrl+Alt+B" } }
}
```

---

## 2.10 Logging design

- `Logger`: levels TRACE..FATAL; sink to rotating files in `%APPDATA%\WallpaperEngine\logs\` (`current.log`, `previous.log`, ≤5–10 MB total) + optional debug output.
- Release default INFO; DEBUG only when enabled; TRACE compiled out or gated in Release.
- Policy: aggregate events only (playback started, decoder mode, adapter name, monitor added/removed, pause/resume with reason, dropped-frame summary per interval, device loss, recoveries). Never log per frame/GPU op/performance sample.
- Thread-safe, low-contention: a small SPSC-style queue drained by the logger (or immediate write with a mutex — measure; logging is rare events only).

---

## 2.11 Performance statistics

- `StatsCollector` computes `PerformanceStats` every ~1–2 s from: PDH/system CPU counters, DXGI adapter memory (`IDXGIAdapter3::QueryVideoMemoryInfo`), GPU engine utilization (Windows "GPU Engine" performance counters; see D-08 / risk R-03), decoder/render counters (decodedFps, presentedFps, droppedFrames, decodeLatencyMs, renderTimeMs), hardware-decode flag.
- Exposed to UI Home/Performance panels (debug overlay is v2).
- Monitoring cost is bounded: sampling ~1–2 s, and fully suspended while GAME/FULLSCREEN/LOCKED/DISPLAY_OFF unless the UI is open and asking.

---

## 2.12 Directory layout (target)

```text
/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── LICENSE
├── cmake/                      # toolchain helpers, sanitizer/audit options
├── include/                    # public headers (or keep headers next to sources; pick one)
│   └── wallpaper/…             # mirror of src/ structure
├── src/
│   ├── app/        main.cpp, ApplicationController, control window
│   ├── wallpaper/  WallpaperManager, WallpaperHost
│   ├── video/      DecoderManager, VideoPlayer, FrameQueue
│   ├── graphics/   D3D11DeviceManager, D3D11Renderer, TextureManager
│   ├── monitors/   MonitorManager
│   ├── playback/   FrameScheduler, PlaybackController
│   ├── playlist/   PlaylistManager
│   ├── detection/  GameDetector, FullscreenDetector
│   ├── performance/WorkloadMonitor, StatsCollector
│   ├── system/     SystemStateMonitor
│   ├── config/     ConfigurationManager
│   ├── library/    LibraryManager, ThumbnailCache
│   ├── ui/         Win32UI, TrayController, HomePanel, …
│   ├── logging/    Logger
│   └── util/       clock, waitable timer, result types, json (D-06)
├── tests/                      # unit tests (config, playlist, governor, queue, …)
├── shaders/                    # VideoShader.hlsl (YUV→RGB + scale/crop), compiled at build
├── assets/                     # icons, tray icon, defaults (only required assets)
├── scripts/                    # build helpers, packaging (zip/installer), perf scripts
└── docs/                       # this plan
```

Note: spec files show both `include/`+`src/` split and a single `src/` tree. Decision D-01 in `05-decisions-risks.md` fixes this: prefer headers beside sources (`src/<module>/`) unless a public API boundary is needed — fewer files, less duplication.
