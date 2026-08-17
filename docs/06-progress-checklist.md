# 6. Milestone Progress Checklist (M0–M14)

Live tracker for implementing the wallpaper engine. **Check boxes off as work completes**; keep it in sync with the code at each milestone boundary (spec rule: never advance while the previous milestone is fundamentally broken).

> **Execution contract:** [`../implement-docs-plan-spec.md`](../implement-docs-plan-spec.md) — interview decisions (app name `Video Wallpaper`, behavior defaults, testing constraints), per-milestone acceptance mapping (§9.1), UI panel specifications (§10). Milestone adjustments from the interview are listed under each milestone below where they apply (e.g. M1 C++23, M7 loop semantics, M8 single-display, M13 reduced soak).

## Status summary

| Milestone | Status | Completed | Notes |
|---|---|---|---|
| M0 — Environment & toolchain audit | ✅ | 2026-08-17 | MSVC 14.44.35207 + SDK 10.0.26100.0 + CMake 4.4.2 installed & verified (Debug+Release hello build). See `BUILD_NOTES.md` |
| M1 — Build skeleton | ☐ | — | |
| M2 — D3D11 renderer | ☐ | — | |
| M3 — Wallpaper host | ☐ | — | |
| M4 — MF playback (software first) | ☐ | — | |
| M5 — Hardware decoding + GPU color | ☐ | — | |
| M6 — Frame timing & queue | ☐ | — | |
| M7 — Playlist engine | ☐ | — | |
| M8 — Multi-monitor & multi-GPU | ☐ | — | |
| M9 — Detection & monitoring | ☐ | — | |
| M10 — Resource governor & suspension | ☐ | — | |
| M11 — UI, tray & minimal library | ☐ | — | |
| M12 — Recovery hardening | ☐ | — | |
| M13 — Profiling, optimization & stability | ☐ | — | |
| M14 — Packaging, README, final report | ☐ | — | |

**Current milestone:** _M1 — Build skeleton_

---

## M0 — Environment & toolchain audit ✅ (2026-08-17)

**Objective:** know exactly what is installed before writing code.

- [x] Inspect repository state (`git status`, existing files)
- [x] Detect VS + MSVC (`vswhere`) and Windows SDK version; confirm x64 toolset — **was missing; installed via winget (user-approved)**
- [x] Confirm MF headers/libs (`mfapi.h`, `mfreadwrite.h`, `mf.lib`, `mfplat.lib`, `mfreadwrite.lib`) — all present in SDK 10.0.26100.0
- [x] Confirm D3D11 (`d3d11.h/.lib`) and DXGI (`dxgi.h` in `shared/`, `dxgi.lib`) — present
- [x] Check FFmpeg availability (informational only — not a dependency)
- [x] Record decisions: compiler = MSVC 14.44.35207, generator = `Visual Studio 17 2022` (x64), SDK = 10.0.26100.0, CMake = 4.4.2
- [x] Record findings in `BUILD_NOTES.md`

**Exit criteria:** ✅ toolchain confirmed; trivial "hello CMake" x64 build succeeded in Debug **and** Release (`.m0check` scratch project, removed after verification).

**Notes / findings:** machine = Windows 11 Home (10.0.26200), Ryzen 5 7535HS, 13.8 GB RAM, NVIDIA RTX 3050 Laptop + AMD Radeon iGPU; test videos at `C:\Users\mbk43\Videos\bgcmp\` (incl. 4K) and `Downloads\Video\`. Details in `BUILD_NOTES.md`.

---

## M1 — Build skeleton

**Objective:** project compiles; hidden control window, logging, config, single instance, event loop work.

- [ ] `git init` + initial commit of existing files (txt specs, `docs/`, `BUILD_NOTES.md`, `implement-docs-plan-spec.md`) — spec §8 step 0
- [ ] Move txt specs to `docs/sources/`; update `docs/README.md` source table; write `LICENSE` (**Apache-2.0** — decided 2026-08-17, "full open source")
- [ ] Rename AppData path references `WallpaperEngine` → `VideoWallpaper` in docs/02 §2.9/§2.10 and docs/03 M1 (spec §5; the checklist Logger task is already updated)
- [ ] CMake: x64-only, **C++23** (smoke-test `/std:c++23` on MSVC 14.44; fallback `/std:c++latest`), Debug + Release presets (`/O2`, LTCG Release; debug layer Debug-only)
- [ ] Test infra: vendor `doctest.h` into `tests/` (single fetch, then committed), add `tests/` CMake target + CTest wiring — enables the M1 config/logger unit tests
- [ ] `wWinMain`: `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` + single-instance named mutex (2nd instance signals 1st, exits)
- [ ] Hidden control window (`WS_EX_TOOLWINDOW`) + `GetMessageW` pump (blocks when idle)
- [ ] `Logger`: levels TRACE..FATAL, rotating file sink in `%APPDATA%\VideoWallpaper\logs\` (≤10 MB), Release default INFO — app name per spec §5 (was `WallpaperEngine` in the plan docs; update plan references during M1)
- [ ] `ConfigurationManager`: load / validate / defaults / corrupt-backup / batched writes
- [ ] Deterministic shutdown path (sequence in plan §3.17)
- [ ] Files: `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `README.md` (stub), `LICENSE`, `src/app/*`, `src/logging/*`, `src/config/*`, `src/util/*`, `tests/` (doctest + first unit tests)

