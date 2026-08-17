# Build Notes — Environment Audit (M0)

Findings recorded during the M0 toolchain/environment audit. Last updated: 2026-08-17.

## Machine

| Item | Value |
|---|---|
| OS | Microsoft Windows 11 Home Single Language, build 10.0.26200 |
| CPU | AMD Ryzen 5 7535HS with Radeon Graphics |
| RAM | 13.8 GB |
| GPU 1 | NVIDIA GeForce RTX 3050 Laptop GPU — driver 32.0.15.9597 |
| GPU 2 | AMD Radeon Graphics (integrated, Ryzen 7535HS iGPU) |

**Implications:**
- Hybrid-GPU laptop (NVIDIA dGPU + AMD iGPU) — M8 adapter selection and "don't wake the discrete GPU needlessly" (doc 2 §59) are directly testable here.
- RTX 3050 NVDEC supports H.264 / HEVC / AV1 hardware decode — but **hardware decode through Media Foundation is NOT achievable on this machine** (M5 forensics: the MS decoder MFTs refuse their internal DXVA handoff despite working drivers; see the M5 section). GPU decode may still be reachable via NVDEC direct APIs or a different framework — out of scope for v1.
- 13.8 GB RAM: adequate; keep 24 h stability runs to a single 1080p–1440p wallpaper or monitor VRAM/RAM headroom with 4K + multi-monitor.

## Toolchain status

| Component | Status |
|---|---|
| MSVC (cl.exe) | ✅ **INSTALLED** — VS 2022 Build Tools 17.14.37, toolset `14.44.35207` at `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` |
| CMake | ✅ **INSTALLED** — 4.4.2 at `C:\Program Files\CMake\bin\cmake.exe` |
| Windows SDK | ✅ **INSTALLED** — 10.0.26100.0 (`C:\Program Files (x86)\Windows Kits\10`); MF/D3D11/DXGI headers (`mfapi.h`, `mfreadwrite.h`, `mfidl.h`, `mftransform.h`, `d3d11.h`, `dxgi.h` in `shared/`, `dcomp.h`) and x64 libs (`Mf.lib`, `Mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`, `d3d11.lib`, `dxgi.lib`, `d3dcompiler.lib`) all present |
| Ninja / make / gcc / clang | not found (VS generator used instead) |
| winget | available (`C:\Users\mbk43\AppData\Local\Microsoft\WindowsApps\winget.exe`) |
| FFmpeg | not checked on PATH (informational only — not a dependency) |
| doctest | ✅ vendored at `tests/doctest.h` **v2.4.11** (single fetch from GitHub, then committed — offline builds) |

**Toolchain verification (M0 exit criteria):** ✅ trivial CMake x64 project configured with the `Visual Studio 17 2022` generator and **built + ran successfully in both Debug and Release** (`m0check.exe` → "M0 toolchain check OK"). Scratch folder removed after verification.

**Install command used (2026-08-17, with user permission):**

```text
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id Kitware.CMake -e
```

## GPU adapters (from `Win32_VideoController`)

| Adapter | Driver | Notes |
|---|---|---|
| NVIDIA GeForce RTX 3050 Laptop GPU | 32.0.15.9597 | NVDEC HW decode (H.264/HEVC/AV1); renderer candidate |
| AMD Radeon Graphics | 32.0.21045.1000 | iGPU; power-efficient fallback for simple wallpaper playback (doc 2 §59) |

Decoder mode must be read from the actual Media Foundation transform at runtime (M5) — do not assume vendor capabilities from these names alone.

## Test video assets on disk (found at M0)

