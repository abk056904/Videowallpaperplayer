# 7. Final Report (M14)

> Date: 2026-08-17 · Build: Release x64, commits `c2b2e41` (M13 review) through
> `4d9e341` (M14). All numbers below were **measured on the dev machine** (Windows 11 Home
> 24H2 build 26200, Ryzen 5 7535HS, 13.8 GB RAM, NVIDIA RTX 3050 Laptop + AMD Radeon
> iGPU, 1920×1080 @ 144 Hz physical) unless explicitly marked `NOT MEASURED`.
> **No number is invented.** Anything that could not be measured is stated as
> `NOT MEASURED — reason`.

---

## Implemented

Complete v1 plan, milestones M0–M14 (see `docs/06-progress-checklist.md` for the
per-milestone detail):

- **M1** Build skeleton: CMake x64 C++23, hidden control window, single instance,
  leveled rotating logger, UTF-8 JSON config with corrupt-file recovery, deterministic
  shutdown.
- **M2** Direct3D 11 renderer: device/DXGI enumeration, vertex-less fullscreen
  triangle, build-time-compiled embedded shaders, device-loss plumbing.
- **M3** Wallpaper host behind desktop icons (both Explorer arrangements handled),
  per-monitor hosts, Explorer-restart recovery.
- **M4** Media Foundation playback (software first): source reader, RGB32 decode,
  bounded FrameQueue, pause/resume/stop/EOS, corrupt-file grace.
- **M5** Hardware decoding + GPU color: DXGI manager, NV12/P010 GPU surfaces, YUV
  shader, honest runtime probe with clean software fallback (this machine has no
  hardware MFT — see *Hardware acceleration*).
- **M6** Frame timing & queue: source-FPS pacing, zero busy-wait scheduler, pause
  preserves position, stats collection.
- **M7** Playlist engine: items/modes/persistence, next-video prep, same-item loop
  without reopen.
- **M8** Multi-monitor & multi-GPU: per-monitor hosts, clone (decode-once) and
  independent modes, adapter association (simulated topologies; single display here).
- **M9** Detection & monitoring: CPU/RAM/VRAM sampling with hysteresis,
  event-driven game/fullscreen classification.
- **M10** Resource governor & suspension: ACTIVE/PAUSED/SUSPENDED, pause-reason
  bitmask, battery policy, long-pause release, threshold cross-validation.
- **M11** UI, tray & minimal library: six-tab Win32 UI, tray, library with
  incremental scan, debounced config writes.
- **M12** Recovery hardening: Explorer restart, device loss (injection-verified),
  decoder/file failure retry, playing-file rename, config corruption.
- **M13** Profiling, optimization & stability: code-search audit clean; baselines;
  frame-buffer recycle pool (measured win); leak-cycle stress green; soak (running —
  see *Known limitations* for the reduced duration).
- **M14** Packaging (portable ZIP), this report, resource-efficiency audit
  (`docs/08-resource-audit.md`), version resource.

## Build

- Compiler MSVC **19.44.35207** (`/std:c++23`), Windows SDK **10.0.26100.0**,
  CMake **4.4.2**, generator `Visual Studio 17 2022` (x64).
- **Debug and Release both build green, 0 warnings** (`/WX`; Debug adds `/RTC1` +
  D3D11 debug layer; Release `/O2` + LTCG + PDBs).