**Verify:** Debug+Release x64 build green; second launch exits cleanly; config created on first run; logs rotate; exit leaves no process. **Exit:** ☐

**Notes:**

---

## M2 — Direct3D 11 renderer

**Objective:** device + DXGI adapter/output detection + fullscreen-triangle renderer.

- [ ] `D3D11CreateDevice` feature levels {11_1, 11_0}, BGRA support; debug layer only under `_DEBUG`
- [ ] DXGI factory: adapters + outputs; record `DXGI_ADAPTER_DESC` (vendor/device), output refresh rates
- [ ] Vertex-less fullscreen triangle (SV_VertexID), 1 draw call; sampler + rasterizer state
- [ ] Swap-chain helper (`CreateSwapChainForHwnd`)
- [ ] Device-loss plumbing stub (`DXGI_ERROR_DEVICE_*` → log + schedule recreate; fully wired in M12)
- [ ] Shader compiled at build time (CMake `fxc` custom command)
- [ ] Files: `src/graphics/D3D11DeviceManager*`, `D3D11Renderer*`, `TextureManager*`, `shaders/VideoShader.hlsl`

**Verify:** solid color + UV gradient renders to test window at 60 FPS (Debug); Release has no debug layer; adapter/output log matches reality. **Exit:** ☐

**Notes:**

---

## M3 — Wallpaper host

**Objective:** rendered content appears **behind desktop icons**; per-monitor hosts.

- [ ] `MonitorManager`: `EnumDisplayMonitors` + `GetMonitorInfo`, stable ids, add/remove/change events
- [ ] Runtime desktop discovery: `Progman` → spawn `WorkerW` (0x052C) → find `SHELLDLL_DefView` host → determine wallpaper layer; **log actual hierarchy**
- [ ] Per-monitor `WallpaperHost`: child window in wallpaper layer, bounds = monitor bounds, `WS_EX_NOACTIVATE`, DXGI swap chain
- [ ] Static test texture renders behind icons; no click capture, no focus steal, no taskbar entry
- [ ] Explorer-restart detection stub (low-frequency validity check → rebuild hook; full logic in M12)
- [ ] Files: `src/wallpaper/WallpaperHost*`, `WallpaperManager*`, `src/monitors/MonitorManager*`

**Verify:** wallpaper behind icons; survives manual `explorer.exe` kill/restart with hosts rebuilt; multi-monitor positioning correct. **Exit:** ☐

**Notes:**

---

## M4 — Media Foundation playback (software first)

**Objective:** one video decodes and plays. Software path first (hardware in M5).

- [ ] `MFStartup` + `MFCreateSourceReaderFromURL`; video stream only (**audio deselected — out of scope for v1**)
- [ ] Metadata → `VideoMetadata` (duration, size, FPS, codec, HDR signal, hasAudio)
- [ ] Software decode path: NV12 (else RGB32) output, `ReadSample` loop → `FrameQueue`
- [ ] Decode worker: demand-driven (waits on queue space/events — no busy loop)
- [ ] `WallpaperHost` renders decoded frames (basic sampler) instead of test texture
- [ ] File validation via real media metadata (not extension); graceful failure on corrupt/unsupported
- [ ] Files: `src/video/VideoMetadata.h`, `DecoderManager*`, `VideoPlayer*`, `FrameQueue*`

