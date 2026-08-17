# 1. Consolidated Requirements

This document merges the requirements from all three task specifications into a single, deduplicated requirements list. Sections marked **(all)** appear in all three specs; others note their origin.

---

## 1.1 Project objective

Build a **native Windows 11 video wallpaper engine**: play video files as animated desktop wallpapers, with the primary engineering objective being

> **Minimum practical resource consumption** — use as little RAM, CPU, GPU, VRAM, disk space, CPU wakeups, threads, handles, filesystem I/O, CPU↔GPU transfers, background activity, and power as reasonably possible, while still providing smooth, reliable playback.

Guiding principles **(all)**:

```
DO THE LEAST AMOUNT OF WORK NECESSARY TO DISPLAY THE CURRENT FRAME.
IF THE USER CANNOT SEE THE WALLPAPER → DO (almost) NO VIDEO WORK.
IF THE CURRENT FRAME HAS NOT CHANGED        → DO NOT REDRAW IT.
IF PLAYBACK IS PAUSED                       → STOP DECODER AND RENDERER.
IF THE SAME FRAME CAN BE SHARED             → DO NOT DECODE IT TWICE.
IF WINDOWS ALREADY PROVIDES A CAPABILITY    → DO NOT SHIP ANOTHER LIBRARY.
IF AN EVENT CAN REPLACE POLLING             → USE THE EVENT.
IF A RESOURCE IS NO LONGER NEEDED           → RELEASE IT.
```

The application must feel like a **tiny dormant Windows component that wakes only to produce a new wallpaper frame**, not a game engine or media center continuously rendering.

---

## 1.2 Non-negotiable requirements (all)

1. Runs natively on Windows 11.
2. x64.
3. C++20 or newer.
4. Native Win32 APIs.
5. **Direct3D 11** for the initial renderer (D3D12 only if a demonstrated technical reason emerges).
6. **Windows Media Foundation** for video decoding/playback where practical.
7. Hardware video decoding preferred.
8. Video frames rendered directly into GPU textures.
9. No unnecessary CPU↔GPU frame copies.
10. Multiple monitor support.
11. Independent wallpaper/playlists per monitor.
12. Video playlists.
13. Loop modes.
14. Shuffle.
15. Automatic pause during games/fullscreen/high workloads.
16. Automatic resume afterward.
17. Stop decoding when playback is paused.
18. Stop rendering when wallpaper cannot be seen.
19. No busy loops.
20. Stable for 24+ hours of continuous operation.
21. Recover from monitor changes and common GPU/decoder failures.
22. Persistent configuration.
23. System-tray application.
24. Optional start with Windows.
25. Useful diagnostics and performance information.

---

## 1.3 Prohibited technologies and architectures (all)

**Do NOT use:**

- Electron, Chromium, WebView/WebView2-based rendering
- HTML/CSS wallpaper rendering
- Unity, Unreal, any game engine
- Python as playback engine, Java, JavaScript runtime
- Qt (unless a compelling reason), Avalonia, WPF, WinUI runtime as core engine
- .NET runtime dependency
- Embedded browsers, large UI frameworks
- Unnecessary third-party frameworks / codec libraries
- Continuously polling every few milliseconds
- CPU-based frame conversion pipeline (decode → CPU bitmap → color convert → GPU upload)
- A Windows Service (**no background service**, no kernel drivers, no admin privileges required)
- Game injection / DLL hooking / anti-cheat-sensitive components

**Do NOT create:** multiple processes (one process only), one thread per monitor/video/decoder/metric/playlist, dozens of threads.

**Do NOT do:** busy-wait loops, `while(running){...}` with `Sleep(1)`, decode entire videos, preload libraries/playlists into memory, render at unnecessary FPS, allocate/create textures every frame, poll all processes continuously, scan folders continuously, generate thumbnails continuously, keep decoder alive while suspended, keep unused GPU textures alive, write logs every frame, write playback position every second, bundle unnecessary frameworks, statically link large libraries without justification.

---

## 1.4 Technology stack (all)

| Concern | Choice |
|---|---|
| Language | C++20+ (MSVC) |
| Platform | Windows 11 x64, Windows SDK |
| UI | Native Win32 (no framework) |
| Rendering | Direct3D 11 + DXGI |
| Decoding | Media Foundation (`mfplat.dll`, `mf.dll`, `mfreadwrite.dll`, `d3d11.dll`, `dxgi.dll` — system-provided) |
| Build | CMake (Debug x64 + Release x64 presets) |
| Configuration | Compact human-readable file in per-user AppData (JSON example given; see decision D-06 in `05-decisions-risks.md`) |
| Tests | Lightweight header-only framework or minimal custom harness (decision D-07) |

