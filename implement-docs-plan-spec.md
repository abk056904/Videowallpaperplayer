# Implement the Plan at `docs/` — Spec

**Request:** Implement the video-wallpaper-engine plan in `docs/` — build the full v1 application (native Windows 11, C++23, Win32 + Direct3D 11 + Media Foundation) per the plan documents, on this machine, following the workflow agreed in the interview.

**Status:** Spec complete (2026-08-17). No code written yet.

---

## 1. Source of truth

| Doc | Role |
|---|---|
| `docs/01-requirements.md` | Consolidated requirements (functional, non-functional, prohibitions) |
| `docs/02-architecture.md` | Architecture: modules, threading, resource state machine, frame pipeline, WorkerW hosting |
| `docs/03-implementation-plan.md` | Milestone plan M0–M14 with tasks, files, APIs, exit criteria; v1 vs v2 scope (§3.0) |
| `docs/04-testing-profiling.md` | Test matrix, profiling methodology, resource budgets, acceptance checklist |
| `docs/05-decisions-risks.md` | Decisions D-01…D-18, risk register R-01…R-14, resolved open questions |
| `docs/06-progress-checklist.md` | Live milestone tracker (M0 ✅ done, M1–M14 pending) |
| `BUILD_NOTES.md` | M0 environment audit: toolchain, GPUs, test videos, codec probe |
| `New Text Document*.txt` (→ to be moved to `docs/sources/`) | Original task specs (doc 1/2/3) |

These documents are authoritative. This spec records the **execution contract** on top of them: scope target, product identity, behavior defaults, environment constraints, engineering standards, and the working agreement.

---

## 2. Goal & success criteria

**Goal:** a working, native, x64 Windows 11 video-wallpaper application built in this repository, milestone by milestone, with every milestone built, tested, documented, and committed before moving on.

**Success = the full v1 plan (M1–M14) completed**, meeting `docs/04` §4.8 acceptance checklist, with the M14 final report and resource-efficiency audit. No milestone may be skipped; exit criteria from `docs/03` must pass before advancing.

**Core engineering principle (from all three source specs):** minimum practical resource consumption — paused/invisible wallpaper ≈ zero active decode/render work; no busy loops; hardware decoding preferred; frames stay on the GPU.

---

## 3. Scope

### 3.1 In scope (v1)

Playback engine (MF + HW decode + SW fallback), D3D11 GPU frame path, frame scheduling, playlists (single/sequential/loop/shuffle), multi-monitor (Clone + Independent), resource governor + auto-pause (game/fullscreen/workload/lock/display-off/battery), tray + minimal Win32 UI (Home, Library, Playlists, Monitors, Performance, Settings), recovery (Explorer restart, device loss, decoder/file errors, config corruption), profiling + stability validation, portable ZIP packaging, README + final report.

### 3.2 Deferred (v2 — not in this effort)

Audio playback, HDR rendering (v1 detects + documents + SDR fallback only), global hotkeys, debug/performance overlay, library thumbnails / search / drag & drop, MSI/Inno installer, shared-playlist monitor mode. Full rationale: `docs/03` §3.0.

---

## 4. Environment facts (from `BUILD_NOTES.md`)