**Verify:** `.mp4` (H.264) and `.mkv` play; metadata log correct; audio never initialized; corrupt file doesn't crash. **Exit:** ☐

**Notes:**

---

## M5 — Hardware decoding + GPU color conversion

**Objective:** hardware MFT → GPU NV12/P010 surface → shader YUV→RGB; no CPU frame copies in happy path.

- [ ] `MFCreateDXGIDeviceManager` + `ResetDevice`; source reader attributes (`MF_SOURCE_READER_D3D_MANAGER`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`)
- [ ] NV12/P010 output types; `IMFDXGIBuffer::GetResource` → `ID3D11Texture2D`
- [ ] Decoder mode detection — report honest `hardware (vendor)` vs `software` (never fabricate vendor)
- [ ] Shader: NV12 as two SRVs (Y `R8`, UV `R8G8`), BT.709, P010 10-bit, scaling modes Fill/Fit/Stretch/Center (Fill default, crop overflow)
- [ ] Software fallback on hardware failure with diagnostics; no crash
- [ ] GPU frame ownership: `DecodedFrame { ComPtr<ID3D11Texture2D>; timestamp }` via `shared_ptr`
- [ ] Files: `DecoderManager` (HW path), `TextureManager` (shared GPU frames), `VideoShader.hlsl` (full), `D3D11Renderer` (video sampling)

**Verify:** log shows actual decoder; GPUView shows Video Decode engine busy, CPU low; no `Map/Unmap`/CPU copy in NV12 path; P010 renders correctly (if file available); HW-disabled fallback clean. **Exit:** ☐

**Notes:**

---

## M6 — Frame timing, queue & scheduling

**Objective:** smooth source-FPS pacing with tiny bounded buffering.

- [ ] `FrameQueue` bounded (default 3, configurable); producer backpressure; drop-oldest for freshness; `droppedFrames` counter
- [ ] `FrameScheduler`: QPC monotonic clock, waitable-timer deadlines, wake on {deadline, new frame, pause, shutdown, monitor change}; **no busy loop**
- [ ] Source-FPS respect (30 FPS video ≠ 144 decodes/presents on 144 Hz); no redraw of static frames
- [ ] Pause semantics: stop decode, clear queue, cancel timer, preserve position; resume from saved position
- [ ] Stats: decodedFps, presentedFps, droppedFrames, decodeLatencyMs, renderTimeMs → `StatsCollector` (M9)
- [ ] Files: `src/playback/FrameScheduler*`, `PlaybackController*`, `src/video/FrameQueue*` (final)

**Verify:** CPU low/stable on 4K/60 HW path; presentedFps ≈ source fps; drops ≈ 0 steady state; pause → CPU/GPU near-zero. **Exit:** ☐

**Notes:**

---

## M7 — Playlist engine

**Objective:** playlists with loop/shuffle/next/prev + persistence + transition preparation.

- [ ] `PlaylistItem` (path, optional start/end, enabled) + ops (add/remove/move/replace/clear/next/prev/shuffle/setCurrent)
- [ ] Modes: Single / Sequential / Loop playlist / Shuffle (no immediate repeat when >1 item; order persisted)
- [ ] Persistence in AppData; saved on transition/shutdown/meaningful change only (never every second)
- [ ] Next-video preparation: lightweight metadata + source-reader open near end (no full decode, no 2nd full pipeline)
- [ ] Loop same video: reuse decoder/GPU resources; reset position only
- [ ] Broken item → log, mark unavailable, advance; recovery later (M12 hardens)
- [ ] Files: `src/playlist/PlaylistManager*`, `PlaylistStore*`

**Verify:** loop/shuffle/sequential behave per spec §18; 100+ item playlist starts instantly; seamless transitions (measure gap); RAM flat vs playlist size. **Exit:** ☐

**Notes:**

---

## M8 — Multi-monitor & multi-GPU

**Objective:** per-monitor wallpapers, clone mode, hot-plug, mixed refresh, adapter locality. *(Shared-playlist mode = v2.)*

- [ ] Full monitor events → create/destroy/reposition hosts; no restart; no resource leaks on disconnect
- [ ] **Clone:** one decoder + one timeline + one source frame; N GPU renderers
- [ ] **Independent:** N decoders only for N distinct videos; shared device/factory/shaders
- [ ] Mixed refresh rates: per-monitor presentation deadlines (single render worker)
- [ ] Multi-GPU: monitor→adapter association; prefer output-driving adapter; documented per-adapter fallback
- [ ] Portrait/ultrawide/4K scaling (no fixed-resolution assumptions)

**Verify:** 3-monitor rig (or simulated): independent videos; clone in sync + decode-once (decoder-count diagnostics); hot-plug without restart; same video on 4K+1440p+1080p decoded once. **Exit:** ☐

**Notes:**

---

## M9 — Detection & monitoring

**Objective:** cheap CPU/GPU/RAM sampling with hysteresis + debounce; event-driven cached game/fullscreen detection.

**Workload:**

- [ ] CPU: `GetSystemTimes`/PDH `% Processor Time`, 1–2 s sampling
- [ ] GPU: `IDXGIAdapter3::QueryVideoMemoryInfo` (VRAM) + Windows "GPU Engine" utilization counters (fallback documented if unavailable)
- [ ] RAM: `GlobalMemoryStatusEx`
- [ ] Hysteresis/debounce from config (e.g. GPU>90% 3 s ⇒ HIGH_GPU; <70% 5 s ⇒ clear); immediate conditions bypass
- [ ] Cost control: skip sampling in GAME/FULLSCREEN/LOCKED/DISPLAY_OFF unless UI open

**Game / fullscreen:**

- [ ] `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` — no polling
- [ ] `FullscreenDetector`: true fullscreen / borderless vs maximized (maximized ≠ fullscreen unless configured); per-window cache
- [ ] `GameDetector`: foreground process inspection only; allow/deny lists; classification cache + invalidation (exit/foreground change/config change)
- [ ] No injection, no game hooks, no admin

- [ ] Files: `src/performance/WorkloadMonitor*`, `StatsCollector*`, `src/detection/GameDetector*`, `FullscreenDetector*`

**Verify:** hysteresis unit tests pass; stress run fires after ~3 s, clears after ~5 s, no oscillation; fullscreen game pauses, maximized editor doesn't; Alt+Tab no flapping; no repeated process scans. **Exit:** ☐

**Notes:**

---

## M10 — Resource governor & automatic suspension

**Objective:** central state authority; near-zero active work when hidden/paused.

- [ ] `SystemStateMonitor`: `WTSRegisterSessionNotification` (lock/unlock), `WM_POWERBROADCAST` (suspend/resume, `GUID_MONITOR_POWER_ON`), AC/battery via notifications (not polling)
- [ ] `ResourceGovernor`: sole authority over decode/render; pause-reason bitmask; transitions per plan §2.4
- [ ] Battery policy: Continue / Reduce quality / Pause (default Pause), configurable
- [ ] Long pause > `longPauseReleaseSeconds` (default 5 s) ⇒ SUSPENDED: release decoder + next-video prep + temp GPU resources; keep position/path/config
- [ ] Resume: recreate decoder, seek to saved position, restart scheduler; device-loss routes through M12
- [ ] Files: `src/governor/ResourceGovernor*`, `PausePolicy*`, `src/system/SystemStateMonitor*`

**Verify:** governor unit tests per doc 3 §95 transition table; lock screen ⇒ ~0 CPU/GPU + decoder released (handle count); unlock ⇒ resumes. **Exit:** ☐

**Notes:**

---

## M11 — UI, tray & minimal library

**Objective:** usable Win32 UI that never drags down the engine. *(V1: no thumbnails, hotkeys, drag & drop, debug overlay.)*

- [ ] `Win32UI`: main window, tabs, DPI-aware layout; constructed lazily, destroyed on close
- [ ] Panels: Home (wallpaper/video/monitor/state/FPS/decoder/GPU @1–2 Hz), Playlists, Monitors (per-monitor wallpaper + clone/independent + scaling), Performance (toggles/thresholds/delays/modes under "Advanced Performance"), Settings (start with Windows via HKCU Run, minimize to tray, battery mode, logging level)
- [ ] Library (minimal): add file/folder, remove, list with metadata columns, preview; incremental scan + `ReadDirectoryChangesW`; lazy metadata
- [ ] Tray: Resume/Pause/Next/Previous/Current wallpaper/Open app/Settings/Exit; left-click toggles UI; UI resources released on close, engine continues
- [ ] UI never decodes/renders/polls; engine interaction only via `ApplicationController` commands
- [ ] Files: `src/ui/Win32UI*`, `src/ui/panels/*`, `src/ui/TrayController*`, `src/library/LibraryManager*`

**Verify:** all panels functional; repeated UI open/close shows no RAM growth; tray works with UI closed; 10k-file folder handled without repeated rescans. **Exit:** ☐

**Notes:**

---

## M12 — Recovery hardening

**Objective:** survive Explorer restarts, device loss, decoder/file failures, config corruption.

- [ ] Explorer restart: host-invalidity detection → rediscovery → rebuild hosts → re-assign monitors → resume (playlist/config untouched); tested via `explorer.exe` kill/restart
- [ ] Device loss (`REMOVED/RESET/HUNG`): stop render → release resources → `GetDeviceRemovedReason` → recreate device + DXGI manager + textures → restart decoder → resume; controlled retry/backoff, no tight loops
- [ ] Decoder failures: log, mark unavailable, advance; tracked retry attempts (no endless retry of same broken file)
- [ ] File change detection: `ReadDirectoryChangesW` on watched folders; handle delete/move/rename/replace of playing file gracefully
- [ ] Config corruption: `.bak` backup + defaults + continue startup
- [ ] Deterministic shutdown under all failure modes

**Verify:** fault injection — kill explorer, disable GPU (test rig), corrupt/rename playing file, corrupt config — each recovers/degrades gracefully; no hang on exit. **Exit:** ☐

**Notes:**

---

## M13 — Profiling, optimization & stability validation

**Part A — Profiling & optimization:**

- [ ] Baselines for all states (plan `04` §4.4) before optimizing
- [ ] Hot-path audit: decoder, frame queue, scheduler, render, stats (allocations, locks, copies, syscalls, GPU submissions) via WPR/WPA, GPUView, PIX, VS profiler
- [ ] Code-search audit: `while(true)`, `while (running)`, `Sleep(`, `sleep_for`, `new`, `malloc`, `memcpy`, `CopyResource`, `Map`, `Unmap`, `CreateTexture`, `CreateThread`, per-frame allocations — justify/fix each
- [ ] Optimizations measured before/after; **revert if no measurable win**
- [ ] No fake claims: report measured near-zero, never literal zero

**Part B — Reliability & stability:**

- [ ] 24 h run: RAM/VRAM/threads/handles at 0 h/1 h/6 h/12 h/24 h; fix any monotonic growth (leak)
- [ ] Stress matrix: 1080p30/60 H.264, 1440p60 H.264, 4K30/60 H.265, 4K60 AV1*, 1/2/3 monitors, mixed resolutions/refresh, clone + independent, huge playlist, corrupt/missing files
- [ ] Repeated play/pause/resume/next/prev/monitor-change/UI-open-close cycles — no handle/COM/thread growth
- [ ] Game/fullscreen/power/hot-plug scenarios (plan `04` §4.3.3–4.3.5)
- [ ] Fix every defect found; no "known leak" acceptance

**Exit criteria:** resource targets met (plan `04` §4.7) or documented deviations; no leaks; stress matrix green. ☐

**Notes:**

---

## M14 — Packaging, README, final report

- [ ] Portable ZIP via CMake/CPack or script (exe + required DLLs + defaults only; no samples/debug/symbols/test assets)
- [ ] Start-with-Windows optional (HKCU Run); no service, no admin
- [ ] `README.md` per spec §64 (overview, architecture, requirements, build/run, codecs, HW accel, multi-monitor, performance behavior, game detection, config, troubleshooting, limitations, development, testing + how low idle usage is achieved)
- [ ] Final report per spec §65 + doc 3 §101 (22-point list) with **measured** numbers or explicit `NOT MEASURED — reason`
- [ ] Resource-efficiency audit answering doc 2 §102's 20 questions

**Exit criteria:** package builds from clean checkout; README complete; report + audit complete with no invented numbers. ☐

**Notes:**

---

## Cross-cutting gates (apply to every milestone)

- [ ] Debug **and** Release x64 builds green
- [ ] Unit tests green (CTest)
- [ ] No busy-wait / per-frame allocation / per-frame log regressions (code-search audit)
- [ ] Working state recorded (git commit when permitted)
- [ ] `docs/06-progress-checklist.md` summary table + notes updated
