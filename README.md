# Video Wallpaper

A native Windows 11 video wallpaper engine — play video files as animated desktop
wallpapers with the minimum practical resource consumption.

Built as a single native x64 C++23 executable (Win32 + Direct3D 11 + Media Foundation).
No .NET, no Electron, no Qt, no runtime frameworks, no bundled codecs: everything
Windows already provides is used as-is.

> **Status:** v1.2 complete — adds FFmpeg hardware decode (D3D11VA / CUDA / NVDEC)
> with GPU-accelerated decode (zero CPU↔GPU copies), full audio pipeline (WASAPI + FFmpeg + swresample),
> library thumbnails, global hotkeys, drag & drop, crash reporting, auto-update checker,
> per-monitor volume/scaling, and a codec-aware decoder factory. The v1 milestones (M0–M14) are in
> [`docs/`](docs/), the execution contract in
> [`implement-docs-plan-spec.md`](implement-docs-plan-spec.md), milestone progress in
> [`docs/06-progress-checklist.md`](docs/06-progress-checklist.md), and the final
> measured report in [`docs/07-final-report.md`](docs/07-final-report.md).

## Download

Build from source (see [Build instructions](#build-instructions)) or use the
distribution scripts:

```bat
powershell -ExecutionPolicy Bypass -File package.ps1
```

**Requirements:** Windows 11 (or 10 21H2+), x64, GPU with Direct3D 11.1.

---

## Overview

Video Wallpaper renders the video wallpaper **behind the desktop icons** (the same
layer Windows uses for the static wallpaper picture), on every monitor, from a
playlist you control — while staying out of the way:

- Renders only when a new frame is due (**source-FPS pacing**, never monitor-refresh
  redraws of static frames) and only while the wallpaper is actually visible.
- Detects games, fullscreen applications, high system load, lock, display-off, and
  battery status, and pauses or suspends itself automatically.
- Long pauses fully release the decoder and temporary GPU resources.
- A system tray icon keeps controls (resume / pause / next / previous) one click away
  even when the UI window is closed.

## Architecture

```
┌───────────────────────────── ApplicationController ─────────────────────────────┐
│  single instance · message pump (blocks when idle) · command queue · shutdown   │
└──────┬──────────────────────────────────────────────────────────────────────────┘
       │
  ┌────┴─────────┐   ┌──────────────┐   ┌───────────────┐   ┌──────────────────┐
  │  Win32 UI    │   │TrayController│   │   Playlist    │   │  LibraryManager  │
  │  (6 panels)  │   └──────────────┘   │   Manager     │   │  (minimal v1)    │
  └──────────────┘                      └──────┬────────┘   └──────────────────┘
                                               │
  ┌────────────────── PlaybackController ──────┴────────────────────────────────┐
  │  FrameScheduler (QPC deadlines, waitable timer) · VideoPlayer · FrameQueue  │
  └───────┬───────────────────────────────────────┬─────────────────────────────┘
          │                                       │
  ┌───────┴─────────┐                    ┌────────┴───────────┐
  │  DecoderManager │                    │  WallpaperManager  │
  │  (MF source     │                    │  per-monitor hosts │
  │   reader, SW    │                    │  behind icons)     │
  │   or HW MFT)    │                    └─────────┬──────────┘
  └─────────────────┘                              │
  ┌─────────────── ResourceGovernor ───────────────┴─────────────────────────────┐
  │  sole authority over ACTIVE / PAUSED / SUSPENDED · pause reasons: user,      │
  │  game, fullscreen, workload, lock, display-off, battery · long-pause release │
  └──────────────────────────────────────────────────────────────────────────────┘
  ┌─────────────── Detection & monitoring ───────────────────────────────────────┐
  │  WorkloadMonitor (CPU/RAM/VRAM, 1–2 s) · GameDetector · FullscreenDetector   │
  │  · SystemStateMonitor (lock/power/display/battery — event-driven)            │
  └──────────────────────────────────────────────────────────────────────────────┘
```

Key components (all under `src/`):

| Module | Purpose |
|---|---|
| `app/ApplicationController` | Composition root; owns every subsystem; single instance; command queue; deterministic shutdown |
| `app/ControlWindow` | Hidden tool window; receives registered messages (`Playback.Pause/Resume/Stop`, `Focus`) |
| `gfx/D3D11DeviceManager` | Device + DXGI adapter/output enumeration, feature level 11_1, device-loss handling |
| `gfx/D3D11Renderer` | Vertex-less fullscreen triangle, embedded shaders, video texture sampling |
| `gfx/TextureManager` | Shared GPU frames (NV12/P010 planes or RGB32 upload) |
| `wallpaper/WallpaperHost` | Per-monitor child window in the wallpaper layer (behind icons), swap chain |
| `wallpaper/WallpaperManager` | Host lifecycle, monitor events, per-monitor frame routing, device-loss recreate |
| `monitors/MonitorManager` | Enumeration, stable ids, add/remove/change events, adapter association |
| `video/IVideoDecoder` | Abstract decoder interface — all backends (MF, NVDEC, FFmpeg SW) implement this |
| `video/DecoderFactory` | Codec-aware factory: MF → NVDEC/CUDA → FFmpeg software; returns the best available decoder |
| `video/FFmpegDecoder` | FFmpeg decode backend: D3D11VA (GPU-to-GPU, zero CPU↔GPU), CUDA (→D3D11 map), or software (NV12/BGRA) |
| `video/DecoderManager` | Media Foundation source reader; hardware (DXGI) path with honest software fallback |
| `video/VideoPlayer` | Session lifecycle: open → decode → close; replay; metadata; EOS handling |
| `video/FrameQueue` | Bounded queue (default 1, configurable), drop-oldest, buffer recycle pool |
| `playback/FrameScheduler` | Source-FPS pacing via QPC + waitable timer; no busy loop |
| `playback/PlaybackController` | Owns player + scheduler + queue; stats feed; pause/resume/stop/replay |
| `playlist/PlaylistManager` | Items, modes (single/sequential/loop/shuffle), persistence, next-video prep |
| `governor/ResourceGovernor` | ACTIVE/PAUSED/SUSPENDED state machine; pause-reason bitmask; long-pause release |
| `performance/WorkloadMonitor` | CPU/RAM/VRAM sampling with hysteresis |
| `detection/GameDetector` | Foreground-process classification vs allow/deny lists (event-driven) |
| `detection/FullscreenDetector` | True/borderless fullscreen vs maximized |
| `system/SystemStateMonitor` | Lock, power, display-off, battery notifications |
| `library/LibraryManager` | Minimal library: add/remove/list, incremental scan, lazy metadata |
| `ui/Win32UI` + `ui/panels/*` | Six-tab DPI-aware window (Home, Library, Playlists, Monitors, Performance, Settings) |
| `ui/TrayController` | Tray icon + menu; left-click toggles the UI |
| `library/ThumbnailExtractor` | FFmpeg-based async thumbnail extraction (background worker, disk cache) |
| `system/SystemStateMonitor` | Lock, power, display-off, battery notifications |
| `util/CrashReport` | Minidump writer for unhandled exceptions |
| `util/UpdateChecker` | Background GitHub releases check on startup |
| `util/FileAssoc` | Windows file association registration (.mp4, .mkv, etc.) |
| `logging/Logger` | Leveled, rotating file sink in `%APPDATA%\VideoWallpaper\logs\` |
| `config/ConfigurationManager` | UTF-8 JSON config; validate/clamp/defaults; corrupt → `.bak` + defaults; atomic save |
| `util/` | Clock, UTF-8 conversion, scale math, JSON parser, FFmpeg lazy-load |

## Requirements

- **Windows 11** (24H2-era builds verified; Windows 10 21H2+ should work — see
  *Known limitations*). x64 only.
- **Portable mode**: place `portable.ini` next to the exe for self-contained data storage.
- **GPU with Direct3D 11.1** (feature level 11_1 or 11_0).
- **No runtime frameworks** — only the OS and the MSVC runtime DLLs
  (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`).
- ~18 MB total (1.9 MB exe + ~13 MB FFmpeg DLLs + runtime DLLs). Installer: 6.2 MiB.
- D3D11VA hardware decode for H.264/HEVC (via FFmpeg); falls back to software
  when GPU decode is unavailable.

## Build instructions

### Prerequisites

- Visual Studio 2022 Build Tools (or VS 2022) with the **Desktop development with
  C++** workload — MSVC 19.44+ (verified 14.44.35207), x64 toolset.
- Windows SDK 10.0.26100.0 (or newer; includes Media Foundation, D3D11, DXGI).
- CMake ≥ 3.28 (verified 4.4.2).

### Configure & build (CMake presets)

```bat
cmake --preset release
cmake --build --preset release
```

Or without presets:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
cmake --build build --config Release
```

Both configurations build warning-free (`/WX`). Debug enables the D3D11 debug layer
(requires the Windows "Graphics Tools" optional feature); Release is fully optimized
(`/O2`, LTCG) with PDBs.

### Tests

```bat
cmake --preset debug
ctest --preset debug
```

Or without presets:

```bat
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build -C Release --output-on-failure
```

~170 unit-test cases in both configurations. Hardware/desktop integration is covered
by the harnesses (`vw_gfx_harness`, dev-only) rather than the default test run.

### Package (portable ZIP)

```bat
powershell -ExecutionPolicy Bypass -File package.ps1
```

Produces `dist\VideoWallpaper-<commit>.zip` containing `VideoWallpaper.exe`,
the three MSVC runtime DLLs, `README.md`, and `LICENSE` — nothing else.

## Running

### Portable Mode

Place a `portable.ini` file (can be empty) next to `VideoWallpaper.exe`. All data
(config, logs, playlists, thumbnails, crash dumps) will be stored in the exe's
directory instead of `%APPDATA%`. This makes the app fully self-contained — ideal
for USB drives or portable installs.

### Normal Mode

- Launch `VideoWallpaper.exe`. The wallpaper appears behind your desktop icons
  within ~1–2 s.
- **First run** creates `%APPDATA%\VideoWallpaper\config.json` (UTF-8 JSON) with
  defaults, including `playback.videoPath`. Point it at a video file, or use the UI:
  **Home → Set video…** or add files via the Library/Playlists tabs.
- **Single instance**: a second launch activates the first and exits.
- **Tray**: right-click for Resume/Pause/Next/Previous/Open/Settings/Exit;
  left-click toggles the UI window.
- **Start with Windows**: Settings → *Start with Windows* (HKCU `Run` entry —
  no service, no admin, no elevated anything).
- **CLI/debug**: `vw_gfx_harness --help` lists the harness modes
  (`--video`, `--wallpaper`, `--device-loss`, `--frames N`, `--scaling`).

## UI Panels

The app features a modern dark-themed DPI-aware window with **6 tabs**:

### 🏠 Home

Dashboard showing the current wallpaper state:
- **Playback status** (Playing / Paused / Stopped)
- **Current video** name, resolution, codec
- **Real-time stats**: decoded FPS, presented FPS, dropped frames, decode latency, render time
- **System stats**: CPU %, GPU memory, RAM usage
- **Adapter name** (GPU model)
- **Playback controls**: Pause / Next / Previous buttons

### 📚 Library

Video file browser with lazy metadata extraction:
- **ListView** with columns: Name, Duration, Resolution, FPS, Codec, HDR, Size
- **Thumbnails**: auto-extracted from video files (background worker, cached in `%APPDATA%\VideoWallpaper\thumbnails\`)
- **File picker**: Add videos via Open File dialog
- **Double-click** to set as wallpaper
- **Drag & drop** files onto the panel to add them

### 📋 Playlists

Playlist management with multiple modes:
- **Add / Remove / Reorder** items (Up/Down buttons)
- **Enable / Disable** individual items (skip without removing)
- **Modes**: Single, Sequential, Loop, Shuffle
- **Import / Export**: M3U file format for sharing playlists
- **Persistence**: saved to `%APPDATA%\VideoWallpaper\playlist.json`

### 🖥️ Monitors

Per-monitor configuration:
- **Monitor list**: shows all displays with resolution, refresh rate, primary flag
- **Frame preview**: live snapshot of the current video frame on the selected monitor
- **Wallpaper mode**: Clone (shared) / Independent (per-monitor)
- **Scaling mode**: per-monitor Fill / Fit / Stretch / Center
- **Volume slider**: per-monitor volume (0–100%)

### 📊 Performance

Resource monitoring and pause triggers:
- **Battery mode**: Continue / Reduce Quality / Pause
- **Pause toggles**: Game / Fullscreen / High CPU / High GPU / High RAM
- **Thresholds**: CPU/GPU/RAM pause and resume percentages (hysteresis engine)
- **Timing**: Pause delay, Resume delay, Long-pause release (SUSPENDED)
- **Advanced**: Performance mode (Balanced / Performance / Quality / Ultra-Low-Resource)
- **Config export/import** buttons

### ⚙️ Settings

General app configuration:
- **Start with Windows** (HKCU Run key)
- **Minimize to tray** on close
- **File associations** (.mp4, .mkv, etc. — registers with Windows)
- **Logging level** (Info / Debug / Warn / Error)
- **Playback speed** (0.5× – 2.0×)
- **Frame queue depth** (1–16)

### Global Hotkeys

| Shortcut | Action |
|----------|--------|
| `Ctrl+Alt+V` | Play / Pause toggle |
| `Ctrl+Alt+←` | Previous video |
| `Ctrl+Alt+→` | Next video |
| `Ctrl+Alt+D` | Toggle debug overlay (FPS, decoder, RAM on wallpaper) |

---

## Feature List

### Core Playback
- ✅ Play video files as animated desktop wallpaper (behind icons)
- ✅ Playlist with Single / Sequential / Loop / Shuffle modes
- ✅ Playlist import/export (M3U)
- ✅ Source-FPS pacing (no busy loops, no monitor-refresh redraws)
- ✅ Zero-copy GPU decode (D3D11VA, CUDA, MF hardware)
- ✅ Codec-aware decoder factory (H.264 → HEVC → VP9 → AV1)
- ✅ Audio playback (WASAPI + FFmpeg, volume control)
- ✅ Playback speed control (0.25× – 4×)
- ✅ Drag & drop to add files
- ✅ File associations (.mp4, .mkv, etc.)

### Multi-Monitor
- ✅ Per-monitor wallpaper hosts (auto-created on display change)
- ✅ Clone mode (decode once, render on all monitors)
- ✅ Independent mode (per-monitor playlists)
- ✅ Per-monitor volume control
- ✅ Per-monitor scaling presets
- ✅ Mixed refresh rate support

### Resource Management
- ✅ Automatic pause on game / fullscreen / high CPU / high GPU / high RAM
- ✅ Hysteresis-based pause/resume (no flapping)
- ✅ Long-pause decoder release (SUSPENDED state)
- ✅ `EmptyWorkingSet()` on suspend (frees physical RAM)
- ✅ Battery mode (Continue / Reduce Quality / Pause)
- ✅ Lock / display-off / suspend detection

### UI & UX
- ✅ Modern dark theme (DPI-aware, Win32 native)
- ✅ System tray with context menu
- ✅ Global hotkeys (play/pause, next/prev, debug overlay)
- ✅ Library with thumbnails (FFmpeg frame extraction)
- ✅ Config import/export (JSON)
- ✅ Debug overlay (FPS, decoder, RAM — Ctrl+Alt+D)
- ✅ Start minimized to tray option
- ✅ Minimize-to-tray on close

### System Integration
- ✅ Start with Windows (HKCU Run key, no admin)
- ✅ Single-instance (second launch activates first)
- ✅ Explorer-restart recovery (auto-rebuild wallpaper layer)
- ✅ GPU device-loss recovery (auto-recreate with backoff)
- ✅ Crash reporting (Minidump on unhandled exception)
- ✅ Auto-update checker (queries GitHub releases)

### Developer
- ✅ GitHub Actions CI/CD pipeline
- ✅ Nightly soak automation
- ✅ ~172 unit tests (doctest, Debug + Release)
- ✅ Minimal FFmpeg build (~13 MiB DLLs, lazy-loaded)
- ✅ Inno Setup installer (6.2 MiB)

---

## Supported codecs & containers

Decoding is handled by a **codec-aware factory** that selects the best available
backend:

| Codec | Backend priority |
|---|---|
| H.264 (AVC) | Media Foundation HW → D3D11VA → CUDA → FFmpeg software |
| HEVC (H.265) | Media Foundation HW → D3D11VA → CUDA → FFmpeg software |
| VP9 | D3D11VA → CUDA → FFmpeg software |
| AV1 | CUDA → FFmpeg software |
| Other | FFmpeg software |

| Audio codec | Support |
|---|---|
| AAC, MP3, FLAC, Opus, Vorbis, etc. | ✅ decoded via FFmpeg, resampled to 48 kHz S16 stereo, output via WASAPI |

Containers: **MP4/MOV** (verified), **MKV/WebM/AVI** — anything FFmpeg's libavformat
can open; the app validates by real media metadata, never by file extension.

## Hardware acceleration

The decoder factory (`CreateBestDecoder`) selects the best available backend:

1. **H.264/HEVC**: tries Media Foundation first (proven zero-copy via `MF_SOURCE_READER_D3D_MANAGER`),
   then falls through to FFmpeg D3D11VA, then CUDA, then software.
2. **VP9/AV1**: tries FFmpeg D3D11VA, then CUDA (cuvid), then software.
3. **Other codecs**: FFmpeg software directly.

### GPU-accelerated decode paths (zero CPU↔GPU copies)

- **D3D11VA** (H.264/HEVC/VP9): FFmpeg decodes on its own D3D11 device; the
  decoded texture is copied GPU-to-GPU via `CopySubresourceRegion` to a shared
  texture, then opened on the render device via `OpenSharedResource1`. **Zero
  CPU↔GPU copies** — the copy is a GPU command. D3D11 immediate contexts are
  not thread-safe, so sharing the render device directly with FFmpeg is not
  possible.
- **MF HW** (H.264/HEVC): Media Foundation decodes directly onto the render
  device — **true zero-copy** (no copies at all).
- **CUDA** (H.264/HEVC/VP9/AV1): decoded on the GPU, then mapped to D3D11 textures
  via `av_hwframe_map()` — also zero-copy.
- **Software**: frames are NV12 or BGRA on the CPU; NV12 is uploaded to the GPU
  where the YUV shader converts + scales (62% less upload than RGB32).

The actual decoder is **probed at runtime and reported honestly** (log line
`decoder: hardware (vendor)` or `decoder: software`). A failed hardware initialization
never terminates playback — the factory falls back automatically.

- Scaling modes (Fill default / Fit / Stretch / Center) are applied in the shader;
  `Fill` crops overflow evenly on both sides to fill the screen with no bars and no
  distortion (any aspect ratio, including anamorphic content — sample aspect ratio
  is honored).

## Multi-monitor support

- A wallpaper host is created on **every monitor**; monitor add/remove/change events
  create/destroy/reposition hosts **without restart**.
- **Clone mode** (default): one decoder + one timeline, N GPU renderers — the video
  is decoded **once** and shared.
- **Independent mode**: each monitor plays its own video (per-monitor playlist items).
- Mixed refresh rates: each host presents on its own vsync.
- Hybrid-GPU laptops: monitors are associated with the DXGI output that drives them
  (adapter locality), with a documented per-adapter fallback.
- *Real multi-monitor behavior was verified on simulated topologies only — this dev
  machine has a single display (see Known limitations).*

## Performance behavior (measured)

Measured on the dev machine (Ryzen 5 7535HS, 13.8 GB RAM, RTX 3050 Laptop + Radeon
iGPU, 1920×1080 @ 144 Hz, Windows 11 24H2), Release build, 1440p60 H.264 + HEVC:

| State | CPU | RAM (RSS) | Handles | Threads |
|---|---|---|---|---|
| **Playing** (D3D11VA HW decode) | ~59% | ~215–235 MB | ~1876 | ~97 |
| **Paused** | 0–5% | ~200 MB | flat | flat |
| **Suspended** (long pause) | **0.0–0.8%** | **~124 MB** (decoder + GPU resources released) | flat | flat |

- D3D11VA hardware decode delivers **60 FPS at 0 dropped frames** for H.264 and
  HEVC 2560×1440 content.
- HEVC D3D11VA now works correctly (auto-detects P010/NV12 format, converts via
  swscale when needed).
- 5-minute soak test: memory stable (no leaks), zero frame drops, CPU time flat.

- The software decode cost is inherent to the machine's MF stack (no hardware MFT
  exists here); on hardware-decode-capable machines the decode moves to the GPU
  engine and CPU drops to single digits.
- The per-frame hot path was measured and optimized: the decode worker now recycles
  frame buffers through a pool (**resize+zero 4.5 → 0.00 ms/f**; the remaining
  ~1.45 ms/f is the irreducible 14.7 MB RGB32 copy).
- A 4–8 h soak (spec-reduced from 24 h — see the report) plus aggressive
  play/pause/UI leak cycles showed **no monotonic memory/handle/thread growth**.
- Idle behavior: message pump and worker threads **block** (no busy waits, zero
  `Sleep()` in the engine); the render deadline is a waitable timer; monitoring is
  1–2 s or event-driven; the UI consumes nothing when closed.

## Game & fullscreen detection

- **Games**: the foreground process is classified against **allow/deny lists**
  (exe name/path) in the config. A classified game **pauses** the wallpaper
  (configurable). No injection, no hooks, no admin, no repeated process scanning —
  the foreground change is an **event** (`SetWinEventHook`), not a poll.
- **Fullscreen**: true fullscreen and borderless-fullscreen are distinguished from
  maximized (maximized ≠ fullscreen unless configured). A fullscreen window pauses
  the wallpaper; Alt+Tab out resumes it.
- Both detectors cache classifications and invalidate on exit/foreground/config
  change — no flapping.

## Configuration

`%APPDATA%\VideoWallpaper\config.json` (UTF-8 JSON, atomic writes, debounced saves,
`.bak` backup + defaults on corruption). Sections:

| Section | Key settings |
|---|---|
| `general` | logging level |
| `playback` | `videoPath`, scaling mode (fill/fit/stretch/center), playlist mode, loop, start-with-Windows |
| `wallpaper` | mode (clone / independent) |
| `performance` | workload thresholds + hysteresis delays (pause/resume), monitoring interval |
| `battery` | battery policy (continue / reduce quality / pause) |
| `detection` | game allow/deny lists, fullscreen detection, treat-maximized-as-fullscreen |

Threshold pairs (pause ≥ resume) are cross-validated at load and on every config
change; invalid pairs are clamped and logged. The governor reacts to live config
changes via a revision counter (no polling).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Wallpaper not visible | Explorer's wallpaper layer may need a refresh (lock/unlock or `explorer.exe` restart); the app detects layer invalidation and rebuilds automatically. Check `%APPDATA%\VideoWallpaper\logs\` for `wallpaper layer` lines. |
| Video doesn't play | Log shows `cannot open video …` — check the path, the file, and the container/codec. The app validates by real metadata; corrupt files are skipped gracefully (playlist advances). |
| Software decode, high CPU | The decoder factory tried all HW backends (MF, D3D11VA, CUDA) and fell back to software. Check GPU drivers are up to date; for NVIDIA, install the CUDA toolkit or update to a driver that includes NVDEC support. On HW-capable machines the app auto-selects the best path. |
| Second instance won't start | It's not supposed to — a second launch activates the first (single instance). |
| Config resets to defaults | Corrupt config was detected → `.bak` written next to it, defaults applied, app continues. |
| No tray icon | The tray icon lives while the app runs; it can be hidden by Windows tray settings (show hidden icons). |

Logs: `%APPDATA%\VideoWallpaper\logs\current.log` (rotates at 1 MB → `previous.log`).

## Known limitations (v1.2)

- **Per-monitor scaling presets** persist in config but the UI updates are deferred to v2.
- **Shared-playlist mode** (one playlist synced across all monitors in Independent mode)
  is a v2 feature; v1 has Clone (decode-once shared frame) and Independent (per-monitor)
  modes.
- **Windows 10 / 32-bit / ARM**: not verified (x64 Windows 11 is the target).
- **Multi-monitor real hardware test**: verified on simulated topologies only — this dev
  machine has a single display.

## Development

- Layout: `src/` (per-domain modules), `tests/` (doctest, unit-test only),
  `harness/` (dev-only GPU/wallpaper harness), `shaders/` (HLSL, compiled at build
  time and embedded — no runtime file lookups), `ext/ffmpeg/minimal/` (FFmpeg
  shared libraries — minimal build: avcodec, avformat, avutil, swscale, swresample),
  `docs/` (plan + spec + report), `build/<cfg>/generated/` (generated shader headers).
- Conventions: C++23, RAII everywhere (no raw `new`/`malloc`), workers block on
  events/CVs (no busy loops, no `Sleep()`), no per-frame allocations or logging,
  `/WX` clean, everything unit-tested before a milestone advances.
- The design contract (command/notification interfaces, acceptance mapping, UI panel
  specs) lives in [`implement-docs-plan-spec.md`](implement-docs-plan-spec.md).

## Testing

- **Unit tests** (~170 cases, Debug + Release): config/JSON edge cases, logger
  concurrency + rotation, UTF-8 round-trips, scale math, FrameQueue + recycle pool,
  scheduler pacing, playlist modes/shuffle/persistence, monitor topology diff,
  governor transition table, detection classification, library scan/watch.
- **Integration**: harnesses for the GPU path (`--video`, `--wallpaper`,
  `--device-loss`), live-verified on this machine's GPUs (vsync-locked ~147 FPS).
- **Live verification per milestone**: playback, pause/resume/stop, loop/shuffle,
  Explorer-restart recovery, device-loss injection, leak cycles, baselines — all
  recorded in `docs/06-progress-checklist.md` with real measured numbers.

## How low idle usage is achieved

The design principle is *"wake only when necessary"* (doc 2 §102):

- **Message pump blocks** (`GetMessageW`/`MsgWaitForMultipleObjects`) — no timer
  spins when nothing is due.
- **Render only when a frame is due**: the scheduler wakes on a waitable timer set to
  the next source-FPS deadline — a 30 FPS video presents 30 times/s, not 144. A
  static frame is **never redrawn**.
- **Decode only on demand**: the decode worker blocks when the bounded queue (3
  frames) is full — backpressure, no speculative decode.
- **Paused ⇒ no video/audio work**: decoder stopped, audio pipeline paused, queue cleared,
  timer cancelled.
- **Suspended (long pause) ⇒ release everything not needed**: decoder, audio pipeline,
  next-video prep, and temporary GPU resources are released; position/path/config
  are kept. Measured: 0.0–0.8% CPU, ~124 MB RAM while suspended.
- **Hidden/covered ⇒ nothing renders**: per-host presents stop when the wallpaper
  can't be seen; lock/display-off are events, not polls.
- **Monitor → no CPU**: workload sampling is 1–2 s; game/fullscreen/lock/power are
  **event-driven** (`SetWinEventHook`, `WTSRegisterSessionNotification`,
  `WM_POWERBROADCAST`).
- **UI closed ⇒ zero UI cost**: closing the window destroys the UI and its timers;
  the tray keeps the engine running.
- **No frameworks, no unnecessary assets** — a minimal FFmpeg build (~31 MB DLLs)
  plus the OS's own decoders in a self-contained exe.

## License

Apache-2.0 — see [`LICENSE`](LICENSE).

freebuff --continue 2026-08-19T15-29-22.710Z