---

## 1.5 Functional requirements

### 1.5.1 Video file support (all)

- Containers: `.mp4`, `.mkv`, `.mov`, `.webm`, `.avi` (as supported by Media Foundation / system codecs).
- Codecs: H.264, H.265/HEVC, AV1, VP9 — **hardware decode where available**, software fallback otherwise (reported clearly in diagnostics).
- Metadata inspection before playback: duration, width, height, frame rate, codec, bit depth, HDR metadata (if available), audio presence.

```cpp
struct VideoMetadata {
    uint32_t width;
    uint32_t height;
    double   frameRate;
    double   durationSeconds;
    VideoCodec codec;      // scoped enum: H264, HEVC, AV1, VP9, Unknown
    bool hdr;
    bool hasAudio;
};
```

- **Audio is OFF by default** and not initialized unless explicitly enabled later.

### 1.5.2 Hardware decoding (all)

- Attempt hardware acceleration first; detect appropriate Media Foundation hardware transforms (MFTs).
- Use `IMFDXGIDeviceManager` + hardware decoder MFT + DXGI/D3D11 surfaces.
- Preferred pipeline: `File → MF Source Reader → hardware decoder → D3D11 texture/DXGI surface → pixel shader → wallpaper`.
- Software fallback on hardware failure — **never crash**.
- Expose actual decoder mode in diagnostics (e.g. `Decoder: NVIDIA NVDEC / hardware`, `Decoder: AMD hardware`, `Decoder: Intel hardware`, `Decoder: Software fallback`). **Never fabricate vendor names** — read them from `IDXGIAdapter::GetDesc`/`DXGI_ADAPTER_DESC`.

### 1.5.3 D3D11 device management (all)

- `D3D11DeviceManager`, `D3D11Renderer`, `TextureManager` classes.
- Feature levels 11_1 → 11_0 with fallback; BGRA support for Windows composition; debug layer only in Debug builds, never in Release.
- DXGI factory/adapter/output enumeration; per-adapter decode capability checks.

### 1.5.4 GPU resource strategy (all)

- Reuse resources; no per-frame create/render/destroy cycles.
- Resource pools where practical; RAII everywhere (`Microsoft::WRL::ComPtr`).
- No raw COM ownership leaks; explicit resource lifetimes (application / playback / frame / UI / monitor).

### 1.5.5 Frame pipeline and timing (all)

- Bounded frame queue, **2–3 frames to start** (profile later). Hard upper bound; no unlimited buffering; no loading entire videos.
- Drop obsolete frames when renderer is behind; freshness > completeness for wallpapers.
- High-resolution timing (`QueryPerformanceCounter`), waitable timers, events, condition variables. **No busy loops.**
- Render thread blocks until: next frame deadline **OR** new frame **OR** pause requested **OR** shutdown **OR** monitor change.
- Source-FPS-aware scheduling: 30 FPS video on a 144 Hz monitor ⇒ ~30 decodes/presents per second, not 144.
- Backpressure: decoder must block/discard when the queue is full; no unbounded producer/consumer growth.

### 1.5.6 Playlist engine (all)

- `PlaylistManager` + `PlaybackController`.
- Modes: Single, Sequential, Loop playlist, Shuffle (avoid immediate repeats).
- Operations: add, remove, move, replace, clear, next, previous, shuffle, setCurrent.
- Persist playlist state; entries are lightweight:

```cpp
struct PlaylistItem {
    std::filesystem::path path;
    std::optional<double> startTime;
    std::optional<double> endTime;
    bool enabled;
};
```

- **Next-video preparation:** when current video nears completion, asynchronously prepare next video's metadata/source reader (do **not** decode it fully); goal is minimal visible transition delay without keeping two full pipelines alive.

### 1.5.7 Scaling and color (all)

- Scaling modes: **Fill** (default, preserve aspect, crop overflow), Fit, Stretch, Center (a.k.a. Original, no scaling). GPU shader scaling only — never CPU. (Naming unified on **Center** across plan/spec; "Original" is the no-scaling mode.)
- NV12/P010 GPU surfaces → pixel shader conversion to RGB (no CPU NV12→RGB→RGBA round trip). Correct BT.709 SDR conversion; HDR: detect and either implement/test a real HDR path or **document behavior + safe fallback** — do not claim untested HDR support.

### 1.5.8 Multi-monitor (all)

