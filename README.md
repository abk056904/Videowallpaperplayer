# Video Wallpaper

A native Windows 11 video wallpaper engine — play video files as animated desktop
wallpapers with the minimum practical resource consumption.

Built as a single native x64 C++23 executable (Win32 + Direct3D 11 + Media Foundation).
No .NET, no Electron, no Qt, no runtime frameworks, no bundled codecs: everything
Windows already provides is used as-is.

> **Status:** v1 complete (milestones M0–M14). The implementation plan lives in
> [`docs/`](docs/), the execution contract in
> [`implement-docs-plan-spec.md`](implement-docs-plan-spec.md), milestone progress in
> [`docs/06-progress-checklist.md`](docs/06-progress-checklist.md), and the final
> measured report in [`docs/07-final-report.md`](docs/07-final-report.md).

## Download

[**⬇ VideoWallpaper-d5f99ca.zip**](https://github.com/abk056904/Videowallpaperplayer/raw/main/dist/VideoWallpaper-d5f99ca.zip) (1.0 MB)

Portable ZIP — extract anywhere, run `VideoWallpaper.exe`. No installer, no admin
rights needed. Includes the three MSVC runtime DLLs, `README.md`, and `LICENSE`.

| File | Size | SHA256 |
|---|---|---|
| `VideoWallpaper-d5f99ca.zip` | 1,068,705 B | `4f50862f…081c` |

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
  │  Win32 UI    │   │ TrayController│  │   Playlist    │   │  LibraryManager  │
  │  (6 panels)  │   └──────────────┘   │   Manager     │   │  (minimal v1)    │
  └──────────────┘                      └──────┬────────┘   └──────────────────┘
                                               │
  ┌────────────────── PlaybackController ───────┴───────────────────────────────┐
  │  FrameScheduler (QPC deadlines, waitable timer) · VideoPlayer · FrameQueue   │
  └───────┬───────────────────────────────────────┬─────────────────────────────┘
          │                                       │
  ┌───────┴─────────┐                    ┌─────────┴──────────┐
  │  DecoderManager │                    │  WallpaperManager  │
  │  (MF source     │                    │  per-monitor hosts │
  │   reader, SW    │                    │  behind icons)     │
  │   or HW MFT)    │                    └─────────┬──────────┘
  └─────────────────┘                              │
  ┌─────────────── ResourceGovernor ──────────────┴─────────────────────────────┐
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
| `video/DecoderManager` | Media Foundation source reader; hardware (DXGI) path with honest software fallback |
| `video/VideoPlayer` | Session lifecycle: open → decode → close; replay; metadata; EOS handling |
| `video/FrameQueue` | Bounded queue (default 3), drop-oldest, buffer recycle pool |
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
| `logging/Logger` | Leveled, rotating file sink in `%APPDATA%\VideoWallpaper\logs\` |
| `config/ConfigurationManager` | UTF-8 JSON config; validate/clamp/defaults; corrupt → `.bak` + defaults; atomic save |
| `util/` | Clock, UTF-8 conversion, scale math, JSON parser |

## Requirements

- **Windows 11** (24H2-era builds verified; Windows 10 21H2+ should work — see
  *Known limitations*). x64 only.
- **GPU with Direct3D 11.1** (feature level 11_1 or 11_0).
- **No runtime frameworks** — only the OS and the MSVC runtime DLLs
  (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`).
- ~1–2 MB disk, ~1.6 MB executable.
- Optional: a hardware video decoder (H.264/HEVC) via Media Foundation; the app
  detects it honestly and falls back to software decode when absent.

## Build instructions

### Prerequisites

- Visual Studio 2022 Build Tools (or VS 2022) with the **Desktop development with
  C++** workload — MSVC 19.44+ (verified 14.44.35207), x64 toolset.
- Windows SDK 10.0.26100.0 (or newer; includes Media Foundation, D3D11, DXGI).
- CMake ≥ 3.28 (verified 4.4.2).

### Configure & build

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

## Supported codecs & containers

Decoding is provided entirely by **Media Foundation** (no bundled codecs):

| Codec | Support |
|---|---|
| H.264 (AVC) | ✅ verified (hardware where the MF stack offers it, else software) |
| HEVC (H.265) | ✅ hardware where the MF stack offers it; software fallback (decoder availability dependent) |
| AV1 / VP9 | ✅ **where the OS/hardware provides an MFT** (Microsoft Store AV1/VP9 extensions, GPU vendor MFTs); otherwise `NOT MEASURED` on this dev machine |
| Audio | ❌ intentionally never initialized (v1 is video-only) |

