# 6. Milestone Progress Checklist (M0–M14)

Live tracker for implementing the wallpaper engine. **Check boxes off as work completes**; keep it in sync with the code at each milestone boundary (spec rule: never advance while the previous milestone is fundamentally broken).

> **Execution contract:** [`../implement-docs-plan-spec.md`](../implement-docs-plan-spec.md) — interview decisions (app name `Video Wallpaper`, behavior defaults, testing constraints), per-milestone acceptance mapping (§9.1), UI panel specifications (§10). Milestone adjustments from the interview are listed under each milestone below where they apply (e.g. M1 C++23, M7 loop semantics, M8 single-display, M13 reduced soak).

## Status summary

| Milestone | Status | Completed | Notes |
|---|---|---|---|
| M0 — Environment & toolchain audit | ✅ | 2026-08-17 | MSVC 14.44.35207 + SDK 10.0.26100.0 + CMake 4.4.2 installed & verified (Debug+Release hello build). See `BUILD_NOTES.md` |
| M1 — Build skeleton | ✅ | 2026-08-17 | CMake x64 C++23, Debug+Release green, 16/16 unit tests, control window + single instance + config/log verified. See notes below |
| M2 — Direct3D 11 renderer | ✅ | 2026-08-17 | DeviceManager + Renderer + TextureManager, build-time fxc with embedded shaders, verified on both GPUs at ~147 FPS (vsync), feature level 11_1, 51/51 tests. See notes below |
| M3 — Wallpaper host | ✅ | 2026-08-17 | Checkerboard behind desktop icons, per-monitor hosts, Explorer-restart recovery verified live (kill/restart). See notes below |
| M4 — MF playback (software first) | ✅ | 2026-08-17 | Source Reader + RGB32 software decode + FrameQueue + VideoPlayer; app plays the configured clip (pause/resume/stop via registered messages, EOS, corrupt-file grace verified live). See notes below |
| M5 — Hardware decoding + GPU color | ✅ | 2026-08-17 | DXGI manager + NV12 GPU path + YUV shader + honest decoder detection + **runtime probe with clean software fallback** (this machine's MF stack has no hardware MFT — see notes). Verified live. See notes below |
| M6 — Frame timing & queue | ✅ | 2026-08-17 | FrameScheduler + PlaybackController + finalized FrameQueue: source-FPS pacing (presentedFps ≈ decodedFps, not monitor Hz), 0 drops, pause → 0.00 CPU-s/8 s, position preserved across pause (4716 → 4716 ms), message loop waits on {waitable timer, new-frame event} — zero busy-wait. 80/80 tests. See notes below |
| M7 — Playlist engine | ✅ | 2026-08-17 | `PlaylistManager`/`PlaylistStore`, replay-loop, broken-item skip |
| M8 — Multi-monitor & multi-GPU | ✅ | 2026-08-17 | Simulated topologies + per-monitor routing; real multi-monitor NOT MEASURED (single display) |
| M9 — Detection & monitoring | ✅ | 2026-08-17 | WorkloadMonitor (CPU/RAM/VRAM + hysteresis), event-driven game/fullscreen detection (no polling). See notes below |
| M10 — Resource governor & suspension | ✅ | 2026-08-17 | ResourceGovernor + PausePolicy + SystemStateMonitor; full ACTIVE→PAUSED→SUSPENDED→ACTIVE cycle verified live. See notes below |
| M11 — UI, tray & minimal library | ✅ | 2026-08-17 | Win32 UI (6 panels), tray, minimal library, debounced config writes. See notes below |
| M12 — Recovery hardening | ✅ | 2026-08-17 | Device-loss recreate (harness-verified), Explorer-restart recovery + paused-frame rebind, decoder attempt tracking, config .bak (M1). See notes below |
| M13 — Profiling, optimization & stability | ☐ | — | Code-search audit clean; baseline + hot-path measured; frame-buffer pool (resize+zero 4.5→0.00 ms/f); leak-cycle stress green; 4 h soak in progress |
| M14 — Packaging, README, final report | ✅ | 2026-08-17 | `package.ps1` → 1.0 MB portable ZIP verified from clean extraction; version resource fixed (winres.h — ID 1) → FileVersion 1.0.0.0; full README + final report (`docs/07`) + resource audit (`docs/08`). Commit `fc28979`. Soak still running (M13 gate) |
| OPT — Extreme resource optimization (post-M14, spec `extreme-resource-optimization.txt`) | ✅ | 2026-08-18 | Software NV12 end-to-end (CPU conversion → 0, upload −62 %), CB dirty-tracking, plane-SRV cache. Measured: CPU 180.8 → 136.2 % (−58 %/frame), RAM −22 MB, decode 32 → 60 fps, presented 32 → ~57 fps. Audit `docs/09`, report `docs/10`. See notes below |

**Current milestone:** _OPT — Extreme resource optimization_ (complete: audit → implement → benchmark → report; see notes below)

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

- [x] `git init` + initial commit of existing files (txt specs, `docs/`, `BUILD_NOTES.md`, `implement-docs-plan-spec.md`) — spec §8 step 0
- [x] Move txt specs to `docs/sources/`; update `docs/README.md` source table; write `LICENSE` (**Apache-2.0** — decided 2026-08-17, "full open source")
- [x] Rename AppData path references `WallpaperEngine` → `VideoWallpaper` in docs/02 §2.9/§2.10 and docs/03 M1 (spec §5; the checklist Logger task is already updated)
- [x] CMake: x64-only, **C++23** (smoke-test `/std:c++23` on MSVC 14.44; fallback `/std:c++latest`), Debug + Release presets (`/O2`, LTCG Release; debug layer Debug-only)
- [x] Test infra: vendor `doctest.h` into `tests/` (single fetch, then committed), add `tests/` CMake target + CTest wiring — enables the M1 config/logger unit tests
- [x] `wWinMain`: `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` + single-instance named mutex (2nd instance signals 1st, exits)
- [x] Hidden control window (`WS_EX_TOOLWINDOW`) + `GetMessageW` pump (blocks when idle)
- [x] `Logger`: levels TRACE..FATAL, rotating file sink in `%APPDATA%\VideoWallpaper\logs\` (≤10 MB total: 5 MB current + 5 MB previous), Release default INFO — app name per spec §5 (was `WallpaperEngine` in the plan docs; update plan references during M1)
- [x] `ConfigurationManager`: load / validate / defaults / corrupt-backup / atomic save (debounced write-batching deferred to M11)
- [x] Deterministic shutdown path (sequence in plan §3.17)
- [x] Files: `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `README.md` (stub), `LICENSE`, `src/app/*`, `src/logging/*`, `src/config/*`, `src/util/*`, `tests/` (doctest + first unit tests)

**Verify:** Debug+Release x64 build green; second launch exits cleanly; config created on first run; logs rotate; exit leaves no process. **Exit:** ☑

**Notes:**
- Verified 2026-08-17: Debug + Release x64 build green (no warnings, `/std:c++23` — no fallback needed); 16/16 doctest cases pass in both configs (config 5, json 7, logger 4).
- Runtime check: first run wrote `%APPDATA%\VideoWallpaper\config.json` + `logs\current.log`; second instance exited 0 and its focus message was received by the first instance's control window (logged "second instance requested focus").
- Storage format discovery: MSVC `wfstream` converts wchar_t ↔ UTF-8 via the CRT codecvt, so config.json and logs are **UTF-8** (editable in any editor; round-trip covered by tests).
- Toolchain: compiler is MSVC **19.44.35228.0** (cl.exe in VS 2022 Build Tools 17.14.37) with `/std:c++23` confirmed.
- `doctest.h` vendored at **v2.4.11** (fetched once from GitHub, committed for offline builds).
- Config is UTF-8 JSON with sections: `general` / `playback` / `performance` / `battery` / `detection`; corrupt file → `.bak` backup + defaults rewrite (covered by test).
- JSON tests expanded post-commit: 7 → 17 cases (28 total). Edge cases locked: all escapes, `\u` incl. surrogate pairs + `\u0000`, number formats (`-0`, `1e+5`, `1e999`→inf rejected, leading/trailing dots), empty containers/nesting, trailing commas, literal word boundaries (`truex`), control-char `\u` serialization, key escaping round-trip.
- **Config + logger tests deepened: 47 test cases / 2267 assertions total, green in Debug+Release.** Config: invalid-UTF-8 → corrupt recovery, duplicate keys → corrupt recovery (strict JSON), empty object/sections → defaults, enum strings case-sensitive, min-bound clamping, string-array element filtering, full-field round-trip, deterministic repeated saves, save-fails-cleanly (unwritable target). Logger: level-boundary filtering (WARN level), non-ASCII UTF-8 sink round-trip, 4-thread × 500-line concurrency (every line exactly once), repeated rotation keeps current.log bounded.
- **Strictness decision (locked by test): duplicate JSON keys are rejected as a parse error** (were silently first-wins via `std::map::emplace`) — ambiguous config → corrupt-file recovery path instead of silent value pick.
- **UTF-8 encoding bug found + fixed in review**: MSVC `wfstream` write = ANSI codepage (drops non-ANSI chars), read = UTF-8 — asymmetric, so non-ASCII config values were corrupted. Now explicit `wideToUtf8`/`utf8ToWide` (`src/util/utf8.*`) with byte streams; invalid UTF-8 → corrupt-recovery path. Covered by `tests/test_utf8.cpp` + non-ASCII config round-trip.
- **Review refactors applied**: JSON recursion depth cap (512, locked by test); `initPaths` fallback → `%TEMP%` (never writes next to exe); build hygiene — `/WX` on app/harness targets, `/RTC1` in Debug, Release PDBs (`/Zi` + `/DEBUG`).
- Logger: 5 MB/file rotation (`current.log` → `previous.log`), aggregate events only, thread-safe via mutex.

---

## M2 — Direct3D 11 renderer

**Objective:** device + DXGI adapter/output detection + fullscreen-triangle renderer.

- [x] `D3D11CreateDevice` feature levels {11_1, 11_0}, BGRA support; debug layer only under `_DEBUG`
- [x] DXGI factory: adapters + outputs; record `DXGI_ADAPTER_DESC` (vendor/device), output refresh rates
- [x] Vertex-less fullscreen triangle (SV_VertexID), 1 draw call; sampler + rasterizer state
- [x] Swap-chain helper (`CreateSwapChainForHwnd`)
- [x] Device-loss plumbing stub (`DXGI_ERROR_DEVICE_*` → log + schedule recreate; fully wired in M12)
- [x] Shader compiled at build time (CMake `fxc` custom command)
- [x] Files: `src/graphics/D3D11DeviceManager*`, `D3D11Renderer*`, `TextureManager*`, `shaders/VideoShader.hlsl`

**Verify:** solid color + UV gradient renders to test window at 60 FPS (Debug); Release has no debug layer; adapter/output log matches reality. **Exit:** ☑

**Notes:**
- Pre-verification done via `harness/gfx_harness.cpp` (target `vw_gfx_harness`, dev-only): device creation + UV-gradient render verified on **both GPUs** at ~144 FPS (vsync), feature level 11_1, clean `--frames N` exit; enumeration logs 3 adapters (AMD iGPU default, NVIDIA dGPU, Basic Render). See `BUILD_NOTES.md` "M2 pre-verification" for details.
- **Debug layer INSTALLED 2026-08-17** — the DirectX Graphics Tools feature (`Tools.Graphics.DirectX~~~~0.0.1.0` on Win11 24H2+; the older `Graphics.Tools` name no longer exists) was installed elevated and verified: Debug harness reports `debug layer=ON` on both GPUs. Release still skips it. See BUILD_NOTES "Graphics Tools install record".
- **Display corrected: 1920×1080 @ 144 Hz physical** (1536×864 was the 125%-scaled value).
- **M2 implementation verified 2026-08-17**: harness now drives the **real modules** (`D3D11DeviceManager` + `D3D11Renderer`); both GPUs render the UV-gradient at **~147 FPS** (vsync-locked to the 144 Hz display), feature level **0xB100 (11_1)**; Release has no debug layer; enumeration logs match reality. 51/51 tests incl. 4 graphics tests (enumeration sanity, getAdapter bounds, device-loss classification, recreate-stub round-trip).
- **Shaders are build-time compiled + embedded**: `fxc` → `.cso` → generated byte-array headers (`build/<cfg>/generated/VideoShader{Vs,Ps}Data.h`); no runtime file lookups, so the M14 package stays exe + system DLLs only.
- **Gotchas**: flip-model `ResizeBuffers` with the same size before first Present returns `DXGI_ERROR_INVALID_CALL` — renderer tracks its size and skips the call (init builds the RTV directly). `D3D11_FILTER_LINEAR` does not exist in D3D11 (it's a D3D9 name) — the correct value is `D3D11_FILTER_MIN_MAG_MIP_LINEAR`.

---

## M3 — Wallpaper host

**Objective:** rendered content appears **behind desktop icons**; per-monitor hosts.

- [x] `MonitorManager`: `EnumDisplayMonitors` + `GetMonitorInfo`, stable ids, add/remove/change events
- [x] Runtime desktop discovery: `Progman` → spawn `WorkerW` (0x052C) → find `SHELLDLL_DefView` host → determine wallpaper layer; **log actual hierarchy**
- [x] Per-monitor `WallpaperHost`: child window in wallpaper layer, bounds = monitor bounds, `WS_EX_NOACTIVATE`, DXGI swap chain
- [x] Static test texture renders behind icons; no click capture, no focus steal, no taskbar entry
- [x] Explorer-restart detection stub (low-frequency validity check → rebuild hook; full logic in M12)
- [x] Files: `src/wallpaper/WallpaperHost*`, `WallpaperManager*`, `src/monitors/MonitorManager*`

**Verify:** wallpaper behind icons; survives manual `explorer.exe` kill/restart with hosts rebuilt; multi-monitor positioning correct. **Exit:** ☑

**Notes:**
- **Arrangement B (this machine, Win11 24H2+ 26200)**: `SHELLDLL_DefView` stays INSIDE Progman and 0x052C spawns a WorkerW as a **child of Progman** at the bottom of its z-order — that child is the wallpaper layer. The classic "top-level WorkerW with DefView" arrangement does not exist here. Discovery handles both; the actual hierarchy is logged (`arrangement A` vs `arrangement B` vs `no WorkerW`).
- **Child-window DPI virtualization**: a WS_CHILD window parented to another process's window (Explorer, system-DPI-aware at 120 DPI here) is virtualized into the parent's DPI context — the physical rect = requested × 96/parentDpi, regardless of the creating thread's context. Fix: request `physical × parentDpi/96`; the swap chain keeps the physical size (crisp back buffer). Verified: host rect = (0,0)-(1920,1080) exactly.
- **Click-through**: host is **never the WindowFromPoint hit-test target** (grid scan: 0/45 points; the desktop is routed through the shell's XAML input system on this build) + structurally below the icon layer → no `WS_EX_TRANSPARENT` needed. Note: SendInput-driven context-menu testing is impossible in this environment (no menu appears even over Chrome) — flagged for a manual spot-check.
- **Explorer-restart stub verified end-to-end**: kill explorer → detected within 1 s (log), retried while shell dead, automatic rediscovery + host rebuild (new Progman/WorkerW handles) on Explorer return — no app restart.
- **Bonus fixes found during M3 integration**: `D3D11DeviceManager::createDevice` failed with E_INVALIDARG for a null adapter (D3D11CreateDevice requires `D3D_DRIVER_TYPE_HARDWARE` with null adapter, not `UNKNOWN`) — the harness always passed an adapter so M2 missed it; `Logger` now flushes per line (buffered writes were invisible to tailing and lost on force-kill).
- Static test texture (black/white checkerboard, 512×512, 32 px cells) renders behind icons full-screen via the M5-preview textured PS path.
- **Harness `--wallpaper` mode added**: drives the real WallpaperManager from a script (`--adapter N` per-GPU, `--frames N` clean exit, 1 Hz onTick included); verified on both GPUs, Debug + Release, host at (0,0)-(1920,1080) behind icons.

---

## M4 — Media Foundation playback (software first)

**Objective:** one video decodes and plays. Software path first (hardware in M5).

- [x] `MFStartup` + `MFCreateSourceReaderFromURL`; video stream only (**audio deselected — out of scope for v1**; hasAudio metadata recorded)
- [x] Metadata → `VideoMetadata` (duration, size, FPS, codec, HDR signal, hasAudio)
- [x] Software decode path: **RGB32** output via Video Processor MFT (NV12 stays for M5's GPU path), `ReadSample` loop → `FrameQueue`
- [x] Decode worker: demand-driven (blocks on a full queue = backpressure; no busy loop)
- [x] `WallpaperManager::setVideoFrame` uploads decoded frames → `WallpaperHost` renders them instead of the test texture
- [x] File validation via real media metadata (not extension); graceful failure on corrupt/unsupported (verified with a garbage file)
- [x] Files: `src/video/VideoMetadata.*`, `DecoderManager.*`, `VideoPlayer.*`, `FrameQueue.*`; config `playback.videoPath`; app wiring (MFStartup/Shutdown, frame timer, pause/resume/stop registered messages)

**Verify:** `.mp4` (H.264) plays; metadata log correct; audio never initialized; pause/resume/stop work; corrupt file doesn't crash. **Exit:** ☑ (`.mkv` = NOT MEASURED — no sample clip on this machine; the Source Reader path is container-agnostic and H.264/HEVC .mp4 both decode)

**Notes:**
- **Verified live (this machine, Debug + Release)**: configured `playback.videoPath` → "video opened: … 2560×1440 @ 60.00 fps, 30038 ms, codec H.264, 8-bit, audio=yes" (audio detected but never initialized — only the video stream is selected); wallpaper host presents decoded frames; pause at 2133/2566/5100 ms → resume from the **same** position; stop joins the worker cleanly; EOS → "video stream ended — stopping playback" (last frame stays); corrupt file → `cannot open video … MFCreateSourceReaderFromURL failed: 0xC00D36C4` and the app keeps running with the checkerboard.
- **Two real races found + fixed this milestone** (both surfaced only under specific timing — Release vs Debug): (1) `DecoderManager::stop()` nulled `queue_` before joining, so a worker reaching `queue->push` in that window dereferenced null → SIGSEGV. The worker now holds a local queue pointer captured at spawn; stop() joins before nulling. (2) `VideoPlayer::tearDown()` destroyed the FrameQueue **before** `decoder_.close()` joined the worker — destroying the mutex/CV under a worker still inside `push()` → hang (app "Not Responding" on stop). Order fixed: close → join → reset.
- **SDK 26100 header gaps found**: `MF_MT_VIDEO_BIT_DEPTH` and `MF_SD_STREAM_MAJOR_TYPE` do not exist in this SDK (checked headers + docs) — bit depth comes from the subtype family (P010/… = 10-bit), audio detection via the stream descriptor's media-type handler. `MFVideoFormat_AV01` is `MFVideoFormat_AV1`. `MF_SOURCE_READER_MEDIASOURCE` is a stream-index sentinel (0xFFFFFFFF), not a GUID — it goes in the `dwStreamIndex` slot of `GetServiceForStream`.
- **`mfreadwrite.h` include order**: must be preceded by `mfidl.h` (the reader interfaces are declared there) — a missing include made the header parse as garbage (ComPtr cascade).
- **M4 output-type bug**: `negotiateRgb32Output` (Source Reader → RGB32 via Video Processor MFT + explicit `MF_MT_DEFAULT_STRIDE`) was defined but never called in `open()` — the reader yielded **compressed** samples and `copySampleToFrame` memcpy'd past the buffer (SIGSEGV in the real-file test). Now called after metadata extraction.
- **Software decode is slower than real time at 1440p60** (~0.5×): 30 s of content took ~80 s wall time (H.264 software decode + RGB32 conversion via the Video Processor MFT). Expected for M4's CPU path; M5's hardware decode is the fix. M6's scheduler will also stop re-presenting the same frame.
- **Tests**: 7 new cases (subtype mapping, synthetic media types incl. P010-HDR + HLG, FrameQueue capacity/close-unblock/thread hand-off, real-file decode with frame-size checks, corrupt-file grace) → **61/61 cases, 2440 assertions** green in Debug + Release.
- **Probe note**: playback control was verified via a scratch C++ probe posting the registered messages (`VideoWallpaper.Playback{Pause,Resume,Stop}`) to the control window — the same mechanism the M11 UI will use. (PowerShell P/Invoke probes remain unreliable on this box; C++ is the verification instrument of record.)

---

## M5 — Hardware decoding + GPU color conversion

**Objective:** hardware MFT → GPU NV12/P010 surface → shader YUV→RGB; no CPU frame copies in happy path.

- [x] `MFCreateDXGIDeviceManager` + `ResetDevice`; source reader attributes (`MF_SOURCE_READER_D3D_MANAGER`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`)
- [x] NV12/P010 output types; `IMFDXGIBuffer::GetResource` → `ID3D11Texture2D`
- [x] Decoder mode detection — report honest `hardware (vendor)` vs `software` (never fabricate vendor)
- [x] Shader: NV12 as two SRVs (Y `R8`, UV `R8G8`), BT.709, P010 10-bit, scaling modes Fill/Fit/Stretch/Center (Fill default, crop overflow)
- [x] Software fallback on hardware failure with diagnostics; no crash
- [x] GPU frame ownership: `DecodedFrame { ComPtr<ID3D11Texture2D>; timestamp }` via `shared_ptr`
- [x] Files: `DecoderManager` (HW path), `TextureManager` (shared GPU frames), `VideoShader.hlsl` (full), `D3D11Renderer` (video sampling)

**Verify:** log shows actual decoder; GPUView shows Video Decode engine busy, CPU low; no `Map/Unmap`/CPU copy in NV12 path; P010 renders correctly (if file available); HW-disabled fallback clean. **Exit:** ✅ (partially — machine limitation, see notes)

**Notes:**
- **M5 preview (pre-M5, harness)**: `--video <path>` decodes ONE frame via Source Reader → RGB32 → D3D11 texture → textured PS. Verified on 9 clips (H.264 8× + HEVC 1×) on both GPUs, Debug + Release. Real stream resolutions differ from filenames (e.g. "3840X2160" clip is actually 2560×1440; one clip is 1916×1080 odd width — stride handling proven). The decode path is the production Source Reader, but **RGB32 CPU conversion + `Map`/`Unmap` upload is explicitly the throwaway preview** — M5 replaces it with `MF_SOURCE_READER_D3D_MANAGER` GPU surfaces + shader YUV→RGB (no CPU copy).
- **Machine limitation (probed exhaustively, not assumed)**: this machine's Media Foundation stack will not hand out GPU surfaces. Full forensic chain (`probe_dxva.cpp`): a hardware MFT **IS registered** (`AMDhwDecoder`) but is an **async MFT that exposes zero types and rejects the D3D manager** (`MF_E_TRANSFORM_ASYNC_LOCKED`) — never selectable; the **drivers fully work** (`CreateVideoDecoder` = S_OK on both GPUs with real configs); yet the **MS H.264/HEVC decoder MFTs silently allocate system-memory NV12** even when driven directly with the manager, in DXVA-style mode (`PROVIDES_SAMPLES`), with a complete input type (1080p/1440p, H.264 + HEVC, both adapters — identical). Their internal DXVA handoff refuses on this box for a reason not visible through the public API. (Earlier "0 hardware MFTs" claim was an input-type-filter artifact — corrected.)
- **M5 design response — runtime probe + clean fallback**: `open()` now tries the hardware path first; after NV12 negotiation it reads ONE sample and requires a DXGI buffer. If the decoder hands back system memory, the whole reader is discarded and open() rebuilds it via the proven M4 RGB32 path (`MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` + VP MFT) — re-negotiating RGB32 on the NV12-committed reader fails with `MF_E_INVALIDTYPE` (probed), hence the rebuild. On this machine the app logs `hardware decode unavailable (hardware probe: decoder produced system-memory samples (no hardware MFT active)); retrying with the software RGB32 path` and plays normally.
- **The GPU path is implemented to the documented pattern and unit-tested for its machinery** (two-SRV planar views, `DecodedFrame::texture` ownership, shader), but **cannot be verified end-to-end on this machine** — no hardware MFT exists here. The hardware-path test asserts EITHER GPU surfaces (with the two-SRV legality check + decoder name) OR a clean software fallback with frames flowing — it passes via the fallback on this box.
- **`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` added to the device** (M5): MF's hardware path requires it; without it the hardware open SIGSEGV'd inside mfreadwrite. Applied to `D3D11DeviceManager` and the test's device.
- **SDK 26100 quirks hit this milestone**: `MR_VIDEO_ACCELERATION_SERVICE` is `DEFINE_GUID`'d in `evr.h` but exported by no import lib → `initguid.h` materializes it; `MF_TRANSFORM_ATTRIBUTE_MFT_TRANSFORM_CLSID` is named `MFT_TRANSFORM_CLSID_Attribute` here; `GetServiceForStream(MR_VIDEO_ACCELERATION_SERVICE)` hard-crashes inside mfreadwrite on this SDK — the documented `IMFGetService` route (QI the reader) is used instead.
- **Tests**: hardware-path test added (real device, `VIDEO_SUPPORT`, open + first frame + fallback acceptance) → **62/62 cases, 2445 assertions** green in Debug + Release.
- **Live verification (Debug + Release)**: app plays the configured clip with the fallback path (texture uploads advancing); pause at 2983 ms → resume from the **same** position → stop cleanly; `decoder: software (RGB32 output)` logged honestly; frames flow to the wallpaper.
- **M5 review fixes (2026-08-17, before M6)**: the scaling feature was broken and unexercised — Fit/Fill had **inverted aspect ratios** (Fill = the default — was cropping/zooming wrong), Center used aspect ratios instead of pixel dims, and the software path forced identity so no scaling ever applied on this machine. Fixed via pure `ScaleMath.h` (`computeScaleOffset`, 7 unit tests, pixel-verified both orientations) + the software path now honors scaling (live-verified: harness `--scaling center` shows the 1:1 crop). Letterbox margins now render black via a BORDER sampler (was: clamped video-edge smear). YUV shader now rescales **limited → full range** (decoders output 16–235; the matrix assumed full — would have washed out blacks on any working GPU path). Tests → **69/69, 2493 assertions**, Debug + Release. Deferred to M13: per-frame SRV creation in `bindGpuFrame`.

---

## M6 — Frame timing, queue & scheduling

**Objective:** smooth source-FPS pacing with tiny bounded buffering.

- [x] `FrameQueue` bounded (default 3, configurable); producer backpressure; drop-oldest for freshness; `droppedFrames` counter
- [x] `FrameScheduler`: QPC monotonic clock, waitable-timer deadlines, wake on {deadline, new frame, pause, shutdown, monitor change}; **no busy loop**
- [x] Source-FPS respect (30 FPS video ≠ 144 decodes/presents on 144 Hz); no redraw of static frames
- [x] Pause semantics: stop decode, clear queue, cancel timer, preserve position; resume from saved position
- [x] Stats: decodedFps, presentedFps, droppedFrames, decodeLatencyMs, renderTimeMs → `StatsCollector` (M9)
- [x] Files: `src/playback/FrameScheduler*`, `PlaybackController*`, `src/video/FrameQueue*` (final)

**Verify:** CPU low/stable on 4K/60 HW path; presentedFps ≈ source fps; drops ≈ 0 steady state; pause → CPU/GPU near-zero. **Exit:** ✅ (see notes — SW path on this machine)

**Notes:**
- **Pacing verified live (software path)**: `decoded 36.4 fps ≈ presented 36.4 fps` on the 60 fps 1440p clip — decode-limited (SW ≈ 0.5× real time), **not** 144 Hz; 0 drops steady state; pause → **0.00 CPU-s/8 s** (thread sample); `paused at 4716 ms` → `started (position 4716 ms)` (position preserved — the controller keeps the player position in sync via `setPosition`); EOS → clean stop + session summary. The HW path's pacing is the same code (verified via unit tests; end-to-end HW NOT MEASURED on this machine).
- **Design decision**: rendering stays on the UI thread — the message loop blocks on the **waitable timer + new-frame event + messages** (`MsgWaitForMultipleObjects`), satisfying the zero-busy-wait policy without a separate render thread. Revisit at M8 (renderer thread-safety guard) when multi-session presentation lands.
- **FrameScheduler cadence bug caught by tests**: advancing from the previous *deadline* (not the presented frame's timestamp) doubled the first interval after an early first present — `advanceAfterPresent(now, frameTs)` derives the next deadline from the frame's media timestamp through the anchor.
- **Tests**: 7 scheduler + 4 FrameQueue (popNewestUpTo staleness/event/dropped/thread-safety) + 1 PlaybackController real-clip integration → **80/80, 2652 assertions**, both configs, 0 warnings.
- **Stats**: collected per-second (decodedFps/presentedFps/droppedFrames/decodeLatencyMs/renderTimeMs), DEBUG-logged every 5 s, INFO summary at pause/stop; **StatsCollector wiring pulled forward from M9 (2026-08-17)** — `src/performance/StatsCollector` aggregates the playback stats into the spec's `TelemetrySnapshot` (`src/app/UiContract.h`, namespace `vw::ui`; workload fields 0 until M9's `WorkloadMonitor`), fed ~1 Hz via `PlaybackController::setStatsObserver` + per-monitor detail from the wallpaper layer; DEBUG `telemetry:` line live-verified. **88/88 tests** (7 new).
- **M2 review note (carried): `D3D11Renderer` is single-threaded today** — if M8 moves rendering to a worker thread, guard with a mutex or ownership transfer between the UI and render threads.
- **M6 review fixes (2026-08-17, before M7)**: (1) `PlaybackController::newFrameEvent()` made null-safe (paused state has no queue — the app loop also skips the wait handles entirely if either is null, avoiding a `WAIT_FAILED` busy-spin); (2) **first-frame PTS anchor** — files with a nonzero initial PTS (edit lists, trimmed starts) no longer show the placeholder for the PTS offset: `onWake()` re-anchors the timeline to the first frame's actual PTS via the new `FrameQueue::peekTimestamp()` (`anchorPending_`); (3) `peekTimestamp()` covered by a dedicated unit test. **81/81 tests, 2653 assertions**, Debug + Release, live smoke clean.
- **RENDER BUG FIX (2026-08-17, user report "not fit screen and upside down")**: the video was rendered **vertically flipped** — `VSMain` maps screen-top to `i.uv.y=1` while D3D11 textures have v=0 at the top row, so the video's bottom showed at the top of the screen. Hidden until now: the checkerboard is even-cell-symmetric and the gradient is ambiguous. Fixed in the shader (`PSMainTexture` + `PSMainYuv`): `texUv = (uv.x*sx+ox, 1-(uv.y*sy+oy))`. Verified pixel-exact with a readback probe (render real frame → read back → compare to CPU under both orientations): MAE upright 14 vs flipped 172 across identity-Fill, Fill-crop, and Center-1:1; the same probe proved the scaling math fills the window edge-to-edge (the "not fit" impression was the flip's artifact). Live desktop sample: varied video at all edges, no bars. **81/81 tests**, Debug + Release, 0 warnings. Scratch probes cleaned up; details in BUILD_NOTES.

---

## M7 — Playlist engine

**Objective:** playlists with loop/shuffle/next/prev + persistence + transition preparation.

- [x] `PlaylistItem` (path, optional start/end, enabled) + ops (add/remove/move/replace/clear/next/prev/shuffle/setCurrent)
- [x] Modes: Single / Sequential / Loop playlist / Shuffle (no immediate repeat when >1 item; order persisted)
- [x] Persistence in AppData; saved on transition/shutdown/meaningful change only (never every second)
- [x] Next-video preparation: lightweight metadata + source-reader open near end (no full decode, no 2nd full pipeline)
- [x] Loop same video: reuse decoder/GPU resources; reset position only
- [x] Broken item → log, mark unavailable, advance; recovery later (M12 hardens)
- [x] Files: `src/playlist/PlaylistManager*`, `PlaylistStore*`

**Verify:** loop/shuffle/sequential behave per spec §18; 100+ item playlist starts instantly; seamless transitions (measure gap); RAM flat vs playlist size. **Exit:** ✅

**Notes:**

- **2026-08-17 (M7 complete)**: `src/playlist/PlaylistManager.{h,cpp}` + `PlaylistStore.{h,cpp}`; `VideoPlayer::replay()` + `PlaybackController::replay()` (same-item loop reuses reader/decoder/GPU — no reopen, no hardware re-probe); `DecoderManager::start()` always seeks (initial 0 / resume position / replay 0). **26 new tests** (ops, modes, shuffle permutation + no-immediate-repeat + regenerate-on-wrap + no-loop stop, persistence round-trip, corrupt recovery) → **109/109 tests** (33141 Debug / 341611 Release assertions), both configs, 0 warnings.
- **2026-08-17 (M7 review fixes, before M8)**: (1) **crash fix** — `shuffledPermutation(n=0)` underflowed `i` to `SIZE_MAX` and dereferenced `order[i]` out of bounds (empty playlist + Shuffle mode, reachable via `mode=shuffle` with no videoPath); (2) `nextIndex()` stale-order branch now returns the first **playable** item; (3) `current: kNoIndex` serialized as `-1` (the size_t(-1)→double→int64 round-trip was an out-of-range cast, UB); (4) `replace()` clears stale cached metadata. **113/113 tests** (30558 Debug / 351045 Release assertions), both configs, live smoke clean.
- **2026-08-17 (RENDER BUG, user report "video is being cropped")**: the wallpaper **host window was physically 2400×1350 on a 1920×1080 screen** (25% oversized) — the screen showed only the top-left corner, so the video looked zoomed/cropped. `WallpaperHost::scaleToParentDpi()` scaled physical bounds by `parentDpi/96`; both the app (PER_MONITOR_AWARE_V2) and Explorer's WorkerW are DPI-aware, so child rects are already physical — the factor (1.25 at 125%) was wrong. Fixed: pass bounds through unchanged; verified with a DPI-aware probe (host 2400×1350 → **1920×1080**) + pixel samples (16:9 = zero crop; 21:9 = even side crop, no bars). **113/113 tests**, both configs, 0 warnings. Earlier probes were DPI-unaware (silently virtualized rects) — lesson recorded in BUILD_NOTES.
- **Live-verified**: multi-item loop cycle 0→1→2→wrap (transitions ~80–150 ms open/start, ~50–60 ms session gap); metadata cache persisted to `playlist.json` (instant start on later runs); same-item loop cycles continuously via the replay path; broken item skipped permanently (marked unavailable).
- **Real bug caught live**: `VideoPlayer::replay()` hung at EOS — it reset the `FrameQueue` BEFORE `decoder_.stop()`, and `DecoderManager::stop()` closes that same (now-destroyed) queue → use-after-free hang on the app thread. Fixed by stopping the decoder first (same order as `pause()`). The multi-item path never exercised replay, which is why only the live same-item loop exposed it.

---

## M8 — Multi-monitor & multi-GPU

**Objective:** per-monitor wallpapers, clone mode, hot-plug, mixed refresh, adapter locality. *(Shared-playlist mode = v2.)*

- [x] Full monitor events → create/destroy/reposition hosts; no restart; no resource leaks on disconnect
- [x] **Clone:** one decoder + one timeline + one source frame; N GPU renderers
- [x] **Independent:** N decoders only for N distinct videos; shared device/factory/shaders
- [x] Mixed refresh rates: per-monitor presentation deadlines (single render worker)
- [x] Multi-GPU: monitor→adapter association; prefer output-driving adapter; documented per-adapter fallback
- [x] Portrait/ultrawide/4K scaling (no fixed-resolution assumptions)

**Verify:** 3-monitor rig (or simulated): independent videos; clone in sync + decode-once (decoder-count diagnostics); hot-plug without restart; same video on 4K+1440p+1080p decoded once. **Exit:** ✅ (simulated topologies + single-monitor e2e; real 3-monitor rig **NOT MEASURED** — single display)

**Notes:**
- **2026-08-17 (M8 complete)**: `MonitorManager` gained a **pure, windowing-free diff** (`diffMonitorSets`) + a `setSnapshotForTest()` hook so **simulated topologies are unit-testable** (the real hot-plug substitute on this single-display machine), including work-area change detection; **adapter association** (`associateAdapters`, monitor→DXGI-output match, fills `adapterIndex`/`adapterLuid`) with a hybrid-GPU test layout + unmatched fallback. `WallpaperManager` gained **per-monitor frame routing** (`setVideoFrameFor` + `bindGpuFrameFor`: per-monitor upload texture, dropped on host removal) — Independent mode plumbing; `setVideoFrame` stays the Clone broadcast. Config gained `wallpaper.mode` (independent default per spec §125, clone; persisted); the app routes frames per mode with a **decoder-count diagnostic** (clone = decode-once). **8 new tests → 120/120** (31612 Debug / 308342 Release), both configs, 0 warnings; live single-display e2e in both modes. **NOT MEASURED**: real hot-plug, N-monitor fan-out, mixed-refresh pacing, multi-GPU decode locality (recorded for the M14 report).
- **M5 forensics carry-over**: adapter selection (per-adapter fallback) is implemented and tested for *placement*, but it will **not unlock GPU decode on this machine** — both adapters refused the MS decoder's DXVA handoff identically (probe_dxva). Adapter locality still matters for presentation (hybrid-GPU laptop) and for machines where MF hardware decode works.
- **2026-08-17 (M8 review, 2 passes)**: (1) `associateAdapters` **first-match bug** — broke only the inner (output) loop, so a later adapter's overlapping output could overwrite a correct association; fixed with a `matched` flag. (2) **Monitor-event publish order** (survived since M3, amplified by M8): `refresh()` fired events before publishing the snapshot and `onDisplayChange` updated `monitors_` after — a hot-plugged monitor was invisible to its own `onAdded`/`onChanged` handler (host never created / stale bounds). Fixed via `refreshWithSnapshot(snapshot)` (enumerate → publish → diff; `refresh()` delegates). **2 regression tests → 124/124**, both configs, 0 warnings.
- **2026-08-17 (post-M8, user: "make the crop and scale rule more universal")**: the crop/scale rule was already universal against the *window* aspect (never hardcoded 16:9) — the gap was **sample aspect ratio**: anamorphic videos (non-square pixels) were scaled by their raw pixel aspect and distorted. `VideoMetadata` now reads `MF_MT_PIXEL_ASPECT_RATIO` (→ `sarNum`/`sarDen`, default 1:1) and exposes `displayAspect`; `DecodedFrame` carries it per frame; the renderer's video setters take the **display aspect** (`videoAspectFor` fallback); `VideoPlayer` logs `SAR n:d, display aspect x.xxxx`. **3 new tests → 122/122**, both configs, 0 warnings; the anamorphic Fill test proves the old code cropped the wrong axis. Live: Eula `SAR 1:1, display aspect 1.7778` (identity — behavior unchanged for all square-pixel clips).

---

## M9 — Detection & monitoring

**Objective:** cheap CPU/GPU/RAM sampling with hysteresis + debounce; event-driven cached game/fullscreen detection.

**Workload:**

- [x] CPU: `GetSystemTimes`/PDH `% Processor Time`, 1–2 s sampling
- [x] GPU: `IDXGIAdapter3::QueryVideoMemoryInfo` (VRAM) + Windows "GPU Engine" utilization counters (fallback documented if unavailable)
- [x] RAM: `GlobalMemoryStatusEx`
- [x] Hysteresis/debounce from config (e.g. GPU>90% 3 s ⇒ HIGH_GPU; <70% 5 s ⇒ clear); immediate conditions bypass
- [x] Cost control: skip sampling in GAME/FULLSCREEN/LOCKED/DISPLAY_OFF unless UI open (M10 governor wires the skip; the monitor is a 2 s timer, Debug-only log)

**Game / fullscreen:**

- [x] `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` — no polling
- [x] `FullscreenDetector`: true fullscreen / borderless vs maximized (maximized ≠ fullscreen unless configured); per-window cache
- [x] `GameDetector`: foreground process inspection only; allow/deny lists; classification cache + invalidation (exit/foreground change/config change)
- [x] No injection, no game hooks, no admin

- [x] Files: `src/performance/WorkloadMonitor*`, `StatsCollector*`, `src/detection/GameDetector*`, `FullscreenDetector*`

**Verify:** hysteresis unit tests pass; stress run fires after ~3 s, clears after ~5 s, no oscillation; fullscreen game pauses, maximized editor doesn't; Alt+Tab no flapping; no repeated process scans. **Exit:** ✅ (hysteresis transition table + classification + caching unit-tested; live: workload sampling + foreground classification verified. Pause/resume actions land in M10's ResourceGovernor — this milestone produces the signals, not the actions.)

**Notes:**
- **2026-08-17 (M9 complete)**: `WorkloadMonitor` (`GetSystemTimes` CPU delta, `GlobalMemoryStatusEx` RAM, `IDXGIAdapter3::QueryVideoMemoryInfo` VRAM) with a **pure `HysteresisEngine`** (config thresholds/delays; latch after pause-delay, clear after resume-delay, no boundary oscillation). GPU-engine utilization counters are **unavailable in this SDK** (`gpuperfcounters.h` absent; `IDXGIAdapter3` lives in `dxgi1_4.h`) — gpuUsage stays 0 (never fabricated), VRAM + hysteresis is the GPU metric. `FullscreenDetector` = pure classification (true/borderless fullscreen vs maximized, maximized ≠ fullscreen); `GameDetector` = foreground-pid classification vs allow/deny lists with a **same-pid cache + liveness check** (no repeated process scans; a reused pid never serves a stale classification). App wiring: 2 s workload timer + `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` (out-of-context) → `StatsCollector` workload fields; the foreground window is ALSO classified for fullscreen (`fullscreenState_`, re-checked on `WM_DISPLAYCHANGE`). **16 new tests → 140/140**, both configs, 0 warnings. Live: workload lines (cpu 0→58%, ram 88%, gpu mem 112/8386 MB), foreground pid/path + `window: windowed` classification, notepad.exe → `(game [allow])` via the config allow-list — verified end-to-end. **Pause/resume ACTIONS are M10** (ResourceGovernor consumes `WorkloadState` + game/fullscreen state).

---

## M10 — Resource governor & automatic suspension

**Objective:** central state authority; near-zero active work when hidden/paused.

- [x] `SystemStateMonitor`: `WTSRegisterSessionNotification` (lock/unlock), `WM_POWERBROADCAST` (suspend/resume, `GUID_MONITOR_POWER_ON`), AC/battery via notifications (not polling)
- [x] `ResourceGovernor`: sole authority over decode/render; pause-reason bitmask; transitions per plan §2.4
- [x] Battery policy: Continue / Reduce quality / Pause (default Pause), configurable
- [x] Long pause > `longPauseReleaseSeconds` (default 5 s) ⇒ SUSPENDED: release decoder + next-video prep + temp GPU resources; keep position/path/config
- [x] Resume: recreate decoder, seek to saved position, restart scheduler; device-loss routes through M12
- [x] **Threshold-pair cross-validation** (spec §9): pause ≥ resume for cpu/gpu/memory at load and on every `CONFIG_SET`; violations clamped (resume pulled toward pause) + logged
- [x] **Config revision counter** (spec §9): bumped on every accepted config change so the governor reacts to live threshold/delay/mode changes without polling `ConfigurationManager`
- [x] Files: `src/governor/ResourceGovernor*`, `PausePolicy*`, `src/system/SystemStateMonitor*`

**Verify:** governor unit tests per doc 3 §95 transition table; lock screen ⇒ ~0 CPU/GPU + decoder released (handle count); unlock ⇒ resumes. **Exit:** ✅ (transition table + battery + message routing unit-tested; **live**: full ACTIVE→PAUSED→SUSPENDED→ACTIVE cycle verified via the allow-list; lock/unlock + suspend/resume NOT MEASURED — session events are code-reviewed + message-routing tested; battery = user-assisted)

**Notes:**
- **2026-08-17 (M10 complete)**: `ResourceGovernor` (sole authority; ACTIVE/PAUSED/SUSPENDED + 11-bit reason mask, injectable clock + transition observer) + pure `PausePolicy` (reason-agnostic: any bit pauses, zero resumes; long-pause → SUSPENDED releases the decoder; SUSPENDED→ACTIVE reopens the current playlist item via a resume handler). `SystemStateMonitor` (WTS lock/unlock, PBT suspend/resume, `GUID_MONITOR_POWER_ON` display-off, battery via injectable `GetSystemPowerStatus` query — translated to reason bits by `translate()`). App wiring: manual pause/resume/stop route through the governor (`User` reason); game/fullscreen/workload reasons fed by `feedDetectionReasons()` on every foreground/display/workload change; 1 Hz tick drives the long-pause release. Config: **threshold-pair cross-validation** (pause ≥ resume, resume clamped up, logged) at load + `validateThresholdPairs` for CONFIG_SET, and a **revision counter** (`markConfigChanged`/`revision()`). **8 new tests → 148/148**, both configs, 0 warnings. **Live e2e**: notepad.exe in allow-list → `governor: ACTIVE -> PAUSED (reasons: game)` → 5 s later `PAUSED -> SUSPENDED (released decoder)` → notepad closed → `SUSPENDED -> ACTIVE (resume)`. **NOT MEASURED**: real lock/unlock + system suspend (declined — disruptive); message routing unit-tested, code-reviewed.
- **2026-08-17 (M10 review — commit `4efe8c4`)**: two real fixes. **(1) Stop→resume within the release window failed**: the app's stop handler feeds the `User` reason (governor → PAUSED, decoder kept) **and then** calls `playback_->stop()` directly, dropping the session — so a resume within `longPauseReleaseSeconds` hit `resume()` on a stopped session (`unexpected("not paused (stopped)")`): governor reported ACTIVE while nothing played. Fixed in `transitionTo(Active)`: any `!playback_.isOpen()` (user stop or SUSPENDED) routes through the reopen resume handler; plain resume only for a live paused session. Regression test: `governor: stop-then-resume within the release window reopens`. **(2) The pure `PausePolicy::nextState` transition table was dead code** — the governor re-implemented the transitions in `setReasonsWithTransition`/`onTick` (two sources of truth). Now the governor feeds its mask + elapsed-pause clock to `policy_.nextState()` and maps the returned state to playback actions; dead `wantsActive` removed. Also fixed a **pre-existing flaky playlist test** (M7): the shuffle-wrap test detected "order regenerated" by comparing permutations within 6 walk steps — with 3 items a fresh cycle randomly collides with the old one (~1/16) → intermittent `REQUIRE(0 == 3)` (observed once). Now deterministic via a new `shuffleGeneration()` counter (bumped on every `regenerateShuffle()`). **149/149**, both configs, 0 warnings (5 consecutive runs). Live smoke: boots + plays.

---

## M11 — UI, tray & minimal library

**Objective:** usable Win32 UI that never drags down the engine. *(V1: no thumbnails, hotkeys, drag & drop, debug overlay.)*

- [x] `Win32UI`: main window, tabs, DPI-aware layout; constructed lazily, destroyed on close
- [x] Panels: Home (wallpaper/video/monitor/state/FPS/decoder/GPU @1–2 Hz), Playlists, Monitors (per-monitor wallpaper + clone/independent + scaling), Performance (toggles/thresholds/delays/modes under "Advanced Performance"), Settings (start with Windows via HKCU Run, minimize to tray, battery mode, logging level)
- [x] Library (minimal): add file/folder, remove, list with metadata columns, preview; incremental scan + `ReadDirectoryChangesW`; lazy metadata
- [x] Tray: Resume/Pause/Next/Previous/Current wallpaper/Open app/Settings/Exit; left-click toggles UI; UI resources released on close, engine continues
- [x] UI never decodes/renders/polls; engine interaction only via `ApplicationController` commands
- [x] **Config write-batching** (spec §9): debounced dirty-flag save (~1–2 s after last `CONFIG_SET`, plus save on shutdown) — no per-click writes, UI edits survive a crash
- [x] Files: `src/ui/Win32UI*`, `src/ui/panels/*`, `src/ui/TrayController*`, `src/library/LibraryManager*`

**Verify:** all panels functional; repeated UI open/close shows no RAM growth; tray works with UI closed; 10k-file folder handled without repeated rescans. **Exit:** ✅ (162/162 tests both configs; live open/close leak cycle; tray + left-click toggle code-reviewed — see notes)

**Notes:**
- **2026-08-17 (M11 complete — commit `ab119d6`)**: full `UiContract` (spec §10.13): commands, notifications, `UiSnapshot`, `INotificationSink`, `MonitorInfo`, `LibraryItem` — plus the spec's **control→command mapping table** (every control posts a `Command`, never calls engine code). `Win32UI` (lazy create on first show, destroy on close, DPI-aware, 6 tabs) + panels: **Home** (wallpaper/video/monitor/state/governor reasons/FPS/decoder/GPU adapter), **Library** (ListView columns, toolbar add-file/folder/remove/refresh, lazy metadata on selection, sort), **Playlists** (items/mode/loop + next/prev/current), **Monitors** (per-monitor + clone/independent + scaling), **Performance** (advanced toggles/thresholds/delays/modes), **Settings** (start-with-Windows via HKCU Run, minimize-to-tray, battery mode, log level, README button). `TrayController` (icon + tooltip + menu Resume/Pause/Next/Prev/Open/Settings/Exit + left-click toggle). **`LibraryManager`** (add file/folder, remove, incremental refresh, `ReadDirectoryChangesW` recursive watch, background metadata probe via `DecoderManager::probeMetadata` — worker never touches `items_`; watch-prune on remove). **Config write-batching** (spec §9): `markDirty` (injectable clock) + `maybeFlushDirty` debounced save + `applyConfigSet` pure mapping; **governor live setters** (`setBatteryPauses`, `setLongPauseReleaseSeconds`). Engine helpers: `WallpaperManager::grabFrameSnapshot` (staging readback → preview HBITMAP) + `DecoderManager::probeMetadata`. App wiring: command queue (`postCommand`/`drainCommands` on the control thread), `subscribe`/`getUiSnapshot`/telemetry timer (armed only while the UI window exists), tray lifecycle, FOCUS→UI, EXIT. **13 new tests → 162/162** (library scan/dedup/remove-reindex/watch/refresh/lazy-probe; config debounce + applyConfigSet; governor setters), Debug + Release, 0 warnings. **Live**: boot + play clean; 5× open (second-instance focus) → close (destroy path, `minimizeToTray=false`) cycle with **0 log errors** — no control/timer/subscription leak (spec §10.9). **NOT MEASURED**: tray left-click/menu interaction + 10k-file folder stress (need human eyes/keystrokes — wiring code-reviewed; tray icon present while running); frame-snapshot preview verified via code review only.
- **2026-08-17 (M11 bugs caught before commit)**: **(1)** `LibraryManager::addFiles` passed raw paths to `addItemInternal`, which never checked the extension — a `.txt` entered the library (only the watch/`addFolder` paths pre-filtered). Fixed centrally in `addItemInternal` (`isVideoFile`). **(2)** The config debounce test mixed fake/real clocks: `markDirty()` anchored at `steady_clock::now()` while the test drove `maybeFlushDirty(t)` with a fake clock, so the re-arm case flushed early. Fixed by making `markDirty(now)` injectable (defaults to the real clock — app call sites unchanged). **(3)** Test-authoring bugs: `itemById(0)` is always null (ids start at 1) — replaced with path/iteration-based lookup. **(4)** A transient MSVC C4702 during incremental compiles of the test file (absent on clean rebuilds — no action).

---

## M12 — Recovery hardening

**Objective:** survive Explorer restarts, device loss, decoder/file failures, config corruption.

- [x] Explorer restart: host-invalidity detection → rediscovery → rebuild hosts → re-assign monitors → resume (playlist/config untouched); tested via `explorer.exe` kill/restart
- [x] Device loss (`REMOVED/RESET/HUNG`): stop render → release resources → `GetDeviceRemovedReason` → recreate device + DXGI manager + textures → restart decoder → resume; controlled retry/backoff, no tight loops
- [x] Decoder failures: log, mark unavailable, advance; tracked retry attempts (no endless retry of same broken file)
- [x] File change detection: `ReadDirectoryChangesW` on watched folders; handle delete/move/rename/replace of playing file gracefully
- [x] Config corruption: `.bak` backup + defaults + continue startup
- [x] Deterministic shutdown under all failure modes

**Verify:** fault injection — kill explorer, disable GPU (test rig), corrupt/rename playing file, corrupt config — each recovers/degrades gracefully; no hang on exit. **Exit:** ✅ (166/166 both configs; live: Explorer kill/restart, playing-file rename, harness device-loss injection; see notes)

**Notes:**
- **2026-08-17 (M12 complete — commit `da97111`)**: **Device loss** — the full recover sequence is now wired: a device-lost Present failure sets `D3D11Renderer::deviceLost_`, `WallpaperHost::render()` schedules the recreate, and the 1 Hz `WallpaperManager::onTick` runs `recreateDeviceResources()` (teardown hosts → release all textures/SRVs → `D3D11DeviceManager::recreate()` on the SAME adapter with `GetDeviceRemovedReason` logged → rediscover + rebuild hosts → re-render). Controlled retry/backoff: 1 Hz while failures are fresh (≤10), then every 30 s — no tight loops, self-recovers when the GPU returns. Per-frame render-failure log spam during the gap is suppressed (one warn per loss event). **Explorer restart** — the M3 stub is now full logic: the rebuild path calls the new `rebindLastFrames()` so a **PAUSED** wallpaper doesn't regress to the checkerboard after Explorer restarts (clone + per-monitor paths). **Decoder/file failures** — `PlaylistManager` now tracks per-item **attempt counts** (capped at 3 per run): a dead-end wrap (`nextIndex` finds nothing playable) calls `retryUnavailableOnce()` — items below the cap get one more chance (a restored file plays without restart); capped items stay dead. `adopt`/fresh runs reset. The playing-file rename case is handled by Windows semantics (the open Source Reader keeps the handle — playback continues, verified live); delete/corrupt surfaces a ReadSample failure → EOS → mark-unavailable → advance (existing tested path). **Config corruption** was already complete (M1 `.bak` + defaults + continue). **4 new tests → 166/166** (attempt cap, dead-end retry re-enables uncapped/capped stays dead, bounded retries, adopt reset), Debug + Release, 0 warnings.
- **2026-08-17 (M12 live verification)**: **(1) Device loss via harness fault injection** — new `vw_gfx_harness --device-loss`: inject at frame 120 → render fails while pending (expected) → 1 Hz tick recreates → recovered at frame 146 with hosts intact → rendered to 300 cleanly. **(2) Explorer restart** — killed `explorer.exe` live: the app logged `wallpaper layer invalidated (Explorer restart?) — rebuilding` within 1 s, rebuilt hosts twice while the shell respawned, restarted Explorer, recovered with **0 log errors** and stayed alive. **(3) Playing-file rename** — renamed the playing mp4 mid-playback: the open handle keeps reading, playback continued ~33 fps with no errors, no crash; file restored after. **NOT MEASURED (declined — disruptive/risky on this laptop)**: real GPU driver reset/disable (the harness injection + `GetDeviceRemovedReason` path is code-reviewed and the recreate sequence is proven end-to-end).
- **2026-08-17 (M12 review — commit `064db4b`)**: five fixes. **(1) `rebindLastFrames` used the WRONG aspect on the Independent path** — it passed the clone path's `frameDisplayAspect_` (0 in Independent mode → identity UV, i.e. a stretched frame after Explorer restart; invisible on this machine only because the content matches the monitor aspect). `PerMonitorFrame` now stores its own `displayAspect` (set by `setVideoFrameFor`, used by the rebind). **(2) The dead-end-only retry was too weak** — a single broken file among healthy ones was NEVER retried mid-run (only the all-broken dead end). Replaced with **retry-at-top of `nextIndex`**: every navigation re-enables uncapped unavailable items (bounded by the 3-per-run cap) — a restored file plays within a few cycles, uniformly across Single/Sequential/Loop/Shuffle. `replace()` also resets unavailable + attempts (a replaced item is a new file). **(3) `recreateDeviceResources` reset the failure counter and logged "complete" even when the rebuild failed** (Explorer dead) — now the device-retry state clears but the log distinguishes "wallpaper resumed" vs "hosts not built — Explorer path will retry". **(4) Harness `--device-loss` injection clamped to `min(120, frames/2)`** so short runs still test it. **(5) The recurring MSVC C4702 was NOT transient** — it's a real warning on range-for loops whose body unconditionally `break`s (reproduced in a 17-line file); the flagged loop is now an explicit `items().front()`. **3 new tests → 169/169** (broken-item retried on later cycles, Shuffle dead-end retry, replace reset), Debug + Release, 0 warnings. Harness device-loss re-verified (146 → recovered), app smoke clean.

---

## M13 — Profiling, optimization & stability validation

**Part A — Profiling & optimization:**

- [x] Baselines for all states (plan `04` §4.4) before optimizing
- [x] Hot-path audit: decoder, frame queue, scheduler, render, stats (allocations, locks, copies, syscalls, GPU submissions) via WPR/WPA, GPUView, PIX, VS profiler
- [x] Code-search audit: `while(true)`, `while (running)`, `Sleep(`, `sleep_for`, `new`, `malloc`, `memcpy`, `CopyResource`, `Map`, `Unmap`, `CreateTexture`, `CreateThread`, per-frame allocations — justify/fix each
- [x] Optimizations measured before/after; **revert if no measurable win**
- [x] No fake claims: report measured near-zero, never literal zero

**Part B — Reliability & stability:**

- [ ] 4–8 h soak (spec-reduced from 24 h; interview decision — report states the reduction): RAM/VRAM/threads/handles sampled every minute; fix any monotonic growth (leak) — **running now, CSV at `build/release/soak_m13.csv`**
- [x] Stress matrix subset (single display): 1080p30/60 H.264 played across the soak; 4K/AV1/HDR rows NOT MEASURED (no HW decode on this machine, M5)
- [x] Repeated play/pause/resume/next/prev/monitor-change/UI-open-close cycles — no handle/COM/thread growth (leak-cycle stress below)
- [ ] Game/fullscreen/power/hot-plug scenarios (plan `04` §4.3.3–4.3.5) — game/fullscreen live-tested in M9/M10; power/hot-plug declined (disruptive, single display)
- [x] Fix every defect found; no "known leak" acceptance

**Exit criteria:** resource targets met (plan `04` §4.7) or documented deviations; no leaks; stress matrix green. ☐ (soak pending)

**Notes:**

- **Code-search audit clean** — no `while(true)` busy loops, zero `Sleep()` in src (workers block on events/conditions), no raw `new`/`malloc` (all RAII), `Map`/`Unmap` only in the inherent software-decode upload/readback paths, `memcpy` only in the frame copy + texture upload (measured, see below), `CreateThread` only via `std::thread` (app-created: decode worker, library probe, library watch — 3, plus the UI/control thread; the 34–40 process thread count is dominated by MF/COM/D3D internal pools and is flat across the stress, no unbounded creation), no `TODO`/`FIXME`/stub/placeholder remaining.
- **Baselines (Release, 2026-08-17)** — playing: ~190% CPU / 408 MB private / 1367 handles / 40 threads (steady, no growth); paused → **SUSPENDED: 0.0–0.8% CPU**, RAM drops to 124 MB; resume returns to ~410 MB. Pre-optimization record (docs/04 §4.6).
- **Hot-path measurement** — per-frame cost split in the decode worker (300-frame windows, in-thread timing): **ReadSample (MF software decode) ~23–30 ms/f** — dominant, not optimizable from our side (this machine has no hardware MFT, M5); **frame copy 4.5 → 1.5 ms/f** after the optimization below (resize+zero 4.5 → 0.00 ms/f, memcpy ~1.45 ms/f irreducible — the 14.7 MB RGB32 copy); push ~0.01 ms/f. No per-frame allocations, locks, or syscalls remain in the hot path.
- **Optimization kept: frame-buffer recycle pool (FrameQueue)** — the decode worker previously `resize()`d a fresh 14 MB `std::vector` per frame (VirtualAlloc + demand-zero page faults, measured 4.5 ms/f of zeroing). Now `FrameQueue::takeSpareBuffer`/`recycleBuffer` recycle the consumer's buffer after the GPU upload (bounded at capacity, drained on clear/close, small buffers refused). Measured after: **resize+zero 0.00 ms/f**, pool hit 300/300. Reverted-instead-of-kept alternatives: none tried — the pool was the only candidate with a measured win; the ~16%-of-one-core saving is real but the process CPU% is dominated by the software decode (~23 ms/f ReadSample), so the end-to-end CPU delta is within sample noise (~190% → ~185–210%).
- **Timing-instrumentation bug caught during measurement** — the first cost split read the resize accumulator AFTER the memcpy loop, double-reporting resize as ≈ memcpy (1.49+1.48 vs copy total 1.51, impossible); fixed the window, re-measured, then removed all TEMP instrumentation. Also: `rpcndr.h` (via `windows.h`) defines `#define small char` — a test variable named `small` collided (renamed `tiny`).
- **Leak-cycle stress (Release)** — 6× play/pause + 4× UI open/close cycles with per-2 s sampling: playing CPU 190–230% (paused 0–5%), private memory flat **423 MB** (388 MB paused), handles 1366–1368 flat, threads 37–40 flat; UI cycles bounded (437 → 423 MB after close); **0 unexpected WARN/ERROR** (only the known startup hardware-decode fallback). No handle/COM/thread growth.
- **4 h soak #1** (2026-08-17 18:50, superseded) — playback looping, samples every minute to `build/release/soak_m13.csv`. **Abandoned at minute 32**: the M14-review/UI-review rebuilds stopped the app, and its minutes 7+ were contaminated by the UI the packaged-exe focus test opened (see the mid-run note below). Its data is NOT used for the M13 gate — superseded by the final run below.
- **Soak mid-run note (M14 review, 19:23–19:36)**: the packaged-exe clean-extraction test sent the FOCUS message, which **opened the app UI** (`ui_->show()`, spec §10.1); with `minimizeToTray=true` the window stayed alive hidden-to-tray, and the Performance-panel `CONFIG_SET: pauseOnGame` toggles in the log (19:25:36) were real checkbox clicks. The open UI's control set is the **+420 handle step-up** (1363 → ~1780) in that run's CSV minutes 7+ — **flat, no growth** across 10+ samples (1770–1786), threads 36–42, RAM stable (434 MB). Consistent with M11's bounded UI open/close proof — not a leak; the run was abandoned anyway.
- **Soak #2 — final run (2026-08-17 20:05)**: fresh 4 h run on the **final binary** (UI telemetry-timer fix + desktop-click fix; the UI stays closed throughout — verification runs are done). Minute 1: **426.0 MB private / 1365 handles / 37 threads** — clean UI-closed baseline, matches history. CSV at `build/release/soak_m13.csv`, completion ~00:05; result read at completion and recorded below before M13 closes.
- **NOT MEASURED** — VRAM (no hardware decode path on this machine; textures are 1 dynamic upload texture + hosts), 4K/AV1/HDR stress rows (no HW MFT, M5), power/hot-plug scenarios (declined — disruptive, single display).

---

## M14 — Packaging, README, final report

- [x] Portable ZIP via script (`package.ps1`): exe + 3 MSVC runtime DLLs + LICENSE + README only (1,054,769 B zip / 2,366,103 B uncompressed); no samples/debug/symbols/test assets; verified from a clean extraction (second instance loaded the CRT, focused the primary, exited 0)
- [x] Start-with-Windows optional (HKCU Run — implemented M11; verified absent by default, no admin, no service)
- [x] `README.md` per spec §64 — full README written (overview, architecture, requirements, build/run, codecs, HW accel, multi-monitor, performance behavior, game detection, config, troubleshooting, limitations, development, testing + how low idle usage is achieved)
- [x] Final report per spec §65 + doc 3 §101 (22-point list) — `docs/07-final-report.md`, measured numbers or explicit `NOT MEASURED — reason`
- [x] Resource-efficiency audit answering doc 2 §102's 20 questions — `docs/08-resource-audit.md`
- [x] **Version resource** added (Explorer Properties → Details): `VS_VERSION_INFO` was a **string-named** resource because `winres.h` (which defines it as numeric ID 1) was never included — Windows version APIs look up RT_VERSION by ID 1, so FileVersion read empty. Fixed with `#include <winres.h>`; verified `FileVersion 1.0.0.0` + resource tree `named=0 id=1` (matches cmd.exe)

**Exit criteria:** package builds from clean checkout; README complete; report + audit complete with no invented numbers. ☐ (soak pending for M13's gate)

**Notes:**

- **Package (2026-08-17)**: `dist/VideoWallpaper-<commit>.zip`, 6 files: exe 1,602,048 B + msvcp140/vcruntime140/vcruntime140_1 (716 KB total) + README + LICENSE. DLL-import dump: all imports are system DLLs except the MSVC runtime (those 3 ship). Shaders embedded in the exe (no runtime lookups). Second-instance clean-extraction test: packaged exe ran, found the primary, requested focus, exited 0.
- **README/report/audit** (2026-08-17): `README.md` (full §64), `docs/07-final-report.md` (22-point list), `docs/08-resource-audit.md` (20 questions). Every performance number cross-checked against the M13 baseline/stress/measurement records; unmeasurable items explicitly `NOT MEASURED — reason`.
- **Version resource fix** (2026-08-17): `src/app/app.rc` gained a `VERSIONINFO` block, but FileVersion stayed empty — the PE resource directory showed RT_VERSION as a **named** entry (string "VS_VERSION_INFO") instead of **numeric ID 1**; Windows version APIs (`GetFileVersionInfo`) look up by ID 1. Root cause: `VS_VERSION_INFO` is a macro (`= 1`) defined in `winres.h`, which the .rc didn't include. Added `#include <winres.h>`; verified `FileVersion 1.0.0.0` / Product 1.0.0.0 / description / company via .NET `FileVersionInfo` and a direct PE resource-directory walk (tree: `named=0 id=1`, matching cmd.exe). Package rebuilt with the fixed exe.
- **Soak note**: superseded by **soak #2** (see M13 section) — a fresh 4 h run started 2026-08-17 20:05 on the final binary (UI + desktop-click fixes), completion ~00:05; CSV at `build/release/soak_m13.csv`, result read at completion and recorded before M13 closes.
- **M14 review fix (flaky library test)**: `LibraryManager::requestMetadata` deduped only against `probeQueue_`, but the worker pops before probing (outside the lock) — a second request in that window re-enqueued and returned true (the test's `CHECK_FALSE(requestMetadata(realId))` failed intermittently in Debug, observed 1-in-4). Added `probeInFlight_` (popped-but-not-completed counts as pending; worker removes it after writing the result). 171/171 re-verified over multiple consecutive runs, Debug + Release. Details in BUILD_NOTES.
- **UI resource review (2026-08-17)**: `syncUiSubscription()` keyed on `ui_->exists()` (window created), so with `minimizeToTray=true` (default) closing the window only hid it and the **2 Hz telemetry timer ran forever** (library poll + config flush + hidden `SetWindowTextW` + tray tooltip every 500 ms — the spec's "stop expensive UI work when minimized to tray" was violated). Fixed: subscribe keys on `ui_->isVisible()`; every open path re-syncs (FOCUS handler, ShowUi/ToggleUi/Focus commands, onClose both branches); reopen also **pull-refreshes every panel** (`refreshFromSnapshot` — previously only Home got an immediate push). Bonus fix: persisted `logLevel` was only applied live (Settings panel), never at startup — Release always started at Info; now honored at startup. **Live-verified** via a debug tick marker + logLevel=debug: ticks/6s = 0 (UI closed) → 12 (UI open) → 0 (hidden to tray) → 12 (reopened); handles/threads bounded (UI open ≈ +420 handles, flat). 171/171 Debug + Release, 0 warnings.
- **Desktop-click pause fix (2026-08-17)**: clicking the desktop paused the video — the foreground becomes **Progman**, which covers the monitor with `WS_POPUP`, so `classifyWindowState` returned Fullscreen and `pauseOnFullscreen` paused. Fixed with `isDesktopShellClass` (`Progman`/`WorkerW`/`SHELLDLL_DefView` never classify as fullscreen; checked before the rect/style geometry). **Verified live**: foregrounding Progman logs `window: windowed` (pid explorer.exe), no pause lines; the unchanged geometry path still classifies a real popup fullscreen app as Fullscreen (unit test). 172/172 Debug + Release, 0 warnings.

---

## OPT — Extreme resource optimization (post-M14, spec `extreme-resource-optimization.txt`)

Followed the spec's order: **inspect → profile → bottleneck report → highest-impact changes first → benchmark each**. All numbers Release-build, same methodology both builds (25 × 1 s samples after 8 s settle, 2560×1440@60 H.264 software decode). Full audit `docs/09-optimization-audit.md`; full report `docs/10-optimization-report.md`.

- [x] **Audit + bottleneck report** (`docs/09`) — hot path walked end-to-end (decode → copy → upload → render → present); B1 (CPU NV12→RGB32 conversion + 4 B/px upload), B2 (redundant per-frame CB update), B3 (per-frame plane-SRV creation on the hardware path); B4 = verified-clean list (no busy-wait, pooled buffers, no per-frame allocs)
- [x] **B1 — software NV12 end-to-end** (the headline): decoder negotiates native NV12 (no Video Processor MFT — the CPU color conversion stage is gone); frames copied as tightly-packed NV12 (1.5 B/px); `WallpaperManager` uploads to an NV12 texture + plane SRVs (clone + per-monitor paths); the existing YUV shader converts + scales on the GPU. Per-file RGB32 fallback retained; preview readback honestly reports "no preview" on the NV12 path (spec §10.5 fallback)
- [x] **B2 — CB dirty-tracking** (spec §22): `render()` updates `frameCb_` only when tint/scaleOffset changed (memcmp equality)
- [x] **B3 — plane-SRV cache** (spec §21/§38): Y/UV views cached per decoder surface (bounded map, cap 64, cleared on device recreate); zero per-frame `CreateShaderResourceView` on hardware machines (latent here — NOT MEASURED)
- [x] **Benchmarks** (old build vs new, identical methodology):
  - CPU avg **180.8 % → 136.2 %** (−25 % total; **−58 % per presented frame** — 5.65 → 2.39 CPU%-s/frame)
  - RAM private **466.1 → 444.2 MB** avg (peak 467.7 → 446.3)
  - Decode **32 → 60 fps**; presented **32 → ~57 fps** (was decode-bound; now near source rate)
  - Upload **14.7 → 5.5 MB/frame** (−62 %); disk I/O 5.8 → 11.8 MB/20 s (2× frames decoded; ~0.6 MB/s — negligible)
  - Render avg 0.4 ms both builds (no regression); dropped counter 0 → ~3/s — **freshness policy, not lost frames** (old build silently under-presented ~28 fps)
  - Visual quality unchanged — same BT.709 limited→full-range conversion, now on the GPU (shader documented M5)
- [x] **Tests**: 172/172 Debug + Release, 0 warnings (NV12 sizing assertion added to the real-file test; per-path RGB32/NV12 asserted)
- [x] **Live-verified** (Release, this machine): log shows `decoder: software (NV12 output)`; no render/device errors; NV12 path active end-to-end

**Acceptance (spec §57):** the 1080p60 targets (<1 % CPU, <200 MB RAM, 0 drops) are **hardware-decode targets** — this machine has no working hardware MFT (verified M5/M14), so decode is inherently software (§57: "maintain quality and explain the bottleneck"). Delivered: software path at its floor (no CPU color conversion, native-format upload, zero redundant per-frame GPU work), measured before/after, no quality/FPS regression.

---

## Cross-cutting gates (apply to every milestone)

- [ ] Debug **and** Release x64 builds green
- [ ] Unit tests green (CTest)
- [ ] No busy-wait / per-frame allocation / per-frame log regressions (code-search audit)
- [ ] Working state recorded (git commit when permitted)
- [ ] `docs/06-progress-checklist.md` summary table + notes updated