- `MonitorManager` using `EnumDisplayMonitors`, `GetMonitorInfo`, `WM_DISPLAYCHANGE`, `WM_DEVICECHANGE`, DXGI output enumeration.
- Handle connect/disconnect/resolution/refresh-rate/primary changes **without restart**.
- No assumptions about 1920×1080/60 Hz/landscape/identical monitors. Support 1080p/1440p/4K/ultrawide/portrait/mixed refresh/mixed GPUs.
- Modes: **Clone** (same video everywhere) and **Independent** (per-monitor playlists) in v1; **Shared playlist** (all monitors advance through same playlist) is a small v2 extension of Clone.
- Multiple GPU adapters: don't assume adapter 0; prefer efficient locality; reliable fallback if cross-adapter sharing is problematic.
- **Same video on N monitors ⇒ ONE decoder, ONE timeline, ONE source frame; GPU scales per monitor.**

### 1.5.9 Detection and pause policy (all)

- `WallpaperVisibilityManager`, `GameDetector`, `WorkloadMonitor`, central pause controller.
- Pause reasons as bitmask; resume only when **all** reasons clear:

```cpp
enum class PauseReason : uint32_t {
    None            = 0,
    User            = 1 << 0,
    Game            = 1 << 1,
    Fullscreen      = 1 << 2,
    HighCPU         = 1 << 3,
    HighGPU         = 1 << 4,
    HighMemory      = 1 << 5,
    Battery         = 1 << 6,
    Locked          = 1 << 7,
    DisplayOff      = 1 << 8,
    MonitorHidden   = 1 << 9,
    SystemSuspended = 1 << 10,
};
```

- **Fullscreen detection:** foreground window + bounds vs monitor bounds + styles + process; distinguish maximized window from true fullscreen/borderless fullscreen; don't pause for ordinary maximized windows unless configured.
- **Game detection:** event-driven on foreground window change → inspect only the relevant process → classify → **cache**; allowlist ("always pause") and denylist ("never pause"); invalidate cache on process exit / foreground change / config change. No continuous process scanning.
- **Hysteresis + debounce:** pause only after e.g. GPU > 90% for 3 s; resume only after GPU < 70% for 5 s (configurable). Immediate pause for: locked, display off, user pause, system suspend, fullscreen/game.
- **Workload monitor:** CPU/GPU/RAM sampling ~1–2 s; use DXGI/Windows GPU performance counters, not crude guesses; monitoring itself must be cheap; drop unnecessary metrics in GAME/FULLSCREEN/LOCKED/DISPLAY_OFF states.
- **Battery:** detect AC/battery via power notifications; modes Continue / Reduce quality / Pause (default **Pause**).
- **Lock screen / display sleep / suspend:** stop decode + render, release unnecessary GPU resources; resume on unlock/display-on via session/power notifications (`WTSRegisterSessionNotification`, `WM_POWERBROADCAST`, power-setting notifications) — not polling.
- **Paused state:** decoder advancement, frame scheduling, GPU rendering all stop; playback position preserved; only lightweight event/monitoring infra alive. Long pause (configurable, e.g. >5 s) ⇒ release decoder + temporary GPU resources.
- **Resume:** validate decoder/device/monitor → request current frame → restart timing → render; recreate device + decoder resources on device loss.

### 1.5.10 Recovery (all)

- **Device loss:** handle `DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG`: stop rendering → release dependent resources → determine reason → recreate device → recreate MF DXGI manager → recreate textures → restart decoder if needed → resume. No crash.
- **Explorer restart:** detect desktop host invalidation → rediscover desktop windows → recreate wallpaper hosts → restore monitor assignments → resume. No app restart; playlist/config untouched.
- **Video/file errors:** log, mark item temporarily unavailable, advance to next playlist item; allow recovery when file reappears. Handle delete/move/rename/replace gracefully.
- **Corrupt config:** back up the corrupted file, write defaults, continue startup.
- **Recovery must itself be low-resource:** no `while(decoderFailed) recreate();` loops — event → cleanup → delayed retry.

### 1.5.11 UI (all)

- Lightweight native Win32 UI. Sections: **Home, Library, Playlists, Monitors, Performance, Settings**.
- Home: current wallpaper, playback state, current monitor/video, FPS, decoder, GPU.
- Library (v1): add file/folder, remove, list, metadata columns (name, duration, resolution, FPS, codec, HDR, file size), preview. **v2:** search/sort polish, async thumbnails (disk cache + bounded LRU memory cache, released on UI close), drag & drop.
- System tray: Resume, Pause, Next, Previous, Current wallpaper, Open application, Settings, Exit; left-click toggles UI; tray must not keep heavy UI alive.
- Global hotkeys (pause/resume, next, previous) — optional per spec; **deferred to v2**.
- Debug/performance overlay — optional per spec, off by default; **deferred to v2**.

### 1.5.12 Configuration (all)

- Stored in per-user Windows location (`%APPDATA%`), **never next to the executable** when installed under Program Files.
- Contents: general (start with Windows, minimize to tray), playback (mode, shuffle, loop), performance (pause toggles, CPU/GPU/memory thresholds, pause/resume delays), battery (mode). Validate everything on load.

