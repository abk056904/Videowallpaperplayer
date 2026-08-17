# 3. Implementation Plan (Milestones M0–M14)

Incremental build plan. **Rule: never advance while the previous milestone is fundamentally broken.** Each milestone ends with:

```text
BUILD → TEST → FIX → VERIFY → (record working state)
```

Milestones map to the staged build orders in all three source specs (doc 1 §61's 14 stages, doc 2's phases, doc 3's 16 phases). This plan merges them into **15 milestones (M0–M14)**, consolidating related work: detection concerns (workload + game/fullscreen) share one milestone, and profiling/optimization + stability validation share another.

---

## 3.0 V1 scope (what this plan builds) vs deferred (v2)

**In scope for v1** — everything the milestones below cover:

- Core engine: MF playback (HW decode + software fallback), D3D11 GPU frame path, frame scheduling, playlists, multi-monitor (clone + independent), resource governor + auto-pause, recovery, tray + minimal Win32 UI, config/logging/stats, portable ZIP packaging.

**Deferred to v2 (explicitly not in these milestones):**

| Item | Why deferred |
|---|---|
| Audio playback | Specs: wallpaper playback does not need audio; never initialized. Only `hasAudio` metadata is recorded. |
| HDR rendering | Spec: never claim untested HDR. V1 **detects** HDR, documents behavior, and uses a safe SDR fallback. |
| Global hotkeys | Spec: "optional but preferred." Skip; tray + UI cover the actions. |
| Debug/performance overlay | Spec: optional, off by default. Skip for v1. |
| Library thumbnails, search polish, drag & drop | Thumbnails must be async/cancellable/bounded — real work. V1 Library = add file/folder, list, metadata columns, remove, preview. Cheap additions later. |
| MSI/Inno installer | V1 ships portable ZIP only (spec's preferred option). |
| Shared-playlist multi-monitor mode | V1 supports Clone + Independent. Shared playlist is a small extension of Clone later. |

**Trimming rationale:** every deferred item is explicitly optional or "only if implemented and tested" in the source specs, and none is needed to satisfy the core acceptance criteria (playback, hardware acceleration, efficiency, stability, multi-monitor independence). Dropping them removes risk from v1 without reducing what the specs require.

---

## 3.1 Milestone overview & dependency graph

```text
M0  Environment & toolchain audit
 │
M1  Build skeleton (CMake, Win32 app, logging, config, control window, single instance)
 │
M2  Direct3D 11 device + renderer (fullscreen triangle, adapter detection)
 │
M3  Wallpaper host (WorkerW discovery, per-monitor host, static test texture)
 │
M4  Media Foundation playback (source reader, metadata, decode, single video, software first)
 │
M5  Hardware decoding (DXGI device manager, hardware MFT, GPU surfaces, shader YUV→RGB)
 │
M6  Frame timing & queue (scheduler, bounded queue, frame dropping, backpressure, source-FPS pacing)
 │
M7  Playlist engine (modes, operations, persistence, next-video preparation)
 │
M8  Multi-monitor (per-monitor playback, clone/independent modes, hot-plug, mixed refresh)
 │
M9  Detection & monitoring (workload sampling + game/fullscreen detection, hysteresis, caching)
 │
M10 Resource governor + automatic suspension (states, battery, lock, display-off, pause release)
 │
M11 UI + tray + minimal library (Home/Playlists/Monitors/Performance/Settings, no thumbnails in v1)
 │
M12 Recovery hardening (Explorer restart, device loss, decoder/file errors, config corruption)
 │
M13 Profiling, optimization & stability validation (hot-path audit, 24 h test, leak tests)
 │
M14 Packaging, README, final report & resource-efficiency audit
```

---

## 3.2 M0 — Environment & toolchain audit

**Objective:** know exactly what is installed before writing code (doc 1 §5, doc 3 §1).

**Tasks:**

- [ ] Inspect repository state (`git status`, existing files — currently only the three spec `.txt` files).
- [ ] Detect toolchain:
  - `cmake --version`; `vswhere` / `"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products *` for VS + MSVC + SDK.
  - Windows SDK version installed; confirm x64 toolset present.
- [ ] Confirm Media Foundation headers/libs (`mfapi.h`, `mfreadwrite.h`, `mf.lib`, `mfplat.lib`, `mfreadwrite.lib`), D3D11 (`d3d11.h`, `d3d11.lib`), DXGI (`dxgi.h`, `dxgi.lib`) available in the SDK.
- [ ] Check whether FFmpeg is already available (informational only — **not** a dependency; system MF preferred).
- [ ] Decide/record: compiler (MSVC), CMake generator, SDK version, architecture (x64).
- [ ] Record findings in `docs/` or a `BUILD_NOTES.md` (what was detected, what will be used).

**Exit criteria:** toolchain confirmed; a trivial "hello CMake" x64 build succeeds in Debug and Release.

---

## 3.3 M1 — Build skeleton

**Objective:** project compiles; hidden control window, logging, config, single instance, event loop work (doc 1 §1/§6, doc 2 §36/§76).

**Files (new):**

```text
CMakeLists.txt, CMakePresets.json, .gitignore, README.md (stub), LICENSE
src/app/main.cpp                 # wWinMain, hidden control window, message pump
src/app/ApplicationController.h/.cpp
src/logging/Logger.h/.cpp
src/config/ConfigurationManager.h/.cpp
src/util/…                       # clock (QPC), waitable timer wrapper, Result<T,E>
src/app/ControlWindow.h/.cpp     # hidden window + notification hooks wiring
```

**Key work:**

- [ ] CMake: `x64` only, C++20, Debug + Release presets (`/O2`, LTCG in Release; debug layer/D3D-debug flags only in Debug; warnings as errors in CI-style builds).
- [ ] `wWinMain`: `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`, single-instance named mutex (`CreateMutexW`) — second instance signals the first (registered window message) and exits cleanly.
- [ ] Hidden control window (class `VideoWallpaperControl`): no taskbar presence (`WS_EX_TOOLWINDOW`), message pump with `GetMessageW` (blocks when idle — no busy loop).
- [ ] `Logger`: levels, rotating file sink in `%APPDATA%\VideoWallpaper\logs\` (≤10 MB), Release default INFO.
- [ ] `ConfigurationManager`: load/validate/defaults/corrupt-backup; atomic save (temp+rename). Debounced write-batching lands in M11 (§3.13).
- [ ] Clean shutdown path (see §3.17) even before all subsystems exist.

**APIs:** `CreateMutexW`, `RegisterClassExW`, `CreateWindowExW`, `GetMessageW`/`DispatchMessageW`, `GetModuleFileNameW`, `SHGetFolderPathW(CSIDL_APPDATA)`, `QueryPerformanceCounter/Frequency`, `CreateWaitableTimerW`.

**Verify:** builds Debug+Release x64; launching twice yields one instance; config file created on first run; logs rotate; exit leaves no process.

---

## 3.4 M2 — Direct3D 11 renderer

**Objective:** device + DXGI adapter/output detection + fullscreen-triangle renderer (doc 1 §11/§21, doc 3 §20).

**Files (new):**

```text
src/graphics/D3D11DeviceManager.h/.cpp   # device, context, factory, adapter, feature level
src/graphics/D3D11Renderer.h/.cpp        # fullscreen triangle, shaders, sampler, rasterizer
src/graphics/TextureManager.h/.cpp       # texture/SRV pool + RAII
shaders/VideoShader.hlsl                 # placeholder: solid color → texture sampling later
```

**Key work:**

- [ ] `D3D11CreateDevice` with feature-level list {11_1, 11_0}, `D3D11_CREATE_DEVICE_BGRA_SUPPORT`; debug layer only under `_DEBUG`.
- [ ] DXGI factory (`CreateDXGIFactory1`/2): enumerate adapters + outputs; record `DXGI_ADAPTER_DESC` (vendor/device names for diagnostics), output refresh rates (`DXGI_OUTPUT_DESC`/`GetDisplayModeList`).
- [ ] Renderer: vertex-less fullscreen triangle (SV_VertexID), 1 draw call; sampler (point/linear), rasterizer state; swap chain helper (`CreateSwapChainForHwnd`) — initially for a test window.
- [ ] Device-loss plumbing: render calls return `DXGI_ERROR_DEVICE_*`; stub handler logs + schedules recreate (fully wired in M12).
- [ ] Compile shader at build time (CMake `fxc` custom command or embed compiled blob; prefer build-time compile → no runtime shader file dependency).

**APIs:** `D3D11CreateDevice`, `ID3D11Device/Context`, `CreateDXGIFactory2`, `IDXGIAdapter/Output`, `CreateSwapChainForHwnd`, `ID3D11DeviceContext::Draw`, `VSSetShader`/`PSSetShader`/`IASetPrimitiveTopology`.

**Verify:** renders a solid color + UV gradient to the test window at 60 FPS in Debug; Release builds without debug layer; adapter/output report printed to log matches reality.

---

## 3.5 M3 — Wallpaper host

**Objective:** video texture appears **behind desktop icons** on a real desktop (doc 1 §7/§36, doc 3 §63).

**Files (new):**

```text
src/wallpaper/WallpaperHost.h/.cpp       # per-monitor host window + swap chain + render hook
src/wallpaper/WallpaperManager.h/.cpp    # desktop discovery, host lifecycle, Explorer-restart watch
src/monitors/MonitorManager.h/.cpp       # enumeration + events (needed here for host placement)
```

**Key work:**

- [ ] `MonitorManager` (M8 will extend): `EnumDisplayMonitors` + `GetMonitorInfo`; stable ids; events.
- [ ] Runtime desktop discovery per §2.6: find `Progman`, spawn `WorkerW` (0x052C), locate `SHELLDLL_DefView` host, determine wallpaper layer. **Log the actual hierarchy found**; don't assume one Windows-version arrangement.
- [ ] `WallpaperHost` per monitor: child window in wallpaper layer, bounds = monitor bounds, `WS_EX_NOACTIVATE`; DXGI swap chain for it.
- [ ] Render a static test texture (checkerboard generated once) behind icons; confirm: behind icons, no click capture, no focus steal, no taskbar entry.
- [ ] Explorer-restart detection stub: low-frequency check (~1 Hz, only while wallpaper exists) that `Progman`/host still valid → teardown/rebuild hook (full logic M12).

**Verify:** on the dev machine, wallpaper shows behind icons and survives `explorer.exe` restart (manual kill/restart test) with hosts rebuilt. Multi-monitor positioning correct.

---

## 3.6 M4 — Media Foundation playback (software first)

**Objective:** one video decodes and plays (doc 1 §9/§10, doc 3 §14). Software path first; hardware in M5.

**Files (new):**

```text
src/video/VideoMetadata.h               # struct + codec enum
src/video/DecoderManager.h/.cpp         # MF source reader, metadata, decode worker
src/video/VideoPlayer.h/.cpp            # playback session glue (metadata→decode→frame)
src/video/FrameQueue.h/.cpp             # bounded queue (used fully in M6)
```

**Key work:**

- [ ] `MFStartup(MF_VERSION)`; `MFCreateSourceReaderFromURL` (URL-encode local path), select video stream only (audio stream deselected — **audio is out of scope for v1**).
- [ ] Metadata: read `MF_MT_FRAME_SIZE`, `MF_MT_FRAME_RATE`, `MF_SD_DURATION`, subtype/codec (`MF_MT_SUBTYPE`), `MF_MT_VIDEO_PRIMARIES`/transfer (HDR signal), audio presence; populate `VideoMetadata`.
- [ ] Software decode path: set output type to NV12 (if supported by decoder) else RGB32 fallback; pull samples via `ReadSample`; push into `FrameQueue` (CPU texture staging for now; replaced by GPU path in M5).
- [ ] Decode worker thread: demand-driven (waits on queue space / events — no busy loop).
- [ ] Wire: `WallpaperHost` renders the decoded frame (scaled by a basic sampler) instead of the test texture.
- [ ] Video file validation: inspect real media metadata, not extension; graceful failure for corrupt/unsupported (log + mark unavailable + advance playlist item — playlist in M7; for now stop cleanly).

**APIs:** `MFStartup/MFShutdown`, `MFCreateSourceReaderFromURL`, `IMFSourceReader` (`SetCurrentMediaType`, `ReadSample`, `GetCurrentMediaType`), `IMFMediaType`, `IMFMediaBuffer`, `IMF2DBuffer`, `MFCreateMediaType`, `MFGetAttributeSize` etc., `PropVariantTo*`.

**Verify:** plays `.mp4` (H.264) and `.mkv`; metadata log shows correct duration/resolution/FPS/codec; audio never initialized; pause/stop works via control window; corrupt file doesn't crash.

---

## 3.7 M5 — Hardware decoding + GPU color conversion

**Objective:** hardware MFT → GPU NV12/P010 surface → shader YUV→RGB; zero CPU frame copies in the happy path (doc 1 §10/§20/§25, doc 3 §15).

**Files (new/changed):**

```text
src/video/DecoderManager (hardware path)      # MFCreateDXGIDeviceManager + hardware MFT
src/graphics/TextureManager (shared GPU frames)
shaders/VideoShader.hlsl                      # NV12→RGB (BT.709) + P010 (10-bit) + scale/crop UV
src/graphics/D3D11Renderer (video sampling)
```

**Key work:**

- [ ] `MFCreateDXGIDeviceManager(&resetToken)`; `IMFdxgiDeviceManager::ResetDevice(d3dDevice, resetToken)`; set on source reader via `MF_SOURCE_READER_D3D_MANAGER` attribute (and `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`).
- [ ] Request hardware-friendly output: NV12 (or P010 for 10-bit) with `MF_MT_DEFAULT_STRIDE`/`MF_MT_INTERLACE_MODE` correctly set; `IMFGetService` on the reader/decoder → `MR_VIDEO_ACCELERATION_SERVICE` to obtain `IMFDXGIBuffer` → `GetResource` → `ID3D11Texture2D`.
- [ ] Decoder mode detection: query the active MFT (`IMFTransform::GetAttributes` / transform-friendly name / `MF_TRANSFORM_ATTRIBUTE_MFT_TRANSFORM_CLSID` → registry lookup or `GetFriendlyName`) to report `hardware (vendor)` vs `software`; never fabricate vendor names.
- [ ] Shader: NV12 texture viewed as two SRVs (Y: `DXGI_FORMAT_R8`, UV: `DXGI_FORMAT_R8G8`); BT.709 matrix; P010 10-bit handling; scaling modes Fill/Fit/Stretch/Center in UV math (Fill default, crop overflow).
- [ ] Software fallback: on hardware init failure or unsupported codec → software path (M4) with `Decoder: Software fallback` diagnostics. No crash.
- [ ] GPU frame ownership: `DecodedFrame { ComPtr<ID3D11Texture2D> texture; LONGLONG timestamp; }` shared via `shared_ptr`; frames released when consumed/cleared.

**APIs:** `MFCreateDXGIDeviceManager`, `IMFDXGIDeviceManager`, `MF_SOURCE_READER_D3D_MANAGER`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`, `IMFDXGIBuffer`, `IMFGetService`, `MR_VIDEO_ACCELERATION_SERVICE`, `ID3D11Texture2D`/`CreateShaderResourceView` (plane views), `CheckVideoDecoderFormat` (adapter capability probe).

**Verify:** log reports actual decoder (e.g. `Decoder: NVIDIA NVDEC / hardware` or software); Task Manager/GPUView shows Video Decode engine busy, CPU low; no `Map/Unmap`/CPU copy in the frame path for NV12 (audit via code search + GPUView); P010 file (if available) renders without banding errors; hardware unavailable (e.g. disable GPU in test) falls back cleanly.

---

## 3.8 M6 — Frame timing, queue & scheduling

**Objective:** smooth source-FPS pacing with tiny bounded buffering (doc 1 §13–15, doc 2 §12/§16, doc 3 §17/§21/§22).

**Files (new/changed):**

```text
src/playback/FrameScheduler.h/.cpp     # waitable-timer pacing, per-monitor presentation
src/video/FrameQueue.h/.cpp            # capacity 2–3, backpressure, drop-oldest policy
src/playback/PlaybackController.h/.cpp # timeline (position, pause, resume, seek, loop hooks)
```

**Key work:**

- [ ] `FrameQueue` finalized: bounded (default 3, configurable), producer blocks when full (or drops oldest for freshness), consumer takes newest ≤ deadline and drops stale; `droppedFrames` counter.
- [ ] `FrameScheduler`: monotonic clock (`QPC`); compute next deadline from source FPS; arm waitable timer; wake on {deadline, new frame, pause, shutdown, monitor change}; **no busy loop**.
- [ ] Source-FPS respect: 30 FPS video never decoded/presented at 144 Hz; static frames not redrawn; loop reuse (M7).
- [ ] Pause semantics: on pause → decoder stops advancing, queue cleared, timer cancelled; position preserved; resume → seek/restart from saved position (M10 wires policy; mechanics here).
- [ ] Stats: decodedFps, presentedFps, droppedFrames, decodeLatencyMs, renderTimeMs fed to `StatsCollector` (M9).

**Verify:** CPU during 4K/60 playback on hardware path is low and stable; presentedFps ≈ source fps (not monitor Hz); dropped frames ≈ 0 in steady state; pausing a video drops CPU/GPU to near-zero (per doc 2's primary benchmark).

---

## 3.9 M7 — Playlist engine

**Objective:** playlists with loop/shuffle/next/prev + persistence + transition preparation (doc 1 §16–18, doc 3 §32–35).

**Files (new):**

```text
src/playlist/PlaylistManager.h/.cpp       # items, modes, ops, persistence, current index
src/playlist/PlaylistStore.h/.cpp         # on-disk playlist format (JSON, D-06)
```

**Key work:**

- [ ] `PlaylistItem` (path, optional start/end time, enabled); add/remove/move/replace/clear/next/previous/shuffle/setCurrent.
- [ ] Modes: Single / Sequential / Loop playlist / Shuffle (avoid immediate repeat when >1 item; shuffle order persisted).
- [ ] Persistence: playlist files + current position saved in AppData; saved on transition/shutdown/meaningful change (not every second).
- [ ] Next-video preparation: near end-of-video (e.g. last ~2 s or after last queued frame), asynchronously open metadata + source reader for next item (lightweight; **no full decode, no second full pipeline**). Transition: swap decoders at boundary; aim minimal visible gap. Keep simultaneous decoders only if profiling justifies it (doc 2 §31).
- [ ] Loop same video: reuse decoder/GPU resources; reset playback position only.
- [ ] Error handling: broken item → log, mark unavailable, advance; allow later recovery (M12 hardens).

**APIs:** mostly std (`std::vector` + stable index, `std::shuffle` with seeded RNG), filesystem ops, config-store JSON.

**Verify:** loop/shuffle/sequential behave per §18 examples; playlist of 100+ entries starts instantly (metadata lazy); transitions between videos are seamless (measure gap); RAM doesn't scale with playlist length.

---

## 3.10 M8 — Multi-monitor & multi-GPU

**Objective:** per-monitor wallpapers, clone mode, hot-plug, mixed refresh rates, adapter locality (doc 1 §8/§37/§38, doc 3 §29–31/§45). *Shared-playlist mode is deferred to v2; v1 = Clone + Independent.*

**Files (new/changed):**

```text
src/monitors/MonitorManager (full)        # events, refresh rates, adapter association
src/wallpaper/WallpaperManager (full)     # per-monitor sessions, modes
src/playback/PlaybackController (multi)   # per-monitor sessions + shared decode sessions
```

**Key work:**

- [ ] Full monitor events → create/destroy/reposition `WallpaperHost`s; no restart; no resource leaks on disconnect (release swap chain + textures).
- [ ] Modes:
  - **Clone:** one decoder + one timeline + one source frame; N renderers scale in GPU per monitor (major RAM/VRAM/CPU win).
  - **Independent:** N decoders only when N distinct videos play; shared device/factory/shaders/infra.
- [ ] Mixed refresh rates: per-monitor presentation time from each monitor's refresh rate; render worker paces each host independently at its own next-deadline (single worker, N timers/events).
- [ ] Multi-GPU: associate each monitor with its adapter (`EnumOutputs` → `GetDisplayModeList`); prefer the adapter actually driving that output for its device (or one device per adapter when required); cross-adapter sharing only if proven necessary; documented fallback = per-adapter device (correctness first).
- [ ] Portrait/ultrawide/4K: shader scaling per aspect; no fixed-resolution assumptions.

**Verify:** 3-monitor rig (or simulated via `EnumDisplayMonitors` in tests): independent videos on each; clone mode shows identical frame synchronized; hot-plug/unplug reconfigures without restart; same video on 4K+1440p+1080p decodes once (verify via decoder-count diagnostics).

---

## 3.11 M9 — Detection & monitoring (workload + game/fullscreen)

**Objective:** cheap CPU/GPU/RAM sampling with hysteresis + debounce, and event-driven cached game/fullscreen detection (doc 1 §23–29, doc 2 §19–27, doc 3 §9–12).

**Files (new):**

```text
src/performance/WorkloadMonitor.h/.cpp    # sampling loop (1–2 s), hysteresis state machine
src/performance/StatsCollector.h/.cpp     # PerformanceStats aggregation for UI + diagnostics
src/detection/GameDetector.h/.cpp         # foreground-window-driven, classification cache
src/detection/FullscreenDetector.h/.cpp   # bounds/styles/process checks
```

**Key work — workload:**

- [ ] CPU: `GetSystemTimes` (kernel+idle delta) or PDH `\Processor(_Total)\% Processor Time`; sample-based, 1–2 s.
- [ ] GPU: `IDXGIAdapter3::QueryVideoMemoryInfo` (dedicated/current usage) for memory; GPU engine utilization via Windows "GPU Engine" counters (`gpuperfcounters.h` / PDH `\GPU Engine(*)\Utilization Percentage` — see D-08/R-03; if unavailable, document and use fallback metrics). Never fabricate a single "GPU %" number when the hardware exposes engine-specific data.
- [ ] RAM: `GlobalMemoryStatusEx`.
- [ ] Hysteresis/debounce: thresholds + durations from config (e.g. GPU>90% 3 s ⇒ HIGH_GPU; GPU<70% 5 s ⇒ clear). Configurable. Immediate conditions (locked/display off/game/fullscreen) bypass this module.
- [ ] Cost control: skip sampling entirely in GAME/FULLSCREEN/LOCKED/DISPLAY_OFF unless UI open; no per-second log spam.

**Key work — game/fullscreen:**

- [ ] Foreground-window change events: `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …)` (non-invasive; no polling).
- [ ] `FullscreenDetector`: on foreground change (and on `WM_DISPLAYCHANGE`), get window rect + monitor rect + styles (`WS_POPUP`/borderless vs `WS_OVERLAPPEDWINDOW` maximized); classify **true fullscreen / borderless fullscreen** vs **maximized** (maximized ≠ fullscreen unless configured). Cache result per window until it changes.
- [ ] `GameDetector`: foreground process (`GetWindowThreadProcessId` → `OpenProcess` → `QueryFullProcessImageNameW`), match against configured allow ("always pause") / deny ("never pause") lists; additional signals optional (fullscreen state, process characteristics); **cache classification** (pid, path, class, timestamp); invalidate on process exit/foreground change/config change; never enumerate all processes.
- [ ] No game injection, no hooks into games, no admin (doc 3 §61–62).

**Verify:** unit tests for hysteresis transitions (see `04` §2); during a CPU/GPU stress run, `HIGH_CPU`/`HIGH_GPU` fires after ~3 s and clears after ~5 s below resume threshold; no oscillation at threshold boundary. Fullscreen game pauses wallpaper; borderless fullscreen pauses; maximized Notepad/editor does **not** pause; Alt+Tab away while game runs → behavior follows policy (configurable, no pause/resume flapping); classification cached (Process Explorer shows no repeated process scans).

---

## 3.12 M10 — Resource governor & automatic suspension

**Objective:** central state authority; near-zero active work when hidden/paused (doc 1 §27–34, doc 2 §24–30, doc 3 §7–8/§40–41/§55).

**Files (new):**

```text
src/governor/ResourceGovernor.h/.cpp      # state machine + pause-reason bitmask + transitions
src/system/SystemStateMonitor.h/.cpp      # lock/unlock, display off/on, power, suspend/resume
src/governor/PausePolicy.h/.cpp           # config-driven policy (pure logic, unit-testable)
```

**Key work:**

- [ ] `SystemStateMonitor`: `WTSRegisterSessionNotification` (`WM_WTSSESSION_CHANGE`: WTS_SESSION_LOCK/UNLOCK), `WM_POWERBROADCAST` (PBT_APMSUSPEND/RESUME, PBT_POWERSETTINGCHANGE via `RegisterPowerSettingNotification` with `GUID_MONITOR_POWER_ON`), `GetSystemPowerStatus` for AC/battery (notified, not polled).
- [ ] `ResourceGovernor`: sole authority over decode/render start-stop. Consumes reasons {User, Game, Fullscreen, HighCPU, HighGPU, HighMemory, Battery, Locked, DisplayOff, MonitorHidden, SystemSuspended} → state transitions per §2.4.
- [ ] Battery policy: Continue / Reduce quality / Pause (default Pause) — configurable.
- [ ] Long-pause release: PAUSED lasting > `longPauseReleaseSeconds` (default 5 s) ⇒ SUSPENDED: release decoder, next-video prep, temporary GPU resources; keep playlist position/path/config.
- [ ] Resume: recreate decoder, seek to saved position, restart scheduler, render; device-loss path routes through M12.
- [ ] Threshold-pair cross-validation (spec §9): pause ≥ resume for cpu/gpu/memory at load and on every `CONFIG_SET`; violations clamped (resume pulled toward pause) + logged.
- [ ] Config revision counter (spec §9): bumped on every accepted config change; governor reacts to live threshold/delay/mode changes without polling `ConfigurationManager`.
- [ ] Home panel reports state and active reasons (M11 UI).

**Verify:** unit tests per doc 3 §95 transition table (pure `PausePolicy` logic): ACTIVE+game→PAUSED; high GPU persists→remain paused until hysteresis clears; PAUSED long→SUSPENDED; SUSPENDED+desktop→ACTIVE; LOCKED/DISPLAY_OFF→SUSPENDED. Manual: lock screen ⇒ CPU/GPU ~0 and decoder released (check handle count); unlock ⇒ resumes.

---

## 3.13 M11 — UI, tray & minimal library

**Objective:** usable Win32 UI that never drags down the engine (doc 1 §39–43, doc 2 §33–35, doc 3 §3/§77). *V1 scope: no thumbnails, no hotkeys, no drag & drop, no debug overlay (all v2).*

**Files (new):**

```text
src/ui/Win32UI.h/.cpp              # main window, tab navigation, DPI-aware layout
src/ui/panels/HomePanel, PlaylistsPanel, MonitorsPanel,
              PerformancePanel, SettingsPanel  (.h/.cpp)
src/ui/TrayController.h/.cpp       # Shell_NotifyIcon, tray menu, left-click toggle
src/library/LibraryManager.h/.cpp  # incremental scan, ReadDirectoryChangesW, metadata cache
```

**Key work:**

- [ ] Home: current wallpaper/video/monitor, playback state, FPS, decoder, GPU (from `StatsCollector`, ~1–2 Hz).
- [ ] Library (minimal v1): add file/folder, remove, list with metadata columns (name, duration, resolution, FPS, codec, HDR, size), preview (play on the current monitor); incremental scan + `ReadDirectoryChangesW`; metadata read lazily. Thumbnails/search/drag & drop deferred to v2.
- [ ] Playlists panel: create/edit/reorder/assign per monitor; Monitors panel: per-monitor wallpaper + mode (clone/independent) + scaling.
- [ ] Performance panel: toggles + thresholds + delays + mode (Performance/Balanced/Quality/Ultra Low Resource) under "Advanced Performance".
- [ ] Settings: start with Windows (HKCU Run key — no admin), minimize to tray, battery mode, logging level.
- [ ] Tray: Resume/Pause/Next/Previous/Current wallpaper/Open app/Settings/Exit; left-click toggles UI; tray must not keep the UI alive (UI destroyed on close, engine continues).
- [ ] UI must not decode/render/poll; all engine interactions via `ApplicationController` commands.
- [ ] Config write-batching (spec §9): debounced dirty-flag save (~1–2 s after last `CONFIG_SET`, plus save on shutdown) — no per-click writes; UI edits survive a crash.
- [ ] Win32 UI constructed lazily; destroyed on close (release controls + library memory).

**Verify:** all panels functional; opening/closing UI repeatedly shows no RAM growth; tray works when UI closed; library handles 10k-file folder without repeated rescans (incremental); no UI-initiated decode/render work.

---

## 3.14 M12 — Recovery hardening

**Objective:** survive Explorer restarts, device loss, decoder/file failures, config corruption (doc 1 §35–36/§52–53, doc 3 §47–48/§90).

**Key work:**

- [ ] Explorer restart: detect host invalidation (Progman/WorkerW recreated or missing) → full rediscovery + host rebuild + monitor re-assignment + resume; playlist/config untouched. Tested by killing/restarting `explorer.exe`.
- [ ] Device loss (`DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG`): stop rendering → release device-dependent resources → `GetDeviceRemovedReason` → recreate D3D device → recreate DXGI manager + decoder binding → recreate textures → restart decoder if needed → resume. No app crash; controlled retry (e.g. 1 s backoff, max N) — no tight loops.
- [ ] Decoder failures: log, mark item unavailable, advance playlist; allow recovery on later attempts; no endless retry of the same broken file (track attempts per item).
- [ ] File change detection: `ReadDirectoryChangesW` on watched library folders (incremental updates); handle delete/move/rename/replace of currently playing file (graceful stop/advance, no crash).
- [ ] Config corruption: backup `.bak`, regenerate defaults, continue startup.
- [ ] Shutdown: deterministic sequence (§3.17) under all failure modes.

**Verify:** manual fault injection (kill explorer, disable GPU via device-manager on test rig, corrupt/rename playing file, corrupt config) → each recovers or degrades gracefully; process never hangs on exit.

---

## 3.15 M13 — Profiling, optimization & stability validation

**Objective:** optimize only what measurement proves; then prove 24 h stability with no leaks (doc 1 §54–55, doc 2 §67/§83–85/§97, doc 3 §80–88).

**Part A — Profiling & optimization:**

- [ ] Establish baselines for all states (see `04` §4) **before** optimizing.
- [ ] Hot-path audit: decoder, frame queue, scheduler, render, stats sampling — measure allocations, locks, copies, syscalls, GPU submissions (WPR/WPA, GPUView, PIX, VS profiler; Task Manager only as a sanity check).
- [ ] Code-search audit: `while(true)`, `while (running)`, `Sleep(`, `sleep_for`, `new`, `malloc`, `memcpy`, `CopyResource`, `Map`, `Unmap`, `CreateTexture`, `CreateThread`, per-frame `std::vector`/string — justify or fix every hit (doc 2 §99).
- [ ] Optimization candidates in priority order: eliminate CPU↔GPU copies; GPU-resident frames; source-FPS pacing; drop-oldest queue policy; reuse textures/state; avoid allocations in steady state; low-power adapter selection (don't wake discrete GPU needlessly); release resources on pause.
- [ ] Measure before/after each change; revert if no measurable win (doc 2 §97).
- [ ] No "fake optimization" claims: report measured near-zero, never literal zero (doc 2 §98, doc 3 §89).

**Part B — Reliability & stability:**

- [ ] 24 h run: RAM/VRAM/threads/handles at 0 h / 1 h / 6 h / 12 h / 24 h; fix any monotonic growth (leak) — not just document it.
- [ ] Stress matrix: 1080p30/60 H.264, 1440p60 H.264, 4K30/60 H.265, 4K60 AV1 (where HW supports), 1/2/3 monitors, mixed resolutions/refresh, clone + independent, huge playlist, corrupt/missing files (see `04` §2.3).
- [ ] Repeated play/pause/resume/next/prev/monitor-change/UI-open-close cycles; assert no handle/COM/thread growth.
- [ ] Game/fullscreen/power/hot-plug scenarios per `04` §4.3.3–4.3.5.
- [ ] Fix every defect found; no "known leak" acceptance.

**Exit criteria:** CPU/GPU/RAM/VRAM in all states meet targets in `04` §4.7 (or documented deviations); no leaks; stress matrix green.

---

## 3.16 M14 — Packaging, README, final report

- [ ] Packaging: portable ZIP (exe + required DLLs + defaults only) via CMake/CPack or script; **no** sample videos, debug binaries, symbols, docs copies, test assets (doc 2 §80–81). Installer deferred to v2.
- [ ] Start-with-Windows optional (HKCU Run), no service, no admin (doc 2 §78).
- [ ] `README.md` per doc 1 §64: overview, architecture, requirements, build/run instructions, supported codecs, HW acceleration, multi-monitor, performance behavior, game detection, configuration, troubleshooting, limitations, development, testing — plus an explicit explanation of how low idle resource usage is achieved.
- [ ] Final report per doc 1 §65 + doc 3 §101 (22-point list): implemented features, build/test results, HW accel status, executable/installed size, RAM/VRAM/CPU/GPU per state, thread/handle counts, frame buffer count, decoder count, CPU↔GPU copies, game/fullscreen/governor behavior, limitations, measured vs NOT MEASURED. **Never invent numbers.**
- [ ] Resource-efficiency audit answering doc 2 §102's 20 questions, with measured answers or explicit `NOT MEASURED — reason`.

---

## 3.17 Deterministic shutdown sequence (used from M1 on)

```text
stop accepting new commands
  ↓
signal workers (decoder, render, monitoring)
  ↓
wake blocked workers (events/condition variables)
  ↓
stop decoder
  ↓
stop rendering
  ↓
release GPU resources (swap chains, textures, device)
  ↓
shutdown Media Foundation (MFShutdown)
  ↓
destroy wallpaper hosts
  ↓
save config/playlist position (batched)
  ↓
shutdown UI/tray
  ↓
exit
```

No hanging process; exit code 0 on clean shutdown; logs flushed before exit.

---

## 3.18 Cross-cutting rules applied in every milestone

- RAII everywhere; `Microsoft::WRL::ComPtr` for COM; explicit ownership & release points (§2.8).
- No global mutable state; no magic numbers (constants in named enums/config); scoped enums; const correctness.
- Error propagation via `std::expected` (MSVC C++20 supports it) or a small `Result<T,E>`; never swallow exceptions silently.
- Security posture from M4 on: video files untrusted; validate paths; no DLL loading from media directories; no admin.
- Release build: no D3D debug layer, no per-frame logging, no debug overlay, no diagnostic allocations.
- After every milestone: build Debug+Release, run tests, fix, record state (git commit when permitted).
