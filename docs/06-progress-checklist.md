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
| M8 — Multi-monitor & multi-GPU | ☐ | — | |
| M9 — Detection & monitoring | ☐ | — | |
| M10 — Resource governor & suspension | ☐ | — | |
| M11 — UI, tray & minimal library | ☐ | — | |
| M12 — Recovery hardening | ☐ | — | |
| M13 — Profiling, optimization & stability | ☐ | — | |
| M14 — Packaging, README, final report | ☐ | — | |

**Current milestone:** _M6 — Frame timing, queue & scheduling_

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

- [ ] Full monitor events → create/destroy/reposition hosts; no restart; no resource leaks on disconnect
- [ ] **Clone:** one decoder + one timeline + one source frame; N GPU renderers
- [ ] **Independent:** N decoders only for N distinct videos; shared device/factory/shaders
- [ ] Mixed refresh rates: per-monitor presentation deadlines (single render worker)
- [ ] Multi-GPU: monitor→adapter association; prefer output-driving adapter; documented per-adapter fallback
- [ ] Portrait/ultrawide/4K scaling (no fixed-resolution assumptions)

**Verify:** 3-monitor rig (or simulated): independent videos; clone in sync + decode-once (decoder-count diagnostics); hot-plug without restart; same video on 4K+1440p+1080p decoded once. **Exit:** ☐

**Notes:**
- **M5 forensics carry-over**: adapter selection (per-adapter fallback) is implemented and tested for *placement*, but it will **not unlock GPU decode on this machine** — both adapters refused the MS decoder's DXVA handoff identically (probe_dxva). Adapter locality still matters for presentation (hybrid-GPU laptop) and for machines where MF hardware decode works.

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
- [ ] **Threshold-pair cross-validation** (spec §9): pause ≥ resume for cpu/gpu/memory at load and on every `CONFIG_SET`; violations clamped (resume pulled toward pause) + logged
- [ ] **Config revision counter** (spec §9): bumped on every accepted config change so the governor reacts to live threshold/delay/mode changes without polling `ConfigurationManager`
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
- [ ] **Config write-batching** (spec §9): debounced dirty-flag save (~1–2 s after last `CONFIG_SET`, plus save on shutdown) — no per-click writes, UI edits survive a crash
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