- **Machine:** Windows 11 Home (10.0.26200), AMD Ryzen 5 7535HS, 13.8 GB RAM.
- **GPUs:** NVIDIA GeForce RTX 3050 Laptop (driver 32.0.15.9597) + AMD Radeon iGPU → hardware-decode + hybrid-GPU testing feasible here.
- **Toolchain (installed + verified, M0 done):** MSVC 14.44.35207 (VS 2022 Build Tools 17.14.37), Windows SDK 10.0.26100.0 (MF/D3D11/DXGI headers+libs present), CMake 4.4.2 (`C:\Program Files\CMake\bin\cmake.exe`). Generator: `Visual Studio 17 2022`, x64. Verified via Debug+Release hello build.
- **Test videos:** `C:\Users\mbk43\Videos\bgcmp\` (10× H.264 incl. 4K/"4K60", 1× HEVC `Furina-…mp4`) and `C:\Users\mbk43\Downloads\Video\` (1080p H.264). **No AV1/VP9/HDR content on disk.**
- **ffmpeg:** not installed (see §6, ffmpeg row — install temporarily for clip synthesis, then remove).
- **Displays:** **one display only** — `\\.\DISPLAY1` 1536×864, primary. Real multi-monitor hot-plug cannot be exercised on this machine (see §6 and M8 note).
- **Git:** **not a git repository** (`git rev-parse` → fatal). The commit-per-milestone workflow requires `git init` + an initial commit as the first M1 action.
- **fxc.exe:** present at `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe` — build-time shader compilation confirmed feasible.

---

## 5. Product identity & behavior (interview answers)

| Item | Decision |
|---|---|
| **App name** | **Video Wallpaper** — exe `VideoWallpaper.exe`; per-user data in `%APPDATA%\VideoWallpaper\` (config, logs, playlists); named mutex/window classes/tray follow the same name. ⚠️ Docs currently say `%APPDATA%\WallpaperEngine` — update all path references (docs/02 §2.9, docs/03 M1 logger/config, docs/06) to `VideoWallpaper` during M1. |
| **Default monitor mode (first run)** | **Independent (per-monitor)** — each monitor gets its own wallpaper/playlist by default; Clone remains selectable. (Reverses the docs' efficiency-leaning default; efficiency is preserved because independent mode still shares device/shaders/infrastructure.) |
| **Second instance** | **Focus the existing instance** — signal the running instance to show its UI/tray and exit (consistent with D-13). |
| **Video ends, loop off** | **Loop anyway** — in single-item mode the video simply loops (treat single mode as loop of the same video). For a multi-item sequential playlist with loop off, docs §18 applies: last item ends → playback stops, last frame held. |
| **First run, no playlist** | **Solid default color** (dark) until the user picks a video — no auto-loading of sample files, no test gradient. |
| **Repo hygiene** | **Move the three original `New Text Document*.txt` specs into `docs/sources/`** early (M1 housekeeping), and update `docs/README.md` source-doc table paths accordingly. |

---

## 6. Testing constraints on this machine (interview answers)

| Constraint | Decision | Implication |
|---|---|---|
| **Disruptive desktop tests** | **Avoid** on this machine | Do **not** kill/restart `explorer.exe` and do **not** lock the screen (Win+L) to verify M3/M10/M12. Substitute: verify host-invalidity detection and recovery logic via unit tests + fault injection that doesn't touch Explorer (e.g. simulated invalidation, config corruption, file renames); the actual Explorer-restart and lock/unlock scenarios are reported **`NOT MEASURED — disruptive test declined on dev machine`** in the M14 report. |
| **24 h stability run** | **Shorter runs OK** | Replace the 24 h soak with **4–8 h runs** + aggressive leak-cycle tests (play/pause/resume/next/monitor/UI-open-close loops with RAM/VRAM/handle/thread tracking). M14 report states the reduced duration explicitly. |
| **Personal videos** | **Reference by path only** | Tests reference `C:\Users\mbk43\Videos\bgcmp\…` and `Downloads\Video\…` by absolute path. **Never copy personal videos into the repo.** No test-media folder; add a `.gitignore` guard if any media path ever needs staging. |
| **ffmpeg** | **Install temporarily, then remove** | `winget install Gyan.FFmpeg` as a **dev/testing-only** tool to (a) probe/verify codec metadata and (b) synthesize AV1/VP9/HDR test clips for M13 coverage. After the clips are produced and M13 is done, `winget uninstall` ffmpeg. ffmpeg is **never** a runtime dependency of the app. Generated clips go in a gitignored temp location or are deleted after use. |
| **Multi-monitor (M8)** | **Single display on this machine** | Only one monitor (1536×864) exists, so real hot-plug / multi-monitor rendering cannot be verified here. Substitute: unit-test `MonitorManager` events and per-monitor session logic with simulated topologies; verify single-monitor behavior end-to-end; report real multi-monitor items as **`NOT MEASURED — no second display on dev machine`** in the M14 report (revisit if a second monitor becomes available). |

---

## 7. Engineering standards (interview + decisions)

| Area | Standard |
|---|---|
| Language | **C++23 with `std::expected`** — CMake `CMAKE_CXX_STANDARD 23`; **smoke-test `/std:c++23` on MSVC 14.44 at M1** (a 5-line compile check) and fall back to `/std:c++latest` only if needed. Update the M1 CMake task (docs/03 §3.3) which says "C++20". |
| Error handling | `std::expected<T, E>` for fallible paths; no silent exception swallowing; no raw owning pointers |
| Shaders | **Build-time compilation with `fxc`** — verified present at `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe`; build-time-only dependency, no runtime shader file |
| Config | JSON, minimal in-repo parser (`src/util/json.*`) — D-06 |
| Tests | Header-only `doctest`, **vendored** as a single committed header under `tests/` — fetched once from the doctest release at M1, then committed so all future builds are offline (no FetchContent/network at configure time) — D-07 |
| Licensing | **Apache-2.0** — user requirement: "it must be full open source". Apache-2.0 is permissive (use/modify/distribute/commercialize) plus an explicit patent grant, with no copyleft obligations for this native Windows app. **Resolved — no longer an open item.** (MIT would also satisfy the requirement; GPL-3.0 is the copyleft alternative.) |
| UI | Native Win32 common controls, standard Windows 11 look, no custom theming — D-17 |
| Headers | Beside sources (`src/<module>/foo.h`) — D-01 |
| Concurrency | Small fixed thread pool; workers block (events/CVs/waitable timers); **zero busy-wait policy** — D-09, R-11 |
| COM/GPU | `Microsoft::WRL::ComPtr`, RAII, explicit ownership + lifetime map (§5.3 of docs/05) |
| Releases | Release: no D3D debug layer, INFO logging, no overlays, no diagnostic allocations |

---

## 8. Delivery workflow (interview answers)

0. **Before M1 scaffolding:** `git init` the repository and make an **initial commit of the existing files** (the three `New Text Document*.txt` specs, `docs/`, `BUILD_NOTES.md`, this spec) — the commit-per-milestone workflow requires version control, and the directory is currently **not a git repository**.
1. **Continuous implementation** toward the full v1 target — I do not pause for approval between milestones.
2. **Document and test after every milestone**: run the milestone's verify steps + unit tests (Debug **and** Release), update `docs/06-progress-checklist.md` (tick tasks, mark exit criteria, fill notes, update the status table + "current milestone"), and record any findings in `BUILD_NOTES.md`.
3. **Commit after each milestone**, but only once the milestone is properly implemented, tested, and documented (per D-18; each commit at the milestone boundary, with the checklist update included).

---

## 9. Milestone plan (current state)

**Done:** M0 (toolchain audit + install + verification; recorded in `BUILD_NOTES.md`, ticked in `docs/06`).

**Next:** M1 — build skeleton (git init + initial commit; CMake presets; `wWinMain`; hidden control window; single instance; Logger; ConfigurationManager; shutdown path; housekeeping: move txt specs to `docs/sources/`, s/WallpaperEngine/VideoWallpaper/).

Then M2 → M14 per `docs/03`; full task lists + exit criteria live in `docs/03` and `docs/06`.

Adjustments baked into the milestone execution (from this interview + review):

- **M1:** `git init` + initial commit of existing files; C++23 with `/std:c++23` smoke test; AppData folder `VideoWallpaper`; txt-spec relocation to `docs/sources/`; README doc-map path updates; write `LICENSE` (Apache-2.0 — decided).
- **M7 (playlist):** "Single" mode loops the same video when loop is off (per §5 — i.e. the `loop` setting is effectively forced on for single-item mode; it still governs playlists). Sequential + loop off still stops at the end of the last item (docs §18), holding the last frame.
- **M8 (multi-monitor):** Independent is the **default** mode on first run; Clone available but not default. Real multi-monitor/hot-plug verification is **NOT MEASURED on this machine** (single display) — substitute with unit-tested simulated topologies + single-monitor end-to-end checks; revisit if a second display appears.
- **M3/M10/M12 (verification):** Explorer-restart and lock-screen checks replaced by non-disruptive substitutes (see §6).
- **M13 (stability):** 4–8 h runs + leak cycles instead of 24 h; AV1/VP9/HDR clips synthesized with temporarily-installed ffmpeg (then removed); if clip synthesis or HW support fails, those rows are `NOT MEASURED`.
- **M14 (report):** explicitly report the reduced-duration soak, the skipped disruptive tests, and the single-display limitation, each with reasons.

### 9.1 Per-milestone acceptance mapping

Each row condenses the exit criteria from `docs/03` and maps them to the acceptance categories of `docs/04` §4.8 (Build / Playback / HW accel / Multi-monitor / Auto-pause & efficiency / Stability / UI / Reporting). Constraints specific to this machine (single display, no disruptive tests, reduced soak) are noted per milestone.

| M | Primary deliverables | Exit criteria (condensed) | Acceptance categories | Verification & constraints on this machine |
|---|---|---|---|---|
| M1 | `CMakeLists.txt`, `CMakePresets.json`, `.gitignore`, `LICENSE` (Apache-2.0), `README.md` stub; `src/app/*`, `src/logging/*`, `src/config/*`, `src/util/*`; **git init + initial commit**; txt specs → `docs/sources/` | Debug+Release x64 green; 2nd instance exits cleanly; config created on 1st run; logs rotate; clean exit | Build | Manual launch/exit + unit tests: **config (incl. JSON parser round-trip/corrupt-recovery) + logger (levels/rotation/no-spam)**; `/std:c++23` smoke test |
| M2 | `D3D11DeviceManager*`, `D3D11Renderer*`, `TextureManager*`, `shaders/VideoShader.hlsl` (placeholder) | Gradient renders @60 FPS (Debug); Release w/o debug layer; adapter/output log matches reality | Build | Manual render window + log check; device-loss stub present |
| M3 | `MonitorManager*`, `WallpaperHost*`, `WallpaperManager*` | Wallpaper behind icons; no focus/click capture; per-monitor positioning; Explorer-restart stub | Build, (Recovery) | Manual behind-icons check; **Explorer-restart verify NOT MEASURED (declined)** → host-invalidity logic unit-tested instead |
| M4 | `VideoMetadata.h`, `DecoderManager*`, `VideoPlayer*`, `FrameQueue*` | `.mp4`/`.mkv` play; metadata correct; audio never initialized; corrupt file doesn't crash | Playback | Manual with `Videos\bgcmp` H.264 files (path-only); corrupt-file fault test; **VideoMetadata unit tests** (synthetic MF media types); **container breadth (MOV/AVI/WebM)** via ffmpeg-synthesized clips (M4/M13) or explicit `NOT MEASURED — no sample` |
| M5 | HW-decode path, full `VideoShader.hlsl` (NV12/P010 + scaling), SW fallback, decoder-mode diagnostics | HW decoder reported with real vendor; no CPU copies in NV12 path; fallback clean | HW accel | GPUView + log; **P010/HDR verify deferred** until M13 clip synthesis (else NOT MEASURED) |
| M6 | `FrameScheduler*`, `PlaybackController*`, final `FrameQueue*` | presentedFps ≈ source fps; drops ≈ 0; pause → CPU/GPU near-zero | Playback, (Efficiency) | Manual + stats check; no busy-loop audit; **FrameQueue unit tests** (capacity bound, backpressure, drop-oldest, thread-safety) |
| M7 | `PlaylistManager*`, `PlaylistStore*` | Modes behave (Single loops per §5); 100+ items instant; seamless transitions; RAM flat | Playback | Playlist/persistence unit tests + manual; transition gap measured |
| M8 | Full `MonitorManager`, `WallpaperManager`, multi-session `PlaybackController` | Clone decodes once; Independent per-monitor; hot-plug; mixed refresh | Multi-monitor | **Connect/disconnect NOT MEASURED (single display)** → simulated-topology unit tests + single-monitor e2e; **resolution/refresh/primary change testable on the single display** (manual, via display settings → WM_DISPLAYCHANGE); Clone decode-once via decoder-count diagnostics |
| M9 | `WorkloadMonitor*`, `StatsCollector*`, `GameDetector*`, `FullscreenDetector*` | Hysteresis unit tests; fullscreen≠maximized; Alt+Tab no flapping; classification cached | Auto-pause | Unit tests + manual: fullscreen a non-game app (e.g. browser/video player) must pause, maximized window must NOT pause, Alt+Tab no flapping; **real-game scenarios NOT MEASURED (no games to test with)** |
| M10 | `ResourceGovernor*`, `PausePolicy*`, `SystemStateMonitor*` | Governor transition table (source spec doc 3 §95) passes; battery/long-pause release; resume | Auto-pause, Efficiency, Suspension | Pure-logic unit tests; **lock/unlock NOT MEASURED (declined)**; **display-off testable** via `powercfg` monitor-timeout (non-disruptive); **battery testable** by unplugging AC (user-assisted); suspend/resume logic code-reviewed (hibernation too disruptive) |
| M11 | `Win32UI*`, panels, `TrayController*`, `LibraryManager*` | Panels functional; tray works with UI closed; no RAM growth on open/close; incremental scan | UI | Manual + library-scan unit tests; UI-open/close leak cycle |
| M12 | Recovery paths (Explorer, device loss, decoder/file, config) | Fault injection recovers; shutdown deterministic | Stability | Manual: config corruption + file rename/delete; device-loss sequencing unit-tested; **Explorer restart NOT MEASURED (declined)**; **real driver reset NOT MEASURED (risky on laptop)** |
| M13 | Profiling + stability evidence | Resource targets (docs/04 §4.7); no leaks; stress matrix green | Efficiency, Stability | **4–8 h soak + leak cycles (not 24 h)**; ffmpeg-synthesized AV1/VP9/HDR clips (then removed); HW-dependent rows NOT MEASURED if unsupported |
| M14 | Portable ZIP, `README.md`, final report + audit | Package builds from clean checkout (**exe + system DLLs only — no runtime framework, no samples/debug artifacts**); report has measured numbers or NOT MEASURED | Reporting | Package build; report reviewed for invented numbers |

---

## 10. UI panel specifications (v1)

Native Win32, standard Windows 11 look (D-17). The UI is **strictly a thin client**: it never decodes, renders, or polls engine state — it sends commands via `ApplicationController` and reads telemetry from `StatsCollector`. Constructed lazily on first open; destroyed on close (engine keeps running; tray persists).

**v1/v2 boundary in the UI:** deferred to v2 are thumbnails, search, drag & drop, hotkeys, and the debug/performance overlay. Additionally, v1 deliberately has **no audio UI** (audio is out of scope entirely), **no HDR controls** (only a metadata flag is displayed — see §10.3), and the Monitors panel exposes **exactly two modes** (Independent/Clone — shared-playlist arrives in v2, see §10.5). Nothing in these panel specs implies a v2 feature; where a v2 feature would later touch a panel, the change is additive (e.g. a third radio option, a search box, an HDR toggle).

### 10.1 Window & shell

- Main window: resizable, default ≈ 900×600, minimum ≈ 720×480, PerMonitorV2 DPI aware.
- Navigation: a top-level **tab control** with six tabs — Home, Library, Playlists, Monitors, Performance, Settings.
- Closing the window hides to tray (per `minimizeToTray` config); `Exit` lives in the tray menu and File menu.
- Second instance → focus this window (bring to front, restore if minimized).
- Telemetry timer: ~1–2 Hz, **only while the window exists**; panels subscribe to `StatsCollector` snapshots.

Wireframe:

```text
┌─ Video Wallpaper ──────────────────────────────────────────────┐
│ File                                             (menu bar)    │
│ ┌──────┬─────────┬───────────┬──────────┬────────────┬────────┐ │
│ │ Home │ Library │ Playlists │ Monitors │ Performance│Settings│ │  ← tab
│ └──────┴─────────┴───────────┴──────────┴────────────┴────────┘ │  control
│                                                                │
│                  (active panel content)                        │
│                                                                │
├────────────────────────────────────────────────────────────────┤
│ Status bar: <selection / state summary>                        │
└────────────────────────────────────────────────────────────────┘
   default ≈900×600 · min ≈720×480 · PerMonitorV2 DPI
```

### 10.2 Home

Status readout (labels, updated at 1–2 Hz):

- Current wallpaper (file name) and playback state (PLAYING / PAUSED / SUSPENDED + active pause reasons)
- Current monitor (name, resolution, refresh rate)
- Presented FPS, dropped frames, decode latency
- Decoder mode (e.g. `NVIDIA NVDEC / hardware` or `Software fallback`) and GPU adapter name
- CPU %, GPU %, RAM used, VRAM used (from `StatsCollector`)
- Buttons: **Pause/Resume**, **Next**, **Previous** (route to `PlaybackController` via controller)

Wireframe:

```text
┌─ Home ─────────────────────────────────────────────────────────┐
│ Current wallpaper : Ganyu - Twilight Blossom ….mp4             │
│ Playback state    : PLAYING          (reasons: none)           │
│ Current monitor   : DISPLAY1  1536×864 @ 60 Hz   (primary)     │
│ Presented FPS     : 30        Dropped frames : 0               │
│ Decode latency    : 8 ms                                       │
│ Decoder           : NVIDIA NVDEC / hardware                    │
│ GPU adapter       : NVIDIA GeForce RTX 3050 Laptop             │
│ CPU: 1.2%   GPU: 4%   RAM: 180 MB   VRAM: 312 MB              │
│                                                                │
│                [ Pause ]  [ Next ]  [ Previous ]               │
└────────────────────────────────────────────────────────────────┘
   (all fields refresh at 1–2 Hz while the window is open)
```

### 10.3 Library (v1 minimal)

- **ListView** (details view) with columns: Name · Duration · Resolution · FPS · Codec · HDR · File size · (optional) Full path. Sorted by name by default; click-to-sort on columns. **The HDR column is metadata only** (detected flag: Yes/No/Unknown) — it implies no HDR rendering in v1; there are no HDR settings anywhere in the v1 UI.
- Toolbar: **Add Files…**, **Add Folder…**, **Remove**, **Set as wallpaper** (applies to the monitor selected in the Monitors panel), **Refresh**.
- Double-click a row = set as wallpaper on the selected monitor.
- Metadata is read lazily (only visible rows / on selection); folder additions register `ReadDirectoryChangesW` watches for incremental updates — **no full rescan on startup, no repeated enumeration** (10k-file folder safe).
- Selection shows a metadata summary in a status bar.

Wireframe:

```text
┌─ Library ──────────────────────────────────────────────────────┐
│ [Add Files…] [Add Folder…] [Remove] [Set as wallpaper] [Refresh]│
│ ┌─────────────────────┬──────────┬──────────┬─────┬─────┬─────┐ │
│ │ Name                │ Duration │ Resolut. │ FPS │Codec│ HDR │ │
│ ├─────────────────────┼──────────┼──────────┼─────┼─────┼─────┤ │
│ │ Furina-Stage….mp4   │ 00:42    │ 1920×1080│ 60  │HEVC │ No  │ │
│ │ Eula-…3840X2160.mp4 │ 01:05    │ 3840×2160│ 30  │H264 │ No  │ │
│ │ …                   │          │          │     │     │     │ │
│ └─────────────────────┴──────────┴──────────┴─────┴─────┴─────┘ │
│ Status: 2 items · Eula-…mp4 · 3840×2160 · 30 FPS · H.264       │
└────────────────────────────────────────────────────────────────┘
   (columns click-to-sort; metadata lazy; folder watch via ReadDirectoryChangesW)
```

### 10.4 Playlists

- Left pane: playlist list (**New**, **Rename**, **Delete**, **Duplicate**).
- Right pane: item list for the selected playlist (**Add from Library…**, **Add File…**, **Remove**, **Move Up/Down**, **Enable/Disable**, **Set start/end time**).
- Bottom bar: mode combo (Single / Sequential / Loop playlist / Shuffle), **Loop** checkbox, and **Apply to monitor** dropdown (assigns this playlist to a monitor — the mechanism behind Independent mode).
- Playlist changes are persisted immediately (batched) via `PlaylistStore`.

Wireframe:

```text
┌─ Playlists ────────────────────────────────────────────────────┐
│ ┌────────────────┐  ┌────────────────────────────────────────┐ │
│ │ My Wallpapers  │  │  #  Name                Start   End  On│ │
│ │ Ambient        │  │  1  Furina-Stage….mp4     —      —  ☑ │ │
│ │ (new)          │  │  2  Eula-….mp4            —      —  ☑ │ │
│ │                │  │                                        │ │
│ │ [New] [Rename] │  │ [Add from Library…] [Add File…]        │ │
│ │ [Delete] [Dup] │  │ [Remove] [▲ Up] [▼ Down]               │ │
│ └────────────────┘  └────────────────────────────────────────┘ │
│ Mode: [Sequential ▾]  [☑ Loop]   Apply to monitor: [DISPLAY1 ▾]│
└────────────────────────────────────────────────────────────────┘
```

### 10.5 Monitors

- Monitor list: one row per display (name, resolution, refresh rate, primary flag) from `MonitorManager`; live-updates on connect/disconnect.
- Selected monitor details: wallpaper source (single file **or** playlist dropdown), scaling mode (Fill default / Fit / Stretch / Center).
- Global mode control: **Independent** (default, per §5) vs **Clone** radio — Clone uses the primary monitor's wallpaper on all displays (one decoder). **Exactly two options in v1**: shared-playlist mode is deferred to v2 and would add a third radio option when it lands (nothing in the v1 wireframe anticipates it).
- Preview button shows a **static snapshot** of the current frame — the engine performs a one-time frame grab on demand (no render loop, no per-frame copies); if the grab proves costly in practice, defer preview to v2 and remove the button from v1.

Wireframe:

```text
┌─ Monitors ─────────────────────────────────────────────────────┐
│ Wallpaper mode:  (•) Independent    ( ) Clone                  │
│ ┌────────────────────────────────────────────────────────────┐ │
│ │ Monitor    Resolution    Refresh    Primary                │ │
│ │ DISPLAY1   1536×864      60 Hz      ✔                      │ │
│ └────────────────────────────────────────────────────────────┘ │
│ Wallpaper source : [My Wallpapers ▾]   (playlist or file)      │
│ Scaling          : [Fill ▾]                                    │
│ [ Preview ]   [ Set as wallpaper for DISPLAY1 ]                │
└────────────────────────────────────────────────────────────────┘
   (list live-updates on connect/disconnect; Clone = one decoder,
    primary monitor's wallpaper on all displays)
```

### 10.6 Performance

- Toggles: pause on game / fullscreen / high CPU / high GPU / high RAM.
- Thresholds (with pause + resume values): CPU %, GPU %, RAM % (e.g. 85/65, 90/70, 90/75 defaults).
- Delays: pause delay (s), resume delay (s) (3/5 defaults).
- Battery mode: Continue / Reduce quality / Pause (default Pause).
- **Advanced Performance** (collapsible): performance mode (Performance / Balanced / Quality / **Ultra Low Resource**), frame-queue depth, long-pause release seconds.
- All values write through `ConfigurationManager` (validated, clamped) and take effect live — no restart.

Wireframe:

```text
┌─ Performance ─────────────────────────────────────────────────┐
│ ☑ Pause on game        ☑ Pause on high GPU                   │
│ ☑ Pause on fullscreen  ☑ Pause on high CPU                   │
│                        ☑ Pause on high RAM                    │
│ CPU pause / resume : [85] % / [65] %                          │
│ GPU pause / resume : [90] % / [70] %                          │
│ RAM pause / resume : [90] % / [75] %                          │
│ Pause delay : [3] s      Resume delay : [5] s                 │
│ Battery mode : [Pause ▾]                                      │
│ ▸ Advanced Performance                                        │
│    Mode: [Balanced ▾]  Frame queue: [3]  Long-pause release: [5] s│
└────────────────────────────────────────────────────────────────┘
   (Advanced Performance is a collapsible section)
```

### 10.7 Settings

- **Start with Windows** (checkbox → HKCU Run value; no admin).
- **Minimize to tray on close** (checkbox).
- Logging level (combo: INFO default, DEBUG for diagnostics; affects Logger, not frame path).
- About: version, build config (Debug/Release), toolchain, link to README.

Wireframe:

```text
┌─ Settings ────────────────────────────────────────────────────┐
│ ☑ Start with Windows                                          │
│ ☑ Minimize to tray on close                                   │
│ Logging level : [INFO ▾]                                      │
│ ────────────────────────────────────────────────────────────  │
│ About:  Video Wallpaper v0.1.0 (Release, x64)                 │
│         MSVC 14.44 · Windows SDK 10.0.26100                   │
│         [ Open README ]                                       │
└───────────────────────────────────────────────────────────────┘
```

### 10.8 System tray

- Icon (embedded resource) + tooltip showing current state (e.g. `Video Wallpaper — Playing`).
- Menu: **Resume**, **Pause**, **Next**, **Previous**, separator, **Current wallpaper** (read-only info item), **Open Video Wallpaper**, **Settings**, separator, **Exit**.
- Left-click toggles the main window (open/hide). Right-click opens the menu.
- Tray must not keep the UI alive — it holds only the icon + menu resources.

Wireframe:

```text
Tray icon:  🎬  tooltip: "Video Wallpaper — Playing"
Left-click  → toggle main window (open/hide)

Right-click menu:
┌────────────────────────────────┐
│ Resume                         │
│ Pause                          │
│ Next                           │
│ Previous                       │
│ ────────────────────────────── │
│ Current wallpaper: Eula-….mp4  │  (read-only info item)
│ ────────────────────────────── │
│ Open Video Wallpaper           │
│ Settings                       │
│ ────────────────────────────── │
│ Exit                           │
└────────────────────────────────┘
```

### 10.9 Interaction & error handling

- UI thread never blocks on engine work (file dialogs excepted); engine calls are fire-and-forget commands.
- Errors surfaced as message boxes **and** written to the log (never silently swallowed).
- Closing/reopening the window repeatedly must not leak controls, timers, or telemetry subscriptions (covered by M11 leak cycle).

### 10.10 ApplicationController command surface

All UI/tray interactions are **commands** posted to a thread-safe queue and consumed on the control thread (fire-and-forget; the UI never blocks on engine work). Commands are typed structs with the payloads below. Reads are **not** commands — the UI subscribes to snapshots/notifications (§10.11).

| Command | Payload | Target subsystem |
|---|---|---|
| `PLAY_PAUSE_TOGGLE` | — | PlaybackController |
| `PAUSE` | — (reason: User) | PlaybackController / ResourceGovernor |
| `RESUME` | — (clears User reason) | PlaybackController / ResourceGovernor |
| `NEXT` / `PREVIOUS` | — | PlaylistManager / PlaybackController |
| `SET_WALLPAPER_FILE` | monitorId, path | WallpaperManager |
| `SET_WALLPAPER_PLAYLIST` | monitorId, playlistId | WallpaperManager |
| `SET_GLOBAL_MODE` | clone \| independent | WallpaperManager |
| `SET_SCALING` | monitorId, fill \| fit \| stretch \| center | WallpaperManager |
| `GRAB_FRAME_SNAPSHOT` | monitorId (returns static bitmap) | D3D11Renderer (engine-side grab) |
| `PLAYLIST_CREATE` / `RENAME` / `DELETE` / `DUPLICATE` | name / id,name / id / id | PlaylistManager |
| `PLAYLIST_ADD_FILES` | playlistId, paths[] | PlaylistManager |
| `PLAYLIST_ADD_LIBRARY_ITEMS` | playlistId, libIds[] | PlaylistManager |
| `PLAYLIST_REMOVE_ITEM` / `MOVE_ITEM` | id, index (+delta) | PlaylistManager |
| `PLAYLIST_TOGGLE_ITEM` | id, index, enabled | PlaylistManager |
| `PLAYLIST_SET_ITEM_TIMES` | id, index, start?, end? | PlaylistManager |
| `PLAYLIST_SET_MODE` | single \| sequential \| loop \| shuffle | PlaylistManager |
| `PLAYLIST_SET_LOOP` | bool | PlaylistManager |
| `LIBRARY_ADD_FILES` / `ADD_FOLDER` / `REMOVE` / `REFRESH` | paths[] / path / ids[] / — | LibraryManager |
| `CONFIG_SET` | key, value (validated, clamped) | ConfigurationManager |
| `SHOW_UI` | optional tab id | Win32UI (via controller) |
| `TOGGLE_UI` | — | Win32UI (via controller) |
| `FOCUS` | — (from 2nd instance) | Win32UI (via controller) |
| `EXIT` | — | ApplicationController (shutdown) |

`CONFIG_SET` keys (all validated/clamped, live effect, no restart): pause toggles (game/fullscreen/highCPU/highGPU/highRAM), threshold pairs (cpu/gpu/ram pause+resume), pause/resume delays, battery mode, performance mode, frame-queue depth, long-pause release seconds, start-with-windows, minimize-to-tray, log level.

### 10.11 Control → command mapping

**Reads (no command):** the UI subscribes to `StatsCollector` telemetry snapshots (~1–2 Hz while open), playback-state notifications, `MonitorManager` add/remove/change events, and library/playlist change notifications. Column sorting, list selection, and local layout are UI-side only.

**Home:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| Pause/Resume button | click | `PLAY_PAUSE_TOGGLE` | PlaybackController | label follows state notification |
| Next button | click | `NEXT` | PlaylistManager | |
| Previous button | click | `PREVIOUS` | PlaylistManager | |
| status fields | — | (read) telemetry snapshot | StatsCollector | 1–2 Hz while window open |

**Library:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| Add Files… | click | `LIBRARY_ADD_FILES` | LibraryManager | file dialog (UI thread) |
| Add Folder… | click | `LIBRARY_ADD_FOLDER` | LibraryManager | registers folder watch |
| Remove | click | `LIBRARY_REMOVE` (selected ids) | LibraryManager | |
| Set as wallpaper | click | `SET_WALLPAPER_FILE` (monitor, path) | WallpaperManager | monitor = Monitors panel selection |
| Refresh | click | `LIBRARY_REFRESH` | LibraryManager | manual incremental rescan |
| row double-click | dblclick | `SET_WALLPAPER_FILE` | WallpaperManager | same as Set as wallpaper |
| column headers | click | — (local sort) | — | |
| status bar | — | (read) selection metadata | LibraryManager snapshot | |

**Playlists:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| New | click | `PLAYLIST_CREATE` (name) | PlaylistManager | |
| Rename | click | `PLAYLIST_RENAME` (id, name) | PlaylistManager | |
| Delete | click | `PLAYLIST_DELETE` (id) | PlaylistManager | blocked if assigned to a monitor (confirm dialog) |
| Duplicate | click | `PLAYLIST_DUPLICATE` (id) | PlaylistManager | |
| Add from Library… | click | `PLAYLIST_ADD_LIBRARY_ITEMS` (id, ids) | PlaylistManager | |
| Add File… | click | `PLAYLIST_ADD_FILES` (id, paths) | PlaylistManager | file dialog |
| Remove | click | `PLAYLIST_REMOVE_ITEM` (id, index) | PlaylistManager | |
| Up / Down | click | `PLAYLIST_MOVE_ITEM` (id, index, ±1) | PlaylistManager | |
| Enable/Disable | toggle | `PLAYLIST_TOGGLE_ITEM` (id, index, bool) | PlaylistManager | |
| Set start/end time | dialog OK | `PLAYLIST_SET_ITEM_TIMES` (id, index, start?, end?) | PlaylistManager | optional fields |
| Mode combo | change | `PLAYLIST_SET_MODE` | PlaylistManager | Single/Sequential/Loop/Shuffle |
| Loop checkbox | toggle | `PLAYLIST_SET_LOOP` (bool) | PlaylistManager | |
| Apply to monitor | change | `SET_WALLPAPER_PLAYLIST` (monitor, id) | WallpaperManager | mechanism behind Independent mode |

**Monitors:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| mode radio (Independent/Clone) | change | `SET_GLOBAL_MODE` | WallpaperManager | Independent default |
| wallpaper source combo | change | `SET_WALLPAPER_PLAYLIST` \| `SET_WALLPAPER_FILE` | WallpaperManager | playlist id or file path |
| scaling combo | change | `SET_SCALING` (monitor, mode) | WallpaperManager | Fill default |
| Preview | click | `GRAB_FRAME_SNAPSHOT` (monitor) | D3D11Renderer | one-time engine-side grab |
| Set as wallpaper for <monitor> | click | `SET_WALLPAPER_PLAYLIST`/`SET_WALLPAPER_FILE` | WallpaperManager | applies current source selection |
| monitor list | — | (read) MonitorManager events | MonitorManager | live add/remove/change |

**Performance:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| pause toggles (5) | toggle | `CONFIG_SET` (pauseOnGame/…, bool) | ConfigurationManager | debounced write |
| threshold fields (3 pairs) | edit/enter | `CONFIG_SET` (cpu/gpu/ram thresholds) | ConfigurationManager | clamped; pause≠resume validation |
| delay fields | edit/enter | `CONFIG_SET` (pause/resume delay) | ConfigurationManager | |
| battery mode combo | change | `CONFIG_SET` (battery mode) | ConfigurationManager | |
| perf mode combo | change | `CONFIG_SET` (performance mode) | ConfigurationManager | Advanced section |
| frame-queue field | edit/enter | `CONFIG_SET` (frame queue) | ConfigurationManager | Advanced section |
| long-pause release field | edit/enter | `CONFIG_SET` (long-pause release) | ConfigurationManager | Advanced section |

**Settings:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| Start with Windows | toggle | `CONFIG_SET` (startWithWindows) | ConfigurationManager | writes HKCU Run (no admin) |
| Minimize to tray on close | toggle | `CONFIG_SET` (minimizeToTray) | ConfigurationManager | |
| Logging level combo | change | `CONFIG_SET` (logLevel) | ConfigurationManager | INFO default |
| Open README | click | — (UI-local `ShellExecute`) | — | no engine command |

**Tray:**

| Control | Event | Command | Target | Notes |
|---|---|---|---|---|
| Resume | click | `RESUME` | PlaybackController | |
| Pause | click | `PAUSE` | PlaybackController | |
| Next / Previous | click | `NEXT` / `PREVIOUS` | PlaylistManager | |
| Current wallpaper | — | (read) state notification | — | read-only info item |
| Open Video Wallpaper | click | `SHOW_UI` | Win32UI | |
| Settings | click | `SHOW_UI` (Settings tab) | Win32UI | |
| Exit | click | `EXIT` | ApplicationController | deterministic shutdown |
| icon left-click | click | `TOGGLE_UI` | Win32UI | open/hide main window |
| tooltip | — | (read) state notification | — | e.g. "Video Wallpaper — Playing" |

Second-instance launch (separate process): `FOCUS` → existing instance brings its window forward.

### 10.12 Notification & snapshot structures (read side)

The UI reads in two ways: **(a)** a one-time snapshot when the window opens, then **(b)** event-driven push notifications. All delivery happens on the UI thread. The contract types live in one shared header, `src/app/UiContract.h`, included by both the UI and the engine modules.

**Delivery model:**

- `UiSnapshot getUiSnapshot()` — pull once on window open: current telemetry, per-monitor playback states, monitor list, library items, playlists, wallpaper assignments. Gives panels immediate values before the first push arrives.
- `INotificationSink` — push deltas; invoked on the UI thread (marshaled via `PostMessage` to the control window).
- **Frequency:** telemetry at 1–2 Hz; all other events immediate.
- **No subscribers ⇒ no telemetry work:** the engine stops producing/pushing telemetry when the UI is closed (subscription count = 0). This is separate from `WorkloadMonitor`, whose 1–2 s sampling continues regardless because it feeds the `ResourceGovernor` pause decisions — but it never pushes to anyone when the UI is closed.
- Subscription is tied to window lifetime: subscribe on create, unsubscribe on destroy (M11 leak gate).

```cpp
// src/app/UiContract.h (excerpt)

struct INotificationSink {
    virtual void onTelemetry(const TelemetrySnapshot&) = 0;              // 1–2 Hz
    virtual void onPlaybackState(const PlaybackStateNotification&) = 0;  // per monitor
    virtual void onMonitorEvent(const MonitorEvent&) = 0;                // add/remove/change
    virtual void onLibraryChange(const LibraryChangeNotification&) = 0;
    virtual void onPlaylistChange(const PlaylistChangeNotification&) = 0;
    virtual void onWallpaperAssignment(const WallpaperAssignmentNotification&) = 0;
    virtual ~INotificationSink() = default;
};
```

```cpp
// TelemetrySnapshot — mirrors PerformanceStats (docs/01 §1.5.14) + per-monitor detail
struct TelemetrySnapshot {
    double cpuUsage;            // % overall
    double gpuUsage;            // % (engine utilization estimate; see R-03)
    uint64_t gpuMemoryUsed;     // bytes (IDXGIAdapter3::QueryVideoMemoryInfo)
    uint64_t gpuMemoryBudget;   // bytes
    uint64_t systemMemoryUsed;  // bytes
    double  decodedFps;
    double  presentedFps;
    uint64_t droppedFrames;
    double  decodeLatencyMs;
    double  renderTimeMs;
    bool    hardwareDecode;
    struct PerMonitor { std::wstring monitorId; double presentedFps; uint64_t droppedFrames; };
    std::vector<PerMonitor> perMonitor;   // only monitors with an active wallpaper
};
```

```cpp
// PlaybackStateNotification — per-monitor playback status (Home + tray tooltip)
struct PlaybackStateNotification {
    enum class State { Playing, Paused, Suspended, NoWallpaper };
    State state;
    uint32_t pauseReasons;     // PauseReason bitmask (docs/01 §1.5.9)
    std::wstring monitorId;    // per-monitor sessions
    std::wstring videoName;    // current video file name
    std::wstring playlistName; // or empty for single-file
    std::wstring decoderMode;  // e.g. "NVIDIA NVDEC / hardware" | "Software fallback"
    std::wstring adapterName;  // GPU adapter description
};
```

```cpp
// MonitorEvent — reuses MonitorInfo (docs/02 §2.7: id, bounds, workArea, refresh, primary, active)
struct MonitorEvent {
    enum class Kind { Added, Removed, Changed } kind;
    MonitorInfo info;
};

struct LibraryChangeNotification {
    enum class Kind { Added, Removed, Updated, RescanStarted, RescanFinished } kind;
    std::vector<LibraryItemId> ids;  // affected items; LibraryItem = id, path, VideoMetadata, fileSize, lastWrite
};

struct PlaylistChangeNotification {
    enum class Kind { Created, Renamed, Deleted, Duplicated, ItemsChanged, ModeChanged } kind;
    PlaylistId id;
};

struct WallpaperAssignmentNotification {
    std::wstring monitorId;
    enum class Source { None, File, Playlist } source;
    std::wstring sourceId;    // file path or playlist id
    ScalingMode  scaling;     // Fill | Fit | Stretch | Center
    bool         clone;       // global mode: true = Clone, false = Independent
};
```

**Consumption rules:**

| Panel | Subscribes to | Behavior |
|---|---|---|
| Home | `TelemetrySnapshot` (1–2 Hz), `PlaybackStateNotification` (per monitor; shows the currently selected monitor) | Refresh fields; also feeds the tray tooltip state |
| Library | `LibraryChangeNotification` | Incremental row add/remove/update; `RescanStarted/Finished` drives the status bar and Refresh button state |
| Playlists | `PlaylistChangeNotification` | Refresh playlist list + items; invalidate "Apply to monitor" dropdowns |
| Monitors | `MonitorEvent`, `WallpaperAssignmentNotification` | Live monitor list; per-monitor source/scaling/mode readback |
| Performance | none (writes only) | Values come from the snapshot on open; no live subscription needed in v1 |

Engine side: each module implements emission via a small dispatcher that no-ops when the UI is closed; the UI implements `INotificationSink` and registers itself on window create.

### 10.13 UiContract.h (complete reference)

Authoritative, complete header — **supersedes the excerpts in §10.12** (which keep the delivery model and consumption rules). Pure data types plus one pure-virtual sink: no Windows headers, no engine types, no `std::filesystem` (paths are `std::wstring`). Includable by any translation unit; lives at `src/app/UiContract.h`, namespace `vw::ui`.

```cpp
// ===== UiContract.h — part 1: enums, media/monitor data, notifications =====
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vw::ui {

// ---- stable ids (strings; empty = none) ----
using MonitorId     = std::wstring;
using PlaylistId    = std::wstring;
using LibraryItemId = uint64_t;

// ---- enums ----
enum class VideoCodec : uint8_t    { Unknown, H264, HEVC, AV1, VP9 };
enum class ScalingMode : uint8_t   { Fill, Fit, Stretch, Center };
enum class PlaylistMode : uint8_t  { Single, Sequential, Loop, Shuffle };
enum class BatteryMode : uint8_t   { Continue, ReduceQuality, Pause };
enum class PlaybackState : uint8_t { NoWallpaper, Playing, Paused, Suspended };
enum class WallpaperSource : uint8_t { None, File, Playlist };
enum class MonitorEventKind : uint8_t   { Added, Removed, Changed };
enum class LibraryChangeKind : uint8_t  { Added, Removed, Updated, RescanStarted, RescanFinished };
enum class PlaylistChangeKind : uint8_t { Created, Renamed, Deleted, Duplicated, ItemsChanged, ModeChanged };

enum PauseReason : uint32_t {              // bitmask — docs/01 §1.5.9
    None            = 0,
    User            = 1u << 0,
    Game            = 1u << 1,
    Fullscreen      = 1u << 2,
    HighCPU         = 1u << 3,
    HighGPU         = 1u << 4,
    HighMemory      = 1u << 5,
    Battery         = 1u << 6,
    Locked          = 1u << 7,
    DisplayOff      = 1u << 8,
    MonitorHidden   = 1u << 9,
    SystemSuspended = 1u << 10,
};

// ---- media / monitor data (mirrors docs/01 §1.5.1, docs/02 §2.7) ----
struct VideoMetadata {
    uint32_t width = 0, height = 0;
    double frameRate = 0, durationSeconds = 0;
    VideoCodec codec = VideoCodec::Unknown;
    bool hdr = false;
    bool hasAudio = false;
};

struct MonitorInfo {                      // HMONITOR exposed as opaque handle
    MonitorId id;
    uintptr_t handle = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;        // bounds
    int32_t workX = 0, workY = 0, workW = 0, workH = 0; // work area
    uint32_t refreshNum = 0, refreshDen = 0;            // refresh rate
    bool primary = false;
    bool active = false;
};

struct LibraryItem {
    LibraryItemId id = 0;
    std::wstring path;
    VideoMetadata metadata;
    uint64_t fileSize = 0;
    uint64_t lastWriteTicks = 0;          // FILETIME
    bool metadataLoaded = false;
};

// ---- notifications (read side) ----
struct TelemetrySnapshot {                // mirrors PerformanceStats — docs/01 §1.5.14
    double cpuUsage = 0, gpuUsage = 0;    // % (gpu: engine estimate, see R-03)
    uint64_t gpuMemoryUsed = 0, gpuMemoryBudget = 0, systemMemoryUsed = 0;
    double decodedFps = 0, presentedFps = 0;
    uint64_t droppedFrames = 0;
    double decodeLatencyMs = 0, renderTimeMs = 0;
    bool hardwareDecode = false;
    struct PerMonitor { MonitorId monitorId; double presentedFps = 0; uint64_t droppedFrames = 0; };
    std::vector<PerMonitor> perMonitor;   // only monitors with an active wallpaper
};

struct PlaybackStateNotification {        // per-monitor status (Home + tray tooltip)
    PlaybackState state = PlaybackState::NoWallpaper;
    uint32_t pauseReasons = 0;            // PauseReason bitmask
    MonitorId monitorId;
    std::wstring videoName, playlistName;
    std::wstring decoderMode, adapterName; // e.g. "NVIDIA NVDEC / hardware"
};

struct MonitorEvent { MonitorEventKind kind; MonitorInfo info; };

struct LibraryChangeNotification {
    LibraryChangeKind kind;
    std::vector<LibraryItemId> ids;       // affected items
};

struct PlaylistChangeNotification { PlaylistChangeKind kind; PlaylistId id; };

struct WallpaperAssignmentNotification {
    MonitorId monitorId;
    WallpaperSource source = WallpaperSource::None;
    std::wstring sourceId;                // file path or playlist id
    ScalingMode scaling = ScalingMode::Fill;
    bool clone = false;                   // true = Clone, false = Independent
};

struct PlaylistSummary {
    PlaylistId id;
    std::wstring name;
    size_t itemCount = 0;
    PlaylistMode mode = PlaylistMode::Sequential;
    bool loop = true, shuffle = false;
};
```

```cpp
// ===== UiContract.h — part 2: commands, snapshot, sink =====

// ---- commands (write side; posted to the controller queue, fire-and-forget) ----
// Field usage per CommandId follows the §10.10 table:
//   s1/s2   = monitorId, path, playlistId, name, or CONFIG_SET key/value
//   paths   = bulk file paths          itemIds = library ids
//   i1/i2   = playlist item index, move delta
//   d1/d2   = optional start/end time (PlaylistSetItemTimes) or CONFIG_SET numbers
//   b1      = toggles (enabled, loop, clone, minimizeToTray, …)
//   scaling = SetScaling               mode = PlaylistSetMode / SetGlobalMode(clone via b1)
enum class CommandId : uint8_t {
    PlayPauseToggle, Pause, Resume, Next, Previous,
    SetWallpaperFile, SetWallpaperPlaylist, SetGlobalMode, SetScaling, GrabFrameSnapshot,
    PlaylistCreate, PlaylistRename, PlaylistDelete, PlaylistDuplicate,
    PlaylistAddFiles, PlaylistAddLibraryItems, PlaylistRemoveItem, PlaylistMoveItem,
    PlaylistToggleItem, PlaylistSetItemTimes, PlaylistSetMode, PlaylistSetLoop,
    LibraryAddFiles, LibraryAddFolder, LibraryRemove, LibraryRefresh,
    ConfigSet, ShowUi, ToggleUi, Focus, Exit,
};

struct Command {                          // tagged POD payload; cheap to queue
    CommandId id = CommandId::PlayPauseToggle;
    std::wstring s1, s2;
    std::vector<std::wstring> paths;
    std::vector<LibraryItemId> itemIds;
    int32_t i1 = 0, i2 = 0;
    double d1 = 0, d2 = 0;
    bool b1 = false;
    ScalingMode scaling = ScalingMode::Fill;
    PlaylistMode mode = PlaylistMode::Sequential;
};

// ---- one-time snapshot (pull on window open) ----
struct UiSnapshot {
    TelemetrySnapshot telemetry;
    std::vector<PlaybackStateNotification> playbackStates; // per active monitor
    std::vector<MonitorInfo> monitors;
    std::vector<LibraryItem> libraryItems;
    std::vector<PlaylistSummary> playlists;
    std::vector<WallpaperAssignmentNotification> assignments;
};

// ---- sink: push notifications; invoked on the UI thread ----
struct INotificationSink {
    virtual void onTelemetry(const TelemetrySnapshot&) = 0;             // 1–2 Hz
    virtual void onPlaybackState(const PlaybackStateNotification&) = 0; // per monitor
    virtual void onMonitorEvent(const MonitorEvent&) = 0;
    virtual void onLibraryChange(const LibraryChangeNotification&) = 0;
    virtual void onPlaylistChange(const PlaylistChangeNotification&) = 0;
    virtual void onWallpaperAssignment(const WallpaperAssignmentNotification&) = 0;
    virtual ~INotificationSink() = default;
};

} // namespace vw::ui
```

**Registration & guarantees (with the header):**

- `ApplicationController::subscribe(INotificationSink*)` / `unsubscribe(...)` — called on window create/destroy; **no callbacks after unsubscribe**; telemetry is pushed only while ≥1 subscriber (1–2 Hz), other events immediate.
- `ApplicationController::getUiSnapshot()` returns `UiSnapshot` for the pull-on-open path.
- Commands are enqueued by the UI via `postCommand(Command)`; the controller drains the queue on the control thread.
- All paths/ids are `std::wstring` (native Win32); `HMONITOR` is an opaque `uintptr_t`; geometry is plain int32 rects — the header stays free of Windows.h so any module can include it.
- The `Command` payload fields are positional per `CommandId`; a thin typed wrapper may be added later if the positional form proves error-prone (measure before refactoring — M13 rule).

---

## 11. Risks / open items carried into implementation

- R-01 WorkerW technique variance → runtime discovery + fallback (unchanged).
- R-02 HEVC/AV1 HW decode availability: HEVC confirmed on disk (1 file); AV1 decode on RTX 3050 expected but must be verified at M5; if the AV1 clip can't be HW-decoded, report and fall back to software.
- R-14 partial: HW verification happens on this machine (has GPU) — good; the *disruptive* and *multi-monitor* scenarios are the ones marked NOT MEASURED.
- No AV1/VP9/HDR source clips — resolved by ffmpeg synthesis; generated clips must not be committed (temp/gitignored location, cleaned up after M13).
- The `%APPDATA%` path rename touches docs/02 §2.9/§2.10, docs/03 M1, and docs/06 M1 — watch for stragglers.
- C++23 flag: `/std:c++23` smoke test at M1; fall back to `/std:c++latest` if needed.
- **Not a git repository** — resolved: `git init` + initial commit is the first M1 action (needed for the chosen commit-per-milestone workflow).
- **Single display (1536×864)** — real multi-monitor verification is impossible on this machine; mitigated via simulated-topology unit tests + documented `NOT MEASURED` (see §6).
- ~~License~~ — **RESOLVED (2026-08-17): Apache-2.0** (user requirement: "it must be full open source"). All open items are now closed; M1 can start.

---

## 12. Definition of done (overall)

- M1–M14 all ticked ✅ in `docs/06-progress-checklist.md` with exit criteria met.
- Debug + Release x64 builds green; unit tests green; `docs/06` cross-cutting gates satisfied at every milestone.
- `README.md` (root) complete per the source spec (doc 1) §64, including how low idle resource usage is achieved.
- M14 final report with **measured** numbers or explicit `NOT MEASURED — reason` (including the declined disruptive tests and reduced soak duration).
- Portable ZIP package builds from a clean checkout; no personal videos, no ffmpeg dependency, no debug artifacts in the package.