- Build commands: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` then
  `cmake --build build --config Release` (Debug likewise).

## Tests

- **171/171 unit-test cases** (Debug + Release), 0 warnings. (Assertion counts are not stated — they vary run-to-run because the lazy-metadata and library-watch tests are timing-dependent; see the M14 review note in BUILD_NOTES about the requestMetadata in-flight dedup fix.)
- Coverage: config/JSON edge cases, logger concurrency/rotation/UTF-8, UTF-8
  round-trips, scale math, FrameQueue + recycle pool, scheduler pacing, playlist
  modes/shuffle/persistence, monitor topology diff, governor transition table,
  detection classification, library scan/watch, device-loss classification.
- Harness integration (dev-only targets): GPU render on both GPUs, `--wallpaper`
  mode behind icons, `--video` frame decode, `--device-loss` injection, all
  live-verified on this machine.

## Hardware acceleration

- The GPU path is **implemented and unit-tested for its machinery** (two-SRV planar
  views, `DecodedFrame` GPU ownership, YUV shader with limited→full range), and the
  runtime probe + clean software fallback is live-verified.
- **`NOT MEASURED` — end-to-end GPU decode on this machine**: the Media Foundation
  stack here registers a hardware MFT (`AMDhwDecoder`) but it is an async MFT that
  exposes zero types and rejects the D3D manager; the MS H.264/HEVC decoder MFTs
  silently allocate system-memory NV12 even driven directly with the DXGI manager
  (full forensic chain in BUILD_NOTES, M5). The app therefore plays via the software
  RGB32 path here, reported honestly in the log (`decoder: software`).

## Performance observations (measured)

| State | CPU | Private RAM | Handles | Threads |
|---|---|---|---|---|
| Playing (1440p60 H.264, software decode) | ~190% (decode-limited) | ~408–424 MB | ~1367 | ~37–40 |
| Paused | 0–5% | ~388 MB | flat | flat |
| Suspended (long pause) | **0.0–0.8%** | **~124 MB** | flat | flat |

- Present rate = **source FPS** (36.4 presented ≈ 36.4 decoded on the decode-limited
  software path; 0 drops steady state), not monitor Hz; static frames are never
  redrawn.
- Hot path measured per-frame in the decode worker: ReadSample (MF software decode)
  ~23–30 ms/f (dominant; inherent to this machine's software stack), frame copy
  **4.5 → 1.5 ms/f** after the recycle pool (resize+zero 4.5 → 0.00 ms/f, memcpy
  ~1.45 ms/f irreducible 14.7 MB copy), push ~0.01 ms/f. No per-frame allocations,
  locks, or syscalls remain.
- Leak-cycle stress (6× play/pause + 4× UI open/close): private memory flat at
  ~423 MB (388 MB paused), handles 1366–1368, threads 37–40, **0 unexpected
  warnings** — no growth.
- Startup: wallpaper visible within ~1–2 s; no library/process scan at launch.

## Known limitations

- **Audio not played** (video-only v1, by design).
- **No MF hardware decoder on this machine** → software decode is the only
  live-exercised path here (HW path implemented, unverifiable end-to-end on this box).
- **Single display** → real hot-plug / multi-monitor / mixed-refresh NOT MEASURED
  (simulated topologies + single-monitor e2e are the substitute).
- **No AV1/VP9/HDR/4K decode rows** (no HW MFT; no HDR/AV1 sample verified).
- **VRAM NOT MEASURED** (no GPU decode path on this machine; textures = 1 dynamic
  upload + per-host swap chain).
- **Disruptive tests declined**: real lock/unlock, system suspend/resume, GPU driver
  reset, power/hot-plug (message routing unit-tested + code-reviewed; injection
  harness for device loss).
- **Soak duration reduced** from 24 h to **4–8 h** per the interview decision (spec
  §3/§9) + aggressive leak cycles; the final soak run is in progress at the time of
  writing and its CSV is read into this report's audit before M14 closes.
- **v2 deferred**: thumbnails, drag & drop, global hotkeys, debug overlay,
  shared-playlist mode, installer, per-monitor playlists.

## Files / components created

```
CMakeLists.txt  CMakePresets.json  package.ps1  LICENSE  README.md
docs/            (plan, spec link, testing/profiling, progress checklist, this report, audit)
shaders/VideoShader.hlsl
src/app/         ApplicationController, ControlWindow, UiContract, app.rc
src/config/      ConfigurationManager (+ JSON parser, utf8)
src/logging/     Logger
src/util/        Clock, utf8, ScaleMath, json
src/gfx/         D3D11DeviceManager, D3D11Renderer, TextureManager
src/wallpaper/   WallpaperHost, WallpaperManager
src/monitors/    MonitorManager
src/video/       VideoMetadata, DecoderManager, VideoPlayer, FrameQueue
src/playback/    FrameScheduler, PlaybackController
src/playlist/    PlaylistManager, PlaylistStore
src/governor/    ResourceGovernor, PausePolicy
src/performance/ WorkloadMonitor, StatsCollector
src/detection/   GameDetector, FullscreenDetector
src/system/      SystemStateMonitor
src/library/     LibraryManager
src/ui/          Win32UI, TrayController, panels/*
tests/           doctest + unit tests
harness/         vw_gfx_harness (dev-only)
```

---

## Doc 3 §101 — 22-point final report

1. **Architecture** — single x64 C++23 exe; layered: app composition root →
   playback (scheduler/player/queue) → video (MF decode) + wallpaper (per-monitor
   D3D11 hosts) → detection/governor. See README *Architecture* for the diagram and
   module table.
2. **Build instructions** — `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`
   + `cmake --build build --config Release`; tests via `ctest --test-dir build -C
   Release`; package via `powershell -File package.ps1`. Details in README.
3. **Supported Windows versions** — Windows 11 (24H2 build 26200 verified).
   Windows 10 21H2+ expected to work but **NOT MEASURED**.
4. **Supported codecs/containers** — H.264 ✅ (verified), HEVC ✅ (HW MFT
   dependent), AV1/VP9 ✅ where the OS/hardware provides an MFT else `NOT MEASURED`;
   containers MP4/MOV verified, MKV/WebM/AVI via MF Source Resolver (any container
   MF can open; validated by metadata, not extension). Audio: none (video-only).
5. **Executable size** — **1,602,048 bytes** (Release x64, stripped of debug info,
   PDB separate).
6. **Installed size** — portable ZIP: **1,054,769 bytes (1.0 MB)**; uncompressed
   **2,366,103 bytes (2.3 MB)** for 6 files (exe 1,602,048 + `msvcp140.dll`
   557,728 + `vcruntime140.dll` 124,544 + `vcruntime140_1.dll` 49,792 + README
   20,633 + LICENSE 11,358). No installer; no bundled codecs/assets/samples.
7. **Active RAM usage** — ~**408–424 MB private** (Release, 1440p60 H.264 software
   decode; dominated by the MF software decode pipeline + 14.7 MB × few RGB32
   frames).
8. **Paused RAM usage** — ~**388 MB private** (decoder stopped, queue empty).
9. **Suspended RAM usage** — ~**124 MB private** (decoder + next-video prep +
   temp GPU resources released; position/path/config kept).
10. **VRAM usage** — **NOT MEASURED — no hardware decode on this machine** (M5).
    Textures = 1 dynamic upload texture + per-host back buffer; GPU-resident NV12
    frames in the HW path would be current frame + 2–3 buffered (bounded).
11. **CPU usage** — playing ~**190%** (software decode, decode-limited), paused
    0–5%, suspended **0.0–0.8%**. No busy waits anywhere (workers block; scheduler =
    waitable timer).
12. **GPU usage** — presentation at source FPS only (36.4 FPS on the 60 FPS clip);
    **decode-engine usage NOT MEASURED** (no HW decode path here). GPU-engine
    utilization counters unavailable in this SDK (gpuUsage=0, never fabricated);
    VRAM + hysteresis is the GPU metric.
13. **Thread count** — ~**37–40** process-wide while playing (flat across stress);
    app-created: 3 `std::thread` workers (decode, library probe, library watch) +
    UI/control thread; the rest are MF/COM/D3D internal pools. Suspended: fewer
    (library probe stopped).
14. **Frame buffer count** — bounded **3** (configurable; `FrameQueue` capacity),
    drop-oldest under pressure, plus 1 dynamic upload texture per monitor in the
    software path. Recycle pool reuses the 14.7 MB buffers (no per-frame alloc).
15. **Decoder count** — **1** active in clone mode (N monitors share it); N in
    independent mode for N distinct videos (M8). This machine: 1 (software).
16. **CPU↔GPU copies** — software path: 1 upload per frame (`Map`/`Unmap` of the
    dynamic texture — inherent to the CPU decode fallback). HW path: **0** in happy
    path (GPU NV12 → shader). No readbacks except the M11 preview grab (on demand).
17. **Game detection behavior** — foreground-change **event** (`SetWinEventHook`);
    allow/deny lists; classified game → governor PAUSED (live-verified: notepad.exe
    on the allow-list paused, closed → resumed). No injection/admin/scanning.
18. **Fullscreen detection behavior** — true/borderless fullscreen classified vs
    maximized (maximized ≠ fullscreen unless configured); pauses on fullscreen,
    resumes on Alt+Tab (live: foreground window classified `window: windowed` /
    fullscreen; maximized editor did NOT pause).
19. **ResourceGovernor behavior** — sole authority; ACTIVE/PAUSED/SUSPENDED with an
    11-bit pause-reason mask (user, game, fullscreen, workload cpu/gpu/mem, lock,
    display-off, battery…); battery policy Continue/Reduce/Pause (default Pause);
    long pause (>5 s) → SUSPENDED releases decoder + GPU resources; threshold pairs
    cross-validated; config revision counter. Full transition table unit-tested
    (doc 3 §95); live cycle ACTIVE→PAUSED→SUSPENDED→ACTIVE verified.
20. **Known limitations** — see the *Known limitations* section above (audio,
    HW-decode-on-this-machine, single display, AV1/HDR rows, VRAM, declined
    disruptive tests, reduced soak, v2 deferrals).
21. **Measured results** — items 5–19 above are measured on the dev machine; the
    soak CSV (`build/release/soak_m13.csv`) is sampled every minute and read at
    completion. All numbers recorded in `docs/06-progress-checklist.md` M13 notes.
22. **Unmeasured results** — HW decode e2e, VRAM, AV1/VP9/HDR/4K rows, real
    multi-monitor/hot-plug/mixed-refresh, lock/suspend/disruptive tests, Windows 10,
    tray-click interactions (need human eyes — code-reviewed; see checklist M11):
    each marked `NOT MEASURED — reason` above and in
    `docs/08-resource-audit.md`.

---

## Guiding principle audit

> "WAKE ONLY WHEN NECESSARY. DECODE ONLY WHAT IS NECESSARY. COPY ONLY WHAT IS
> NECESSARY. RENDER ONLY WHEN NECESSARY. RELEASE EVERYTHING THAT IS NOT NECESSARY."

Verified in code and by measurement: message pump + workers block (zero busy-wait,
zero `Sleep()`); decode is demand-driven with backpressure; the scheduler presents at
source FPS and never redraws static frames; paused → no video work; suspended →
decoder + GPU resources released (measured 0.0–0.8% CPU / 124 MB RAM); hidden
wallpaper stops presenting; monitoring is 1–2 s or event-driven; UI closed = zero UI
cost. Every resource is released on the deterministic shutdown path (M1/M3.17).