| Path | Notes |
|---|---|
| `C:\Users\mbk43\Videos\bgcmp\` | Many `.mp4` live-wallpaper clips, incl. `Eula-Genshin-Impact.3840X2160.mp4` (4K) and a "4K60" clip |
| `C:\Users\mbk43\Downloads\Video\` | 1080p `.mp4` clips (YouTube downloads) |

**Codec probe (2026-08-17, container fourcc scan of all 11 `.mp4` files):**

| Codec | Files | Coverage |
|---|---|---|
| H.264 (`avc1`) | 10 of 11 (incl. the 4K `3840X2160` and "4K60" clips) | ✅ good for M4/M5/M13 stress matrix |
| HEVC (`hvc1`) | 1 — `Furina-Stage-Water-Genshin-Impact-Moewalls-Com.mp4` (11 MB) | ✅ one sample |
| AV1 (`av01`) | 0 | ❌ missing — need to source or synthesize |
| VP9 (`vp09`/`vp08`) | 0 | ❌ missing (a `.webm` would cover this) |
| HDR10 (`mdcv`/`clli`) / Dolby Vision (`dvhe`) | 0 | ❌ no HDR content — P010/10-bit path may be unverifiable without a synthetic clip |

**Caveats:** the fourcc scan is a heuristic (bit depth not distinguishable from `hvc1` alone); M4's real MF metadata reader is authoritative. For full test coverage (doc 1 §55: H.264 1080p/1440p/4K, HEVC 4K, AV1 4K60, plus a 10-bit/HDR clip), AV1/VP9/HDR samples must be **sourced or synthesized** (e.g. via ffmpeg, not a runtime dependency) or reported `NOT MEASURED` at M13.

---

# M1 Build Notes (2026-08-17)

M1 exit criteria met: Debug+Release x64 build green, 16/16 unit tests pass in both configs, runtime behavior verified.

## Findings

- **`/std:c++23` confirmed** — compiler is MSVC **19.44.35228.0**; no fallback to `/std:c++latest` needed. `std::expected` + `std::format` (wide) compile clean.
- **Config & log storage are UTF-8** (explicit conversion via `util/utf8.h`, `CP_UTF8`). Initial M1 assumption was wrong and was caught by review: MSVC `std::wfstream` is **asymmetric** — its *write* path converts to the ANSI codepage (drops chars > 0xFF) while its *read* path decodes UTF-8. ASCII worked by coincidence; any non-ASCII value (é, CJK paths) was corrupted or silently dropped. **Fixed: byte streams (`std::ofstream`/`std::ifstream`) + explicit `wideToUtf8`/`utf8ToWide`**; invalid UTF-8 on read is treated as corrupt config (backup + defaults). Covered by `tests/test_utf8.cpp` + a non-ASCII config round-trip test.
- **Config format** — JSON sections `general` / `playback` / `performance` / `battery` / `detection`; first run writes defaults; corrupt file → `config.json.bak` + defaults rewrite; atomic save (temp + rename).
- **Single instance** — `Local\VideoWallpaper.SingleInstance` mutex; second instance finds the control window via class name, posts a registered focus message, exits 0 (verified live: first instance logged "second instance requested focus").
- **Control window** — class `VideoWallpaperControl`, `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`, no taskbar; `WM_APP` = shutdown request; message pump blocks when idle (`GetMessageW`).
- **Logger** — 5 MB/file rotation (`current.log` → `previous.log`, ≤10 MB total), mutex-guarded, aggregate events only, Release default INFO, Debug default DEBUG.
- **Gotchas fixed during M1** (worth remembering):
  1. `std::filesystem::rename` over an open `ifstream/wifstream` fails on Windows (sharing violation) — close the stream before renaming (config corrupt-backup path).
  2. `replace_extension(L"json.bak")` does not yield `config.json.bak` — plain `+= L".bak"` was used instead.
  3. `const auto` on `std::filesystem::path` locals prevents `+=`/`replace_extension` (const-correctness trap) — plain `auto`.
  4. `std::ofstream` cannot write `wchar_t*` — wide streams (`wofstream`/`wifstream`) required for wide strings.
  5. `std::format` wide format string requires **wide** string literals for all args (`L"0.1.0"`, not `"0.1.0"`).
  6. `ControlWindow` is non-copyable (owns an HWND) — deterministic shutdown via an idempotent `destroy()` method, not copy-assignment.
  7. Template params that never appear in the parameter list are not deducible (`readStrings` had a stray `typename F`).
  8. `std::map::emplace` does **not** overwrite existing keys — duplicate JSON keys were silently first-wins; changed to reject (strict validation, D-06) so ambiguous config hits the corrupt-recovery path.
  9. Test literals with nested quotes/backslashes are error-prone — use C++ raw string literals (`LR"(...)"`) for JSON fixtures.
  10. **Never use `std::wfstream`/`std::wifstream` for files** — MSVC write = ANSI codepage (drops non-ANSI), read = UTF-8 decode (asymmetric, corrupts non-ASCII). Byte streams + explicit `CP_UTF8` conversion (`util/utf8.h`) instead.

## Pre-M2 review refactors (2026-08-17, applied)

- **JSON recursion depth cap (512)** — user-writable config could otherwise stack-overflow the parser; deep nesting is now a clean parse error → corrupt-recovery path (test locks it).
- **`initPaths` fallback** → `%TEMP%\VideoWallpaper` instead of the working directory (plan rule: never write next to the executable).
- **Build hygiene**: `/WX` (warnings-as-errors) on app + harness targets (Debug+Release are warning-free); `/RTC1` in Debug; `/Zi` + link `/DEBUG` in Release (PDBs now produced — verified `VideoWallpaper.pdb` in build/release/Release/).

---

# M2 Build Notes (2026-08-17)

M2 exit criteria met: DeviceManager + Renderer + TextureManager shipped, build-time fxc with embedded shaders, verified on both GPUs.

## Findings

- **Build-time fxc pipeline**: `shaders/VideoShader.hlsl` → `.cso` (fxc from the Windows SDK, auto-located via `CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION` with a glob fallback) → **embedded as byte arrays** via `cmake/embed_shader.cmake` into `build/<cfg>/generated/VideoShader{Vs,Ps}Data.h`. No runtime file lookups — keeps the M14 package to exe + system DLLs only. Sizes: VS 736 B, PS 836 B.
- **Verified on both GPUs (Debug + Release)**: feature level **0xB100 (11_1)**, ~147 FPS vsync-locked to the 144 Hz display, clean `--frames N` exit. Release: no debug layer (correct).
- **Harness now drives the real modules** — it no longer duplicates device/swapchain/pipeline code; `--list`/`--frames`/`--adapter`/`--no-debug` flags unchanged.
- **Debug layer now INSTALLED (2026-08-17)** — see the "Graphics Tools install" section below; Debug builds get a real debug-layer device on both GPUs.

## Gotchas (new)

1. **Flip-model `ResizeBuffers` with the same size before the first Present returns `DXGI_ERROR_INVALID_CALL`** — the renderer tracks its current size and skips `ResizeBuffers` when unchanged; `init()` builds the RTV directly since the swap chain was just created at that size.
2. **`D3D11_FILTER_LINEAR` does not exist in D3D11** (it's a D3D9-era name) — use `D3D11_FILTER_MIN_MAG_MIP_LINEAR`.
3. Graphics modules log via the `Logger` singleton; the harness doesn't initialize a file sink, so the DeviceManager's log lines are invisible there — the harness prints feature level / debug status itself (the app will get the log lines when it integrates the modules at M3+).

---

# M2 pre-verification — D3D11 harness findings (2026-08-17)

`harness/gfx_harness.cpp` (dev-only target `vw_gfx_harness`) verified the M2 renderer prerequisites on this machine.

## Verified working ✅

- **Device creation on both GPUs** — feature level **0xB100 (11_1)**, BGRA support, on the AMD iGPU (default adapter) and the NVIDIA RTX 3050 (`--adapter 1`).
- **Rendering + vsync present** — UV-gradient fullscreen triangle renders; **~142–146 FPS** at vsync on the 144 Hz display (i.e. vsync-locked to the display refresh), clean exit via `--frames N`.
- **Enumeration matches reality** — DXGI reports **3 adapters**: `AMD Radeon(TM) Graphics` (vendor 0x1002, **default adapter 0**), `NVIDIA GeForce RTX 3050 Laptop GPU` (0x10DE, adapter 1), `Microsoft Basic Render Driver` (0x1414, adapter 2).

## Corrected environment facts

- **Display is 1920×1080 @ 144 Hz physical** — the M0 audit's "1536×864" was the **125%-scaled** value; `EnumDisplaySettingsW` reports 1920×1080 @ 144 Hz. Still **one display** (single-monitor `NOT MEASURED` posture in the spec is unchanged).
- **Default adapter is the AMD iGPU**, not the NVIDIA dGPU — relevant to M8 adapter selection ("don't wake the discrete GPU needlessly"): the power-efficient iGPU is already the default.

## Debug layer: NOT installed ⚠️

- ~~`D3D11CreateDevice` with `D3D11_CREATE_DEVICE_DEBUG` failed with hr=0x887A002D (DXGI_ERROR_SDK_COMPONENT_MISSING)~~ → **RESOLVED 2026-08-17**: the debug layer is now installed and verified **ON on both GPUs** in Debug builds.
- The harness/DeviceManager **gracefully falls back** to a non-debug device when the layer is absent — the correct pattern, kept regardless.

## Graphics Tools (D3D debug layer) — install record (2026-08-17)

- **Capability name on this machine (Windows 11 10.0.26200): `Tools.Graphics.DirectX~~~~0.0.1.0`** — the older `Graphics.Tools~~~~0.0.1.0` name no longer exists on 24H2+ (a first install attempt with the old name was a silent no-op; a `Get-WindowsCapability -Online | Where Name -like '*Graphics*'` diagnostic revealed the new name).
- Installed elevated (UAC-approved) via `Add-WindowsCapability -Online -Name Tools.Graphics.DirectX~~~~0.0.1.0`; took ~7–8 min (download); `RestartNeeded=False`.
- **Verified**: Debug harness reports `debug layer=ON` on both the AMD iGPU and NVIDIA RTX 3050, feature level 11_1, 90 frames rendered cleanly. Release builds still skip the debug layer (gated).
- Side note: `d3d10sdklayers.dll` was already present in System32; the D3D11 debug layer is what the feature gates.

---

# M5-preview — harness textured-frame path (2026-08-17)

Pre-M5 de-risking: `vw_gfx_harness --video <path>` decodes ONE frame via the production Source Reader (RGB32 output), uploads it to a D3D11 texture, and renders it through the textured pixel shader — proving the texture upload + sampling pipeline before M5's GPU-frame work. **RGB32 CPU conversion + `Map`/`Unmap` upload is explicitly the throwaway preview**; M5 replaces it with `MF_SOURCE_READER_D3D_MANAGER` GPU surfaces + shader YUV→RGB (no CPU copy in happy path).

## Verified ✅

- **All 9 clips decode + render** (Debug + Release, AMD iGPU + NVIDIA RTX 3050): 8× H.264 + 1× HEVC (`Furina`), feature level 11_1, 90-frame runs clean.
- **Shader now has a texture-sampling PS entry** (`TexPSMain` + sampler), selected when a texture SRV is bound (`render(..., video)`); gradient path unchanged. New embedded shader: VS 736 B / PS1 836 B / PS2 ~1 KB.
- **Real stream resolutions differ from filenames** — "Eula 3840X2160" decodes as **2560×1440**, "Ganyu 4K60" also 2560×1440, one clip is **1916×1080** (odd width → nonzero row stride; the 2D-buffer pitch handling is exercised). Good data for M4's metadata reader + M13 stress matrix.

## Gotcha (the fix that made it work)

- **`IMFSourceReader::SetCurrentMediaType` with an RGB32 output type returned `MF_E_INVALIDMEDIATYPE` (0xC00D36B4)** on the H.264 decoder. Two required pieces:
  1. Create the reader with **`MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING = TRUE`** — the documented YUV→RGB32 path routes conversion through the Video Processor MFT; the decoder's own converter rejects the type.
  2. Set **`MF_MT_DEFAULT_STRIDE`** on the requested RGB32 type (via `MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride)`).
- Side effect of the probe: the harness now prints the **native video subtype** GUID (`{34363248-...}` = H264, `{43564548-...}` = HEVC) — useful for M5 decoder-mode diagnostics.
- Note for M5: the Source Reader + `SetCurrentMediaType` on the NV12/P010 GPU path must also set `MF_MT_DEFAULT_STRIDE` and the D3D manager attributes — same negotiation class of bugs.

---

# M3 Build Notes — Wallpaper host (2026-08-17)

M3 exit criteria met: checkerboard renders **behind desktop icons** full-screen, host is click-through, survives a live `explorer.exe` kill/restart with hosts rebuilt automatically. Verified on this machine in Debug + Release.

## Desktop arrangement on this machine (the big finding)

**Windows 11 24H2+ (build 26200) uses a DIFFERENT wallpaper-layer arrangement than the canonical one:**

```text
Progman (top-level)
├── SHELLDLL_DefView → SysListView32   # icons — stay INSIDE Progman
└── WorkerW (spawned by 0x052C)        # CHILD of Progman, bottom of its z-order = the wallpaper layer
```

- The canonical code (find a top-level `WorkerW` containing `SHELLDLL_DefView`, then take the next WorkerW) finds **no DefView in any top-level WorkerW** here. Naively falling back to Progman puts the host ABOVE the icons (bad).
- The working discovery: after 0x052C, if no top-level DefView WorkerW exists, take the **first WorkerW child of Progman** (this build spawns exactly one, at the bottom of Progman's child z-order → behind icons).
- `WallpaperManager::discoverDesktop()` handles arrangement A (classic), arrangement B (this machine), and the no-WorkerW fallback; the actual arrangement is logged at startup.
- Note: many orphaned top-level WorkerWs accumulate across Explorer restarts on this build (12 found) — the discovery ignores them.

## Child-window DPI virtualization (gotcha #11)

- A `WS_CHILD` window parented into another process's window (Explorer is system-DPI-aware at **120 DPI / 125%** here) is **DPI-virtualized into the parent's context regardless of the creating thread's DPI awareness**: physical rect = requested × 96/parentDpi. Requesting 1920×1080 yielded a 1536×864 window (then 1229×691 after an attempted divide fix).
- Fix: request `physical × parentDpi/96` via `GetDpiForWindow(parent)`; the swap chain keeps the **physical** size so the back buffer stays crisp. Verified: host rect exactly (0,0)-(1920,1080).

## Click-through (no WS_EX_TRANSPARENT needed)

- **Grid scan (45 points): the host window is NEVER the `WindowFromPoint` hit-test target** (0/45). The entire desktop (background + icons) is routed through the shell's XAML input system on this build — classic HWND hit-testing doesn't even reach the desktop's own ListView (no SysListView32 hits either). Structurally the host is also below the icon layer.
- Caveat recorded: SendInput-driven context-menu testing does NOT work in this environment (no menu appears even over Chrome) — the click-through conclusion rests on the hit-test scan + z-order structure; a manual right-click spot-check is still worthwhile.

## Explorer-restart recovery (stub, verified live)

- `taskkill /f /im explorer.exe` → the app's 1 Hz tick logged `wallpaper layer invalidated (Explorer restart?) — rebuilding` within 1 s, retried while the shell was dead, and on Explorer's return **auto-rediscovered the new Progman/WorkerW and recreated the host** (new handles, 1920×1080, behind icons) — no app restart, verified via window enumeration.

## Bug found by M3 integration: `D3D11DeviceManager::createDevice(nullptr)` → E_INVALIDARG

- `D3D11CreateDevice` with a **null adapter requires `D3D_DRIVER_TYPE_HARDWARE`** (the `UNKNOWN` driver type only pairs with an explicit adapter). The harness always passed an adapter (`getAdapter(index)`), so M2 never hit it; the app passes null → 0x80070057. Fixed with a driver-type switch on the adapter pointer.

## Logger change (M3-adjacent)

- **`Logger` now flushes every line.** The old buffered writes were invisible to `tail` during long runs and were lost entirely on force-kill (observed: a 2-minute run's log was 0 bytes after `taskkill /F`). Volume is tiny (<1 Hz steady state) so per-line flush is free.

## Verified on this machine

| Check | Result |
|---|---|
| Host placement | parent=Progman-child WorkerW, rect (0,0)-(1920,1080), below `SHELLDLL_DefView` in z-order |
| Visibility | shown after first present (created hidden to avoid black flash) |
| Debug layer | ON in Debug (feature 11_1), OFF in Release (correct) |
| Click-through | host never hit-tested (0/45 grid points) |
| Explorer kill/restart | detected in ≤1 s, hosts rebuilt automatically |
| Tests | 54/54 (3 new monitor tests), 2316 assertions, both configs, 0 warnings under /WX |

## Harness `--wallpaper` mode (dev tool, 2026-08-17)

- `vw_gfx_harness --wallpaper [--adapter N] [--frames N]` drives the **real** `WallpaperManager` (discovery, hosts, checkerboard) from a script without the full app: message pump + per-frame `renderAll()` + the same 1 Hz `onTick()` Explorer-restart stub the app runs. `--frames N` exits cleanly (N vsync-blocked presents).
- `WallpaperManager::start(IDXGIAdapter1*)` now takes an optional adapter (the app passes null/default; the harness passes the chosen one for per-GPU verification) and `renderAll()` is public. The debug layer follows the build like the app (`--no-debug` does not apply in wallpaper mode).
- Verified: 1 host at (0,0)-(1920,1080), parented to the Progman-child WorkerW, visible, on **both GPUs** (AMD iGPU + NVIDIA RTX 3050) in Debug and Release; 3000-frame runs exit cleanly with no leftover process. (Note: PowerShell P/Invoke probes for nested windows kept failing during these checks — the C++ probe is the verification instrument of record.)

---

# M3 review (2026-08-17, before M4)

Fixed:
- **`WallpaperHost::setBounds` skipped the child-window DPI scaling** — it passed raw physical pixels to `SetWindowPos`, but the window lives in the parent's virtualized DPI context (the exact problem `init` had to solve). A monitor change would have mispositioned the host. Both paths now share `scaleToParentDpi()` (physical rect → parent-context rect; swap chain stays physical).
- **`WNDCLASSEXW` was missing `cbSize`** in `WallpaperHost::registerClass` (worked empirically; `ControlWindow` sets it).
- **1 Hz timer only set when `start()` succeeds** (no GPU → `running_` false → the timer was a no-op anyway).

Checked sound (no change): monitor-event lambdas die with the manager (both members), shutdown order (hosts → texture → device), `refresh()` diff pointers into the snapshot are safe (snapshot untouched during the loop), event order add-before-remove, retry semantics after a failed initial build, `Logger` per-line flush under rotation/concurrency tests.

Deferred notes for later milestones:
- **WM_DPICHANGED**: a display-scaling change resizes the child host (it's in the parent's DPI context) without our `setBounds` — re-apply bounds on display change (M8, alongside full monitor events).
- **Device-loss while running**: `render()` errors propagate and are logged; the full teardown/recreate path is M12.
- `MonitorManager::refresh` doesn't detect `workArea`-only changes (taskbar resize) — M8.
- The 0x052C `SendMessageTimeout` result is not checked (arrangement-B/fallback discovery covers a failed spawn) — acceptable, logged hierarchy shows what actually exists.

---

# M4 Build Notes — Media Foundation playback (software first) (2026-08-17)

## What shipped

- `src/video/`: `VideoMetadata` (subtype/codec/size/fps/duration/bit-depth/HDR/hasAudio from the **native** media type + presentation descriptor), `FrameQueue` (bounded, blocking push = backpressure, `close()` unblocks), `DecoderManager` (Source Reader, RGB32 output, demand-driven decode worker), `VideoPlayer` (open/start/pause/resume/stop/pollFrame, position tracking, EOS sentinel).
- Config: `playback.videoPath` (read/write/round-trip). App: `MFStartup`/`MFShutdown` in the deterministic shutdown sequence, ~60 Hz frame timer (M4 placeholder; M6 replaces with the FrameScheduler), pause/resume/stop via registered window messages (`VideoWallpaper.Playback{Pause,Resume,Stop}`), EOS → clean stop with the last frame on screen.
- `WallpaperManager::setVideoFrame()`: persistent dynamic texture + SRV (recreated on frame-size change), upload of tightly-packed B8G8R8A8, rebind on all hosts.

## SDK 26100 gotchas (headers checked against this SDK)

- **`MF_MT_VIDEO_BIT_DEPTH` does not exist** in the platform headers (also absent from the official media-type attribute list). Bit depth comes from the subtype family: P010/P016/Y210/Y216/v210/v216/Y410 → 10-bit, else 8.
- **`MF_SD_STREAM_MAJOR_TYPE` does not exist** in mfidl.h. Detect audio via the stream descriptor's media-type handler (`GetMediaTypeHandler` → `GetMajorType == MFMediaType_Audio`).
- **`MFVideoFormat_AV01` does not exist** — it's `MFVideoFormat_AV1` (FCC `AV01`).
- **`MF_SOURCE_READER_MEDIASOURCE` is a stream-index sentinel (0xFFFFFFFF), not a GUID.** It goes in the `dwStreamIndex` slot of `IMFSourceReader::GetServiceForStream` with `GUID_NULL` as the service GUID.
- **`mfreadwrite.h` must be included AFTER `mfidl.h`** (the reader interfaces are declared there). Including it alone parses as garbage (ComPtr/C2065 cascade).
- `MF_SOURCE_READER_FIRST_VIDEO_STREAM`/`ALL_STREAMS` are signed enum constants — cast to `DWORD` explicitly.

## Two real races found by live verification (both fixed)

1. **`DecoderManager::stop()` SIGSEGV (Release)**: `stop()` set `stopRequested_` and **nulled `queue_` before joining** the worker; a worker that reached `queue->push()` in that window dereferenced null. The worker now uses a **local copy of the queue pointer** captured at spawn; `stop()` closes the queue, joins, then nulls the member. (Surfaced as a flaky crash in the real-file decode test — Debug passed, Release crashed, classic timing-dependent race.)
2. **`VideoPlayer::tearDown()` hang on stop ("Not Responding")**: the queue was destroyed (`queue_.reset()`) **before** `decoder_.close()` joined the worker — destroying the mutex/CV under a worker still inside `push()`. Order fixed: `close()` → join → `reset()`. (`pause()` already had the correct order — only `tearDown()` was wrong.)

Lesson: any object a worker holds a raw pointer to must outlive the worker — join before destroy, or own it in the worker.

## M4 output-type bug

`negotiateRgb32Output` (Source Reader → RGB32 via `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` + explicit `MF_MT_DEFAULT_STRIDE`) was defined but **never called from `open()`** — the reader yielded compressed samples at the native type and `copySampleToFrame` memcpy'd past the small buffer (SIGSEGV in the real-file test). Now called after metadata extraction. (This is the same class of bug the M5-preview harness documented: the decoder's own converter rejects RGB32; the Video Processor MFT is the documented path.)

## Verified live (this machine, Debug + Release)

| Check | Result |
|---|---|
| Open + metadata | 2560×1440 @ 60.00 fps, 30038 ms, codec H.264, 8-bit, audio=yes (audio never initialized — video stream only) |
| Frames advance | pause at 2133/2566/5100 ms — position tracks real decode progress |
| Pause/resume | resume continues from the **same** position (fresh worker + queue, reader kept) |
| Stop | worker joins cleanly, app stays responsive |
| EOS | "video stream ended — stopping playback", last frame stays, no crash |
| Corrupt file | `cannot open video … 0xC00D36C4`, app keeps running (checkerboard) |
| Tests | 61/61 cases, 2440 assertions, both configs, 0 warnings under /WX |
| Debug layer | ON in Debug, OFF in Release (correct) |

## Performance note (expected, not a defect)

Software decode of 1440p60 H.264 → RGB32 (Video Processor MFT) runs at roughly **half real time** (~30 s of content in ~80 s wall). That's the M4 CPU path — M5's hardware MFT + GPU surfaces is the fix, and M6's scheduler will stop re-presenting the same frame while waiting.

---

# M5 — Hardware decoding + GPU color (2026-08-17)

## The big finding: this machine has no working hardware decode path in Media Foundation

Follow-up forensics (`probe_dxva.cpp`) walked the full chain — MFT registration → driver capability → direct decoder drive → codec cross-check. The verdict from M5 stands (**systemic MF-stack limitation, not a code bug**), but two M5 claims are now **corrected** (see table): a hardware MFT *is* registered (the earlier probe's input-type filter excluded it), and the drivers *can* create decoders — the refusal happens one level up, inside the MS decoder MFT itself.

| Level | Probe | Result |
|---|---|---|
| MFT registration | `MFTEnumEx(MFT_ENUM_FLAG_ALL)` video decoders | 20 MFTs: `AMDhwDecoder` (HW, hwurl) + AMD/NVIDIA MJPEG (HW) + MS H.264 (SYNC) etc. **CORRECTION**: "0 hardware MFTs" was a filter artifact — `AMDhwDecoder` exposes no types through enumeration, so it never matched the input-type filter |
| `AMDhwDecoder` (the HW MFT) | CoCreateInstance + SET_D3D_MANAGER + type queries | **useless**: async MFT — SET_D3D_MANAGER → `MF_E_TRANSFORM_ASYNC_LOCKED` (0xC00D6D77); exposes **zero** input/output types even after the manager message. Never selectable by the Source Reader |
| Driver (both GPUs) | `CheckVideoDecoderFormat` (H264-VLD-NOFGT, NV12) | **1 on BOTH** (RTX 3050 + Radeon iGPU) |
| Driver (both GPUs) | `CreateVideoDecoder` with a REAL config from `GetVideoDecoderConfig` | **`S_OK` on BOTH** (AMD: 11 configs, NVIDIA: 3). The earlier `E_INVALIDARG` was an incomplete desc (no `SampleWidth/Height`) — the drivers are fully functional |
| MS H.264 decoder MFT | `MF_SA_D3D11_AWARE`, SET_D3D_MANAGER, NV12 output, direct ProcessInput/Output | aware=1; manager accepted; NV12 S_OK; `streamInfo.flags=0x107` (**PROVIDES_SAMPLES** — DXVA-style mode entered); decodes real frames — **but every output buffer is system memory** (`MFGetService(MR_VIDEO_ACCELERATION_SERVICE)` → `E_NOINTERFACE`) |
| Input type completeness | native type from real 1080p clip | **complete**: 1920×1080, 30fps, `mpeg2Profile=100` (High) — not the placeholder-type path (placeholder → `MF_E_TRANSFORM_STREAM_CHANGE`, which we handled and it still fell back) |
| Adapter | same drive on adapter 0 (AMD iGPU) vs 1 (NVIDIA dGPU) | **identical system-memory output** |
| Resolution | 1080p vs 1440p clip | identical (not the 1920×1088 DXVA-guarantee threshold) |
| Codec | Source Reader NV12 probe on H.264 AND HEVC clips | **both negotiate NV12 → both system memory** — systemic, not codec-specific |

**Root cause (established):** the MS H.264/HEVC decoder MFTs accept the DXGI manager, enter DXVA-style output mode (`PROVIDES_SAMPLES`), and then **silently allocate system-memory NV12** for every frame. Their internal DXVA handoff refuses on this machine even though every public prerequisite is satisfiable: drivers advertise the mode *and* create decoders, the decoder claims D3D11-awareness, the manager message succeeds, the input type is complete. This is an internal decoder/driver interaction failure not observable through the public API — and it is why M5's runtime probe + clean software fallback is the correct engineering answer, not a workaround.

## M5 design response: runtime probe + clean rebuild fallback

- `open()` tries hardware first. After NV12 negotiation it reads **one sample** and requires a DXGI buffer (`IMFGetService` on the media buffer — not the crashing `GetServiceForStream` route, see SDK gotchas).
- System-memory sample → discard the NV12-committed reader entirely and rebuild via the proven M4 RGB32 path. Re-negotiating RGB32 on the same reader fails with **`MF_E_INVALIDTYPE`** (0xC00D36B4, probed), so a rebuild is required — a same-reader re-negotiation would have been a silent dead end.
- On this machine the app logs `hardware decode unavailable (hardware probe: decoder produced system-memory samples (no hardware MFT active)); retrying with the software RGB32 path` and plays normally.

## SDK 26100 gotchas (new this milestone)

- `MR_VIDEO_ACCELERATION_SERVICE`: `DEFINE_GUID`'d in `evr.h`, exported by **no** SDK import lib → `initguid.h` before `evr.h` materializes it in the TU.
- `MF_TRANSFORM_ATTRIBUTE_MFT_TRANSFORM_CLSID` is named `MFT_TRANSFORM_CLSID_Attribute` in this SDK.
- `GetServiceForStream(MR_VIDEO_ACCELERATION_SERVICE)` **hard-crashes** inside mfreadwrite (both HW and SW paths probed) → use the documented `IMFGetService` route (QI the reader).
- `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` combined with the D3D manager attributes → reader creation fails `E_INVALIDARG` (probed). VP flag is software-path-only.

## Fixes along the way

- **`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` missing from the device** → MF hardware open SIGSEGV'd inside mfreadwrite. Added to `D3D11DeviceManager` (and the test's device).
- **`detectDecoder` used the member `reader_` (null during open)** → `QueryInterface` on null → SIGSEGV. Now takes the local reader.
- **Order bug in the app**: `player_->setD3DDevice(...)` was called before `player_` was created → null deref. Fixed.

## Verified live (this machine, Debug + Release)

| Check | Result |
|---|---|
| Hardware probe | correctly detects system-memory samples → clean fallback, no crash |
| Decoder honesty | `decoder: software (RGB32 output)` logged (never fabricated) |
| Playback | video plays through the fallback path, texture uploads advancing |
| Pause/resume/stop | pause 2983 ms → resume from **same** position → stop clean (registered messages) |
| Tests | 62/62 cases, 2445 assertions, both configs, 0 warnings under /WX |
| Debug layer | ON in Debug, OFF in Release (correct) |

---

# M5 review fixes — scaling & YUV color correctness (2026-08-17)

Post-M5 review of the renderer/GPU-frame path (the M5 diff, fresh eyes). The review found the scaling feature was **broken and entirely unexercised**: the software path forced identity mapping, the hardware path's math was wrong, and nothing tested either. The GPU path can't run on this machine, so the fix also wires scaling into the **software** path — making the feature real and live-verifiable here.

## The scaling math was wrong (verified at pixel level, then unit-tested)

The shader maps window UV → texture UV via `texUv = uv * scale + offset`; the CPU computed `scale/offset` per mode in `D3D11Renderer::setVideoPlanes` with **inverted aspect ratios**:

| Mode | Intent | Code before | Verdict |
|---|---|---|---|
| Fill (default) | cover: fills the window, crops overflow | `sy = winAspect/vidAspect` (window wider) / `sx = vidAspect/winAspect` (video wider) | **inverted** — the reciprocal of the correct ratio |
| Fit | contain: whole frame visible, letterboxed | same inversion | **inverted** — cropped instead of letterboxing |
| Center | 1:1 native pixels | used aspect ratios, not pixel dims | **wrong** — behaved like Fit |
| Stretch | full frame, aspect ignored | identity | ✓ correct |

E.g. window 1920×1080 + video 640×480: Fill should show the video at 1920×1440 cropping 180 px top/bottom (`sy = 0.75, oy = 0.125`); the code produced `sy = 1.333..` (zoom). Fit should letterbox to 1440×1080 (`sx = 1.333.., ox = -0.166..`); the code produced `sx = 0.75` (crop). **The default mode (Fill) was broken.** The math is now a pure, header-only `ScaleMath.h` (`computeScaleOffset`, no D3D deps) with **7 unit tests** (`tests/test_scale.cpp`, pixel-verified expected values for both orientations of every mode + the zero-size guard).

## Letterbox margins must sample black, not the video edge

With the corrected Fit/Center math, `scale > 1` samples **outside** `[0,1]` in the letterbox margins. The CLAMP sampler smeared the video's edge texel column/row across the bar; the renderer now has a second **BORDER (opaque black) sampler** selected for Fit/Center (`scalingNeedsBorder`). Fill/Stretch never leave `[0,1]` (verified in the tests' math), so they keep CLAMP.

## Software path now honors scaling (was forced identity)

`setVideoTexture` gained `(videoWidth, videoHeight, scaling)` and computes the same UV mapping as the hardware path; `PSMainTexture` applies `uv*scale + offset`. Before, the config's scaling mode silently did nothing on the only path that runs on this machine.

## YUV range: decoders output LIMITED range — the matrix assumed full

H.264/HEVC decode output is **limited range** (luma 16–235, chroma 16–240; `MFVideoNominalRange` defaults to limited when unset). The shader applied the BT.709 matrix directly to `[0,1]`-normalized samples, which would wash out blacks/whites on any machine where the GPU path runs. `PSMainYuv` now rescales **limited → full** before the matrix (`y = (y − 16/255)·(255/219)`, chroma `(uv − 128/255)·(255/224)`); the same constants fit 10-bit P010 (64–940) within 0.02%.

## Verified

- **Tests: 69/69 (7 new), 2493 assertions, Debug + Release, 0 warnings under /WX.**
- **Live (software path, this machine):** harness `--video … --scaling center|fit` runs clean (90/60 frames); Center on the 2560×1440 clip shows the 1:1 center crop vs Fill — the mapping is real now. App: hardware probe → software fallback → frames flowing, clean stop.
- Deferred (noted for M13): `bindGpuFrame` creates 2 SRVs per decoded frame — cache per decoder texture when the hardware path is exercised on a working machine.

---

# M6 Build Notes — frame timing, queue & scheduling (2026-08-17)

M6 exit criteria met: source-FPS pacing verified live, drops ≈ 0, pause → CPU near-zero, no busy loop, FrameQueue unit tests green.

## What shipped

- **`src/playback/FrameScheduler`** — pure pacing math over the QPC 100 ns clock (no threads, no Win32 waits): source-FPS interval, a media↔wall anchor (`dueMediaAt(now) = anchorMedia + (now − anchorWall)`), deadline advance derived from the **presented frame's media timestamp** (exact even for early/late presents), and idle re-arms that snap forward so a stale deadline never busy re-fires. Unit-tested.
- **`src/playback/PlaybackController`** — owns `VideoPlayer` + `FrameScheduler` + a high-resolution **waitable timer** + the per-session stats. `onWake()` (called on timer OR new-frame events) drains the queue per the scheduler and returns the frame to present (or nothing — no redraw of static frames). Pause cancels the timer, stops the decoder, preserves position; resume re-anchors and seeks to the saved position. `config.playback.frameQueue` is now wired as the queue capacity.
- **`FrameQueue` finalization** — manual-reset **new-frame event** (signaled on push — the loop's "wake on new frame"), `popNewestUpTo(due)` (pops the NEWEST frame whose media timestamp is due, dropping stale ones into a `droppedFrames` counter; the EOS sentinel is delivered immediately), event reset on clear/close.
- **Message loop restructure** — the app no longer uses a fixed 16 ms `SetTimer`. It waits on `MsgWaitForMultipleObjects({waitable timer, new-frame event}, QS_ALLINPUT)` while playing (messages only otherwise): **zero busy-wait**, deadline-precise, and the slow-decoder path presents each frame as soon as it arrives (new-frame wake) instead of waiting for the next deadline.
- **Stats (M6 collection; M9 `StatsCollector` subscribes)** — decodedFps / presentedFps / droppedFrames / decodeLatencyMs / renderTimeMs, recomputed once per second, DEBUG-logged every 5 s and summarized at pause/stop (INFO). `DecoderManager` counts produced frames; `DecodedFrame.decodeTime100ns` stamps decode wall-time for latency.

## Design decision: rendering stays on the UI thread

The docs describe a dedicated render worker thread waiting on a waitable timer + events. For M6 the single-threaded design was kept (M2 review note): the UI thread's message loop already blocks on the waitable timer + new-frame event + messages, which satisfies the zero-busy-wait policy and the M6 exit criteria. A separate render thread (with the renderer thread-safety guard the M2 review flagged) is revisited at **M8** when multi-session presentation lands.

## Verified live (this machine, Debug + Release)

| Check | Result |
|---|---|
| Source-FPS pacing | `decoded 36.4 fps ≈ presented 36.4 fps` on the 60 fps 1440p clip — decode-limited (software ≈ 0.5×), and **not** 144 Hz; static frames are not redrawn |
| Drops | **0** in steady state (freshness drops only on decode catch-up bursts) |
| Pause → CPU | **0.00 CPU-s per 8 s** (thread-level sample, pause confirmed in log first) — decode worker joined, timer cancelled, loop blocks on messages |
| Position preserved | `paused at 4716 ms` → `started (position 4716 ms)` (resume seeks to the exact saved position — the controller keeps `VideoPlayer::position` in sync via `setPosition` on every present) |
| EOS | clean stop + `playback session: … 0 frame(s) dropped, … N frame(s) presented` summary |
| Tests | **80/80 cases, 2652 assertions** (Debug) / 2633 (Release), 0 warnings under /WX; 7 scheduler + 4 queue + 1 controller integration tests added |

## Gotchas (new)

1. **Test-design deadlock found while testing**: a consumer loop that can run ahead of the producer (200 fast `yield()` iterations before the producer thread even started) let the producer fill the capacity-3 queue and block forever with no consumer. The production loop never has this problem — it always waits on the new-frame event/timer — but the test now explicitly waits for the producer. (A doctest filter quirk also fought back: names containing `/` + wildcards don't match in this doctest version — use exact names or narrower patterns.)
2. **FrameScheduler cadence bug caught by unit tests**: advancing the arming deadline from the previous *deadline* (not the presented frame's timestamp) doubled the first interval when the first frame presented early (ahead of its deadline). Fixed: `advanceAfterPresent(now, frameTimestamp)` derives the next deadline from the frame's media timestamp through the anchor.
3. **Pause position double-logged**: `VideoPlayer::pause` and `PlaybackController::pause` both logged "playback paused at …" — removed the player's duplicate (the controller is the integration owner).
4. **Probe staleness bit me mid-verification**: a reused `probe_pause.exe` binary (12 s hold) made a STOP message look 9 s late and a CPU sample look busy — rebuild scratch probes before trusting their timing.

---

# M6 review fixes (2026-08-17, before M7)

Post-M6 review of the playback/pacing diff with fresh eyes. Three fixes:

1. **`PlaybackController::newFrameEvent()` was not null-safe** — it dereferenced `player_->queue()` unconditionally, which crashes if the queue is null (paused state). The app loop guarded on state first, but the method now returns `nullptr` when the queue is absent. The message loop additionally skips the wait handles entirely if either is null (`waits[2]`, `waitCount` computed at the top of each iteration — a null handle in the array would make `MsgWaitForMultipleObjects` return `WAIT_FAILED` and busy-spin, since a failed wait leaves the message queue untouched).
2. **Nonzero initial PTS delayed the first present** — the scheduler anchored media-time-0 to `start()`, so a file whose first frame has a nonzero PTS (edit lists, trimmed starts) would show the placeholder for the PTS offset while the deadline "caught up". `onWake()` now re-anchors the timeline to the **first frame's actual PTS** once it arrives (`anchorPending_`, driven by the new `FrameQueue::peekTimestamp()`), so playback starts the moment the first frame is ready. Resume keeps the old behavior (saved position is already due).
3. **`FrameQueue::peekTimestamp()` added** — returns the front frame's media timestamp (nullopt when empty or the front is the EOS sentinel), with a dedicated unit test covering empty / nonzero-PTS / pop-advance / EOS-front / clear cases.

Verified: **81/81 tests, 2653 assertions, Debug + Release, 0 warnings under /WX**; live smoke — app opens the 1440p60 clip and plays cleanly with the review build.

---

# Video was rendered UPSIDE DOWN — root cause + fix (2026-08-17)

User report: "the video it plays is not fit screen and upside down". Investigation with a pixel-level readback probe (renders a real decoded frame through the actual renderer, reads the back buffer back, compares against the CPU frame under both orientation hypotheses) found the video was **vertically flipped** on screen.

## Root cause: the vertex-less triangle's UV convention vs D3D11 texture convention

`VSMain` maps the fullscreen triangle such that **screen-top ↔ i.uv.y = 1** (the D3D viewport transform maps NDC +y to the top of the render target). D3D11 textures, unlike OpenGL, have **v = 0 at the TOP row** — so sampling `v = uv.y*sy + oy` directly puts the video's **bottom row at the top of the screen**. Every path shared the bug; it went unnoticed because the placeholder gradient is ambiguous and the M3 checkerboard is **even-cell-symmetric** (a vertical flip is visually identical). The first real video exposed it.

## The fix (shader, both paths)

`PSMainTexture` and `PSMainYuv` now flip v when mapping window UV → texture UV:

```hlsl
float2 texUv = float2(i.uv.x * scaleOffset.x + scaleOffset.z,
                      1.0 - i.uv.y * scaleOffset.y - scaleOffset.w);