### 1.5.13 Logging (all)

- Levels TRACE/DEBUG/INFO/WARN/ERROR/FATAL; Release default INFO.
- Aggregate events only (e.g. "Playback started", "Decoder = hardware", "Monitor added", "Wallpaper paused: GameDetected", "Dropped frames = 3 over 60 s") — **never per-frame logs**.
- Bounded log rotation: `current.log` + `previous.log`, max ~5–10 MB total.

### 1.5.14 Performance statistics (all)

```cpp
struct PerformanceStats {
    double   cpuUsage;
    double   gpuUsage;
    uint64_t gpuMemoryUsed;
    uint64_t systemMemoryUsed;
    double   decodedFps;
    double   presentedFps;
    uint64_t droppedFrames;
    double   decodeLatencyMs;
    double   renderTimeMs;
    bool     hardwareDecode;
};
```

- Updated at a reasonable interval (~1–2/s), never per frame; UI graphs update ~1–2/s.

---

## 1.6 Resource budgets (targets, not guarantees) (all)

| State | CPU | GPU | Decoder | Frame queue | Timers/polling |
|---|---|---|---|---|---|
| ACTIVE (playing) | as low as possible via HW decode | only necessary render/decode | active | 2–3 frames | frame deadline only |
| REDUCED | minimal optional work | minimal | active if needed | minimal | minimized |
| PAUSED | near-zero active work | 0% wallpaper rendering | stopped | empty/minimal | none |
| SUSPENDED (long pause) | near-zero | 0% | **released** | empty | none |

Explicit budgets:

- 4K RGBA frame ≈ 33 MB ⇒ never keep CPU-side 4K frame buffers; GPU-native path.
- Frame queue ≈ 2–3 frames; do not default to 30/60/120.
- Playlist of 10,000 videos must use very little RAM (paths + small cached metadata only).
- Thumbnail cache: bounded (e.g. ≤100 MB disk, far smaller default; LRU; ≤~50 in-memory).
- Logs: ≤5–10 MB total.
- Threads: ~4–6 total (main/UI, decode worker, render/control worker, low-frequency monitoring, thumbnail worker only while UI open).
- Startup: fast; no full library scan, no thumbnail generation, no process scanning at launch.

---

## 1.7 Security (all)

- Treat video files as untrusted input: no executing files, no loading arbitrary DLLs from video directories, don't trust extensions, validate paths, handle malformed media safely.
- No admin privileges, no services, no kernel drivers, no game injection/hooking, no unrelated system modifications.

---

## 1.8 Default settings (all)

```text
hardware decoding    = enabled
audio                = disabled
auto pause game      = enabled
auto pause fullscreen= enabled
auto pause high GPU  = enabled
auto pause high CPU  = enabled
battery              = pause
shuffle              = disabled
loop                 = enabled
debug overlay        = disabled
verbose logging      = disabled
scaling              = Fill
frame queue          = small (2–3)
performance mode     = Balanced (Performance / Balanced / Quality / Ultra Low Resource)
```

---

## 1.9 Deliverables (all)

- Buildable repository (CMake Debug/Release x64), source, shaders, tests, README.
- Final report: implemented features, build/test results, hardware acceleration status, **actual measured** performance numbers (never invented; state "NOT MEASURED — reason" where hardware/environment is unavailable), known limitations, files created.
- Resource-efficiency audit answering the 20-question list (see `04-testing-profiling.md` §7).

## 1.10 V1 scope vs deferred (v2)

**V1 (this plan):** core playback/render/playlist/multi-monitor (Clone + Independent), detection + resource governor + auto-pause, tray + minimal UI (Home, Playlists, Monitors, Performance, Settings; minimal Library), recovery, config/logging/stats, portable ZIP.

**Deferred to v2** (explicitly optional or "only if implemented and tested" in the specs — see `03-implementation-plan.md` §3.0):

| Item | Reason for deferral |
|---|---|
| Audio playback | Specs: wallpaper playback does not need audio; never initialized. Only `hasAudio` metadata recorded. |
| HDR rendering | Spec: never claim untested HDR. V1 detects HDR + documents behavior + safe SDR fallback. |
| Global hotkeys | Spec: "optional but preferred." |
| Debug/performance overlay | Spec: optional, off by default. |
| Library thumbnails, search, drag & drop | Thumbnails require async/cancellable/bounded machinery; v1 Library covers add/remove/list/metadata/preview. |
| MSI/Inno installer | Spec's preferred option is portable ZIP. |
| Shared-playlist monitor mode | Small v2 extension of Clone. |