Containers: **MP4/MOV** (verified), **MKV/WebM/AVI** — anything the Media Foundation
Source Resolver can open; the app validates by real media metadata, never by file
extension.

## Hardware acceleration

- The app requests the **hardware path first**: `MF_SOURCE_READER_D3D_MANAGER` +
  `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`, NV12/P010 GPU surfaces, and a shader
  that converts YUV→RGB on the GPU — **zero CPU frame copies in the happy path**.
- The actual decoder is **probed at runtime and reported honestly** (log line
  `decoder: hardware (vendor)` or `decoder: software`). A one-sample DXGI-buffer
  probe after negotiation verifies the decoder really hands out GPU surfaces; if it
  does not, the reader is rebuilt on the proven software RGB32 path (with
  diagnostics). On machines where MF hardware decode is unavailable (including this
  dev machine — see *Known limitations*), the app plays in software automatically.
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
iGPU, 1920×1080 @ 144 Hz, Windows 11 24H2), Release build, 1440p60 H.264:

| State | CPU | Private RAM | Handles | Threads |
|---|---|---|---|---|
| **Playing** (software decode) | ~190% (decode-limited; ~23 ms/f MF software decode — no HW MFT on this machine) | ~408–424 MB | ~1367 | ~37–40 |
| **Paused** | 0–5% | ~388 MB | flat | flat |
| **Suspended** (long pause) | **0.0–0.8%** | **~124 MB** (decoder + GPU resources released) | flat | flat |

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
| Software decode, high CPU | This machine's MF stack has no hardware MFT (log: `hardware decode unavailable … software RGB32 path`). Install hardware-video extensions / GPU drivers for HW decode; on HW-capable machines the app uses it automatically. |
| Second instance won't start | It's not supposed to — a second launch activates the first (single instance). |
| Config resets to defaults | Corrupt config was detected → `.bak` written next to it, defaults applied, app continues. |
| No tray icon | The tray icon lives while the app runs; it can be hidden by Windows tray settings (show hidden icons). |

Logs: `%APPDATA%\VideoWallpaper\logs\current.log` (rotates at 1 MB → `previous.log`).

## Known limitations (v1)

- **Audio is not played** (video-only by design; audio streams are never initialized).
- **No thumbnails, drag & drop, or global hotkeys** (v2 items; the library is minimal).
- **Shared-playlist mode across monitors** is v2; v1 has clone (decode-once shared
  frame) and independent (per-monitor) modes.
- **No installer** — portable ZIP (installer is v2).
- **Windows 10 / 32-bit / ARM**: not verified (x64 Windows 11 is the target).
- **This dev machine's limits** (reported `NOT MEASURED` in the final report): no
  MF hardware decoder available (software path exercised end-to-end), single display
  (multi-monitor verified on simulated topologies), no AV1/VP9/HDR sample rows,
  no VRAM measurement (no GPU decode), and disruptive tests (lock screen, system
  suspend, GPU driver reset, 24 h soak) were declined — see
  [`docs/07-final-report.md`](docs/07-final-report.md).

## Development

- Layout: `src/` (per-domain modules), `tests/` (doctest, unit-test only),
  `harness/` (dev-only GPU/wallpaper harness), `shaders/` (HLSL, compiled at build
  time and embedded — no runtime file lookups), `docs/` (plan + spec + report),
  `build/<cfg>/generated/` (generated shader headers).
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
- **Paused ⇒ no video work**: decoder stopped, queue cleared, timer cancelled.
- **Suspended (long pause) ⇒ release everything not needed**: decoder, next-video
  prep, and temporary GPU resources are released; position/path/config are kept.
  Measured: 0.0–0.8% CPU, ~124 MB RAM while suspended.
- **Hidden/covered ⇒ nothing renders**: per-host presents stop when the wallpaper
  can't be seen; lock/display-off are events, not polls.
- **Monitor → no CPU**: workload sampling is 1–2 s; game/fullscreen/lock/power are
  **event-driven** (`SetWinEventHook`, `WTSRegisterSessionNotification`,
  `WM_POWERBROADCAST`).
- **UI closed ⇒ zero UI cost**: closing the window destroys the UI and its timers;
  the tray keeps the engine running.
- **No frameworks, no bundled codecs, no unnecessary assets** — the OS's own
  decoders and a ~1.6 MB self-contained exe.

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