```

Identity (Fill + matching aspect): screen top → v = 0 (video top). Cropped Fill / Center / Fit: the centered band still shows, top-to-bottom (the flip composes with the scale/offset offsets correctly). `ScaleMath.h` is unchanged — it computes the window→texture mapping in the unflipped convention; the v-flip is the D3D11 texture-convention step, documented in the shader.

## Verified (readback probe, pixel-exact)

| Case | MAE upright | MAE flipped | Verdict |
|---|---|---|---|
| Identity Fill (800×450, 16:9) | **14.4** | 172.3 | UPRIGHT (was flipped: 12.6/172.3) |
| Fill crop (800×600, 4:3) | **14.6** | 166.9 | UPRIGHT |
| Center 1:1 (16:9, letterbox offsets) | **15.6** | 142.1 | UPRIGHT |

(MAE ≈ 14 is bilinear filtering vs the probe's nearest-neighbor expectation — the match is exact to sampling; the 172-vs-14 gap is decisive.) The probe ALSO verified the scaling math end-to-end: Fill crops the correct axis (`sx=0.75, ox=0.125` on a 4:3 window), so the video fills the window edge-to-edge — the user's "not fit" impression was the flip's visible artifact (wrong half of the frame on screen), not a scaling bug. Live desktop sample after the fix: varied video content at all screen edges, no bars, no clear-color background, video playing via the expected software fallback. **81/81 tests, Debug + Release, 0 warnings.**

---

# StatsCollector wiring (2026-08-17, pulled forward from M9)

The M6-collected playback stats now flow into a `StatsCollector` telemetry snapshot instead of waiting for M9.

## What shipped

- **`src/app/UiContract.h`** (namespace `vw::ui`) — the shared UI contract header per spec §10.12/§10.13, currently defining `TelemetrySnapshot` exactly as specced (workload fields `cpuUsage/gpuUsage/gpuMemory*/systemMemoryUsed` + playback fields `decodedFps/presentedFps/droppedFrames/decodeLatencyMs/renderTimeMs/hardwareDecode` + `perMonitor` vector). Windows-free by design; the rest of the contract (commands, sink, notifications, `UiSnapshot`) lands with M11.
- **`src/performance/StatsCollector`** — thread-safe aggregator holding the latest `TelemetrySnapshot`; `updatePlayback(...)` fills the playback fields, `updatePerMonitor(...)` replace-or-appends per monitor, `snapshot()` returns a consistent copy, `reset()` clears. DEBUG-logged `telemetry:` line at ~1 Hz (the exact stream the M11 UI will render).
- **`PlaybackController::setStatsObserver`** — invoked after each per-second stats recompute (~1 Hz while Playing; not while paused/stopped — values freeze). Copy-only, never blocks.
- **App wiring** — `ApplicationController` owns a `StatsCollector`, resets it per playback session, and feeds it from the observer (playback fields + the wallpaper monitor's per-monitor detail).

## Verified

- **7 new unit tests** (defaults, field mapping, overwrite, per-monitor append/replace-by-id, snapshot copy independence, reset, concurrent updates/reads) → **88/88 tests** (28720 Debug / 344983 Release assertions), both configs, 0 warnings under /WX.
- **Live (Debug build)**: `telemetry: decoded 36.0 fps, presented 36.0 fps, 0 dropped, decodeLatency 0.1 ms, render 0.6 ms, hardwareDecode=no` at ~1 Hz, matching `PlaybackController`'s `stats:` line — end-to-end flow confirmed.
- **Workload fields remain 0** until M9's `WorkloadMonitor` fills them (documented in the header); per-monitor detail is single-monitor for now (M8 adds real per-monitor sessions).

---

## M7 — Playlist engine

**Committed as `90b03e8`** — playlist items/modes/persistence + loop-replay + broken-item skip.

## What shipped

- **`src/playlist/PlaylistManager`** — pure playlist policy (docs/03 §3.9): items + ops (add/remove/move/replace/clear), modes `Single | Sequential | Loop | Shuffle`, per-item `enabled`/start/end, `markUnavailable` (runtime-only; recovery later — M12), metadata cache (`updateCachedMetadata`), and `nextIndex()`/`previousIndex()` navigation. Shuffle = Fisher–Yates permutation persisted as `shuffleOrder`; **no immediate repeat** of the item just played (`shuffledPermutation` avoids `current` at position 0 when n>1); the order is **regenerated per cycle** on looped wrap (docs/03 §3.9) — the earlier "reuse forever" behavior was caught by the unit tests and fixed.
- **`src/playlist/PlaylistStore`** — JSON persistence (D-06, the project's strict parser, no third-party dep): `{version, mode, loop, current, shuffleOrder, items[path, start/end100ns, enabled, duration100ns, width, height, codec]}`. Saved ONLY on transition/shutdown/meaningful change (never every second). Unknown keys ignored, wrong-typed values fall back to defaults; a file with a NEWER version is treated as corrupt (never guess) → caller seeds defaults. Atomic save (temp + rename); corrupt/invalid-UTF8 load → nullopt.
- **Loop same video (docs §34): `VideoPlayer::replay()` + `PlaybackController::replay()`** — at EOS for the SAME item (Single self-loop / 1-item loop), the reader + decoder + GPU resources are REUSED: no reopen, no hardware re-probe, no source-reader churn. `DecoderManager::start()` now **always seeks** the reader (initial 0, resume position, and replay 0 after the reader was left at EOS — skipping the seek would make a replay immediately hit EOS again).
- **App wiring** — `ApplicationController` owns the playlist: loads from AppData (seeds from `config.playback` mode/loop + `videoPath` on first run/corrupt), advances on EOS per mode, marks broken items unavailable and advances (bounded loop: every non-returning iteration marks one item unavailable), caches real metadata at each transition, saves at shutdown.

## Verified live (this machine, Debug + Release)

- **Multi-item loop cycle**: 3-item playlist, EOS → next item in **~80–150 ms open/start** (gap between session end and next present ~50–60 ms); full cycle 0→1→2→**wrap to 0**; transitions logged with open cost (`transition to playlist item N ... in X ms`).
- **Metadata cache persisted**: after one pass, `playlist.json` holds real duration/width/height/codec for all items (loaded instantly on later runs, no decode).
- **Same-item loop (replay path)**: 1-item playlist loops continuously — `decode end of stream → playback looping (position reset)` with the reader reused; steady-state cycles every ~16 s (decode-limited software path) with no reopen and no re-probe.
- **Broken-item skip**: nonexistent file marked unavailable (`MFCreateSourceReaderFromURL failed: 0x80070002`), the loop advanced to the next playable item in ~40 ms, and the broken item was skipped on every subsequent cycle (0→2→0→2…).
- **109/109 tests, 0 warnings under /WX, both configs** (26 playlist tests: ops, modes, shuffle permutation/no-immediate-repeat/regenerate-on-wrap/no-loop-stop, persistence round-trip, corrupt recovery, adoption).

## M7 review fixes (2026-08-17, before M8)

Fresh-eyes review of the M7 commit found four issues (all fixed + regression-tested):

1. **`shuffledPermutation(n=0)` crashed on an empty Shuffle playlist** — `for (size_t i = n - 1; ...)` underflows `i` to `SIZE_MAX` and dereferences `order[i]` out of bounds. Reachable in the app: `config.playback.mode=shuffle` with no `videoPath` → empty playlist in Shuffle mode → `nextIndex()` → regenerate. Now returns an empty order for n=0.
2. **`nextIndex()`'s stale-order branch returned a non-playable item** — when current was missing from the (defensive) stale shuffle order, it returned `shuffleOrder.front()` without an `isPlayable` check; now scans for the first playable item (same policy as `shuffleForward`'s wrap branch).
3. **`current: kNoIndex` broke the store round-trip** — `size_t(-1)` cast to double (1.84e19) and back through `int64_t` on load was an out-of-range cast (UB) → garbage current (e.g. after `remove()` of the current item). kNoIndex is now serialized as `-1` and negative reads clamp to kNoIndex.
4. **`replace()` left stale cached metadata** — swapping a different file behind the same index kept the old duration/width/height/codec until the next real open; now clears the cached fields.

**113/113 tests** (30558 Debug / 351045 Release assertions), both configs, 0 warnings under /WX; live smoke (seed-from-config path) clean.

---

## Real bug caught live (and why the unit tests didn't)

**`VideoPlayer::replay()` hung at EOS — use-after-free on the frame queue.** The original order was `queue_->close(); queue_.reset(); decoder_.stop();` — but `DecoderManager` holds a raw pointer to that same `FrameQueue` and calls `queue_->close()` inside `stop()`. Resetting the queue first left `stop()` closing a **destroyed** queue (freed mutex/condvar) → the app thread hung silently right after `decode end of stream` (no crash, no log). The multi-item path never hit it (transitions go through `open()`, which tears down cleanly); only the same-item loop exercised replay. Instrumented with probe logs → bisected to the replay chain → fixed by stopping the decoder BEFORE dropping the queue (matching `pause()`'s order). **All live-verified paths (multi-item, same-item loop, broken-skip) now confirmed end-to-end.**

---

## RENDER BUG FIX (2026-08-17, user report "the video is being cropped")

The wallpaper HOST WINDOW was physically **25% oversized** — 2400×1350 on a 1920×1080 screen at 125% DPI scaling. The screen showed only the top-left corner of the render: the video looked zoomed-in with the bottom/right cut off (the user's "video is cropped" report). The scaling MODE was never the problem (Fill = crop-to-aspect + scale to fill, no bars, no distortion — exactly the user's stated spec).

**Root cause:** `WallpaperHost::scaleToParentDpi()` scaled the physical monitor bounds by `parentDpi/96`. That conversion is only correct when the parent lives in a DIFFERENT DPI context than the caller. Both the app (`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`) and Explorer's WorkerW are per-monitor DPI aware, so the child-window rect is already in **physical pixels** — the factor (120/96 = 1.25 at 125% scaling) inflated the window. Undetected since M3: the earlier "empirical" note was taken at 96 DPI where the factor is identity.

**Why earlier probes missed it:** the first window-rect probes were DPI-UNAWARE PowerShell, which silently virtualizes coordinates (reported 1920×1080 for the 2400×1350 window). A probe that first calls `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` returns true physical rects. **Lesson: always measure window rects from a DPI-aware probe.**

**The fix:** `scaleToParentDpi()` now returns the physical bounds unchanged (both sides of the parent are DPI-aware). Verified with a DPI-aware probe: host rect 2400×1350 → **1920×1080** = the physical screen. Pixel-sampled the rendered host window:
- 16:9 clip (Eula): fills the whole screen, zero crop, no bars.
- 21:9 ultrawide clip (Miyabi): crops evenly on both sides (sx=0.75) to 16:9, scales to fill 1920×1080 — no bars, no distortion.

**113/113 tests, Debug + Release, 0 warnings** (one-line DPI mapping; scale math already unit-tested). Also cleaned up a leftover M7-test playlist.json that had pointed the app at an ultrawide clip.

---

## M8 — Multi-monitor & multi-GPU (single-display scope)

**Committed as `c394ff5`** — per spec §6/§82/§125: real multi-monitor/hot-plug is **NOT MEASURED on this machine** (one display); the substitute is simulated-topology unit tests + single-monitor e2e. Multi-GPU adapter association is implemented + unit-tested; presentation locality is per-host vsync-blocked swap chains.

## What shipped

- **`MonitorManager`** (docs/03 §3.10):
  - **Pure diff extracted**: `diffMonitorSets(prev, current)` — a windowing-free helper that computes added/removed/changed keyed by stable id, so **simulated topologies are unit-testable** (the real hot-plug substitute). `refresh()` and a new `setSnapshotForTest()` test hook both use it. The diff now also detects **work-area changes** (taskbar moved) in addition to move/resize/refresh/primary.
  - **Adapter association**: `associateAdapters()` matches each monitor's bounds center to a DXGI output's `DesktopCoordinates` and fills `adapterIndex`/`adapterLuid` (multi-GPU locality). Pure over supplied adapter/output lists — unit-tested with a hybrid-GPU layout (integrated drives DISPLAY1, discrete drives DISPLAY2) and an unmatched-monitor fallback (adapter 0, zero LUID). `AdapterInfo` now carries the DXGI `luid`.
- **`WallpaperManager` per-monitor routing** (Independent mode): new `setVideoFrameFor(monitorId, frame)` uploads into a **per-monitor texture** and rebinds that host only (plus `bindGpuFrameFor` for hardware surfaces); `setVideoFrame` remains the Clone broadcast. Per-monitor textures are dropped on host removal/teardown (no leaks). Upload code factored into shared helpers.
- **Config**: `wallpaper.mode` = `independent` (default, spec §125) | `clone`; persisted under the `wallpaper` JSON section; unknown names fall back to independent.
- **App session layer**: `onFrameWake` routes frames per mode — Clone broadcasts to all hosts, Independent targets the primary monitor's host (`primaryMonitorId()`). Startup logs the mode + decoder-count diagnostic (Clone = decode-once for all displays; Independent = one per display).

## Verified

- **8 new unit tests** (6 monitor simulated topologies: empty diff, add/remove by id, move/resize/refresh/primary/work-area change, HMONITOR-not-a-change, refresh event sequences, adapter association happy + fallback; 1 config default/round-trip/unknown; 1 config clone clamp) → **120/120 tests** (31612 Debug / 308342 Release assertions), both configs, 0 warnings under /WX.
- **Live (single display, both modes)**: `wallpaper mode: independent (1 monitor(s), decoder count: 1 — one per display)` and `wallpaper mode: clone (1 monitor(s), decoder count: 1 — decode-once for all displays)` — both play the Eula clip cleanly with telemetry flowing; host window verified 1920×1080 physical.
- **NOT MEASURED (recorded for M14)**: real connect/disconnect hot-plug, mixed-refresh per-monitor pacing, N-monitor independent fan-out, true multi-GPU decode locality — single-display dev machine; simulated topologies + single-monitor e2e are the substitute. Per-monitor presentation is architecturally per-host vsync-blocked swap chains (each present syncs to its own monitor).

### M8 review fix (2026-08-17, before the SAR change): `associateAdapters` first-match bug

`associateAdapters` broke only the **inner** (output) loop on a match, so a **later adapter's overlapping output could overwrite a correct association** (last-writer-wins). Real on virtual-display / surround layouts where outputs can share desktop coordinates — and invisible to the original tests (they only used non-overlapping outputs). Fixed with a `matched` flag that stops the outer loop at the first match; **1 new regression test** (identical overlapping output rects on two adapters → adapter 0 + its LUID stick) → **123/123 tests**, Debug + Release, 0 warnings under /WX.

---

## UNIVERSAL CROP/SCALE RULE — anamorphic (SAR) correction (2026-08-17, user: "make the crop and scale rule more universal")

**Committed as `f5e33b2`** — the crop/scale rule (crop evenly to the target aspect, scale to fill, no bars, no distortion) was already universal in one sense: `ScaleMath.h` computes against the **window's** aspect ratio, never a hardcoded 16:9. The gap was **sample aspect ratio (SAR)**: the renderer consumed the raw pixel dims, so anamorphic content (non-square pixels — e.g. 720×480 DVD, SAR 10:11) cropped/scaled to the wrong aspect and distorted.

## What shipped

- **`VideoMetadata`**: reads `MF_MT_PIXEL_ASPECT_RATIO` (packed UINT64; `MFGetAttributeRatio` unpacks it) into `sarNum`/`sarDen` (default 1:1), and exposes `displayAspect = (width·sarNum)/(height·sarDen)` — the aspect the scaling math must consume.
- **`DecodedFrame.displayAspect`**: carried on every frame (constant per stream, copied from the metadata in the decode worker). 0 = unknown → callers fall back to the raw pixel aspect.
- **`ScaleMath.h`**: new `videoAspectFor(w, h, displayAspect)` (SAR-corrected aspect, raw-pixel fallback, garbage-guarded) + an aspect-based `computeScaleOffset(winW, winH, vidAspect, mode)` overload. The width/height overload now delegates (Center keeps its native-pixel meaning via a dedicated branch).
- **`D3D11Renderer`/`WallpaperHost`/`WallpaperManager`/harness**: the video setters now take the display aspect instead of width/height — the renderer only used those for the aspect anyway. `WallpaperManager` computes it per frame with `videoAspectFor` (clone path caches it for the rebind).
- **`VideoPlayer::open` log** now prints `SAR n:d, display aspect x.xxxx` so the metadata path is verifiable on real files.

## Verified

- **3 new unit tests** (videoAspectFor prefers SAR / fallbacks / guards; anamorphic Fill crops to the DISPLAY aspect with even offsets; Fit letterboxes the corrected aspect) → **122/122 tests**, Debug + Release, 0 warnings under /WX. The anamorphic test proves the old behavior cropped the wrong axis (visible 0.84375 vs 0.75 of the height) — the distortion this fixes.
- **Live**: Eula clip opens with `SAR 1:1, display aspect 1.7778` (16:9 = identity under Fill, still edge-to-edge). No anamorphic clip exists on this machine to pixel-verify live; the unit tests cover the non-square-pixel math.
- Behavior is unchanged for square-pixel content (all current clips): SAR 1:1 → display aspect == pixel aspect.

---

## Toolchain note (cost this investigation real time)

Scratch-probe builds from bash hit a confusing wall: `cl` from the hardcoded 14.44 path **ignored `/std:c++23`** (D9002) and `std::expected` never resolved. Two compounding factors: (1) this cl's named modes are `c++14|c++17|c++20|c++latest` — **no `c++23`**; CMake 4.4.2's C++23 maps to **`stdcpplatest`** in the vcxproj, so the project builds with `/std:c++latest`, and the M1 "c++23 confirmed" note was wrong; (2) MSYS2 argument conversion mangles `/nologo`-style flags (turned into `C:\Program Files\Git\nologo`) unless `MSYS2_ARG_CONV_EXCL='*'` is set. Scratch probes should compile with `/std:c++latest` + `MSYS2_ARG_CONV_EXCL='*'`, or better, through CMake.

---
