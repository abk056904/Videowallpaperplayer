# 5. Key Decisions, Risks & Open Questions

---

## 5.1 Decisions (D-xx) — with rationale

| ID | Decision | Choice | Rationale / status |
|---|---|---|---|
| D-01 | Header layout | Headers beside sources (`src/<module>/foo.h` + `foo.cpp`) | The specs show both `include/`+`src/` and flat `src/`. Single tree = fewer files, no duplication; internal-only project. Revisit only if a public API boundary is needed. |
| D-02 | Renderer API | **Direct3D 11** | Spec mandates D3D11 unless a demonstrated reason for D3D12. MF hardware decode integrates cleanly with D3D11 (`IMFDXGIDeviceManager`). Revisit only if profiling shows D3D12 is substantially better (unlikely for a wallpaper). |
| D-03 | Decoding stack | **Media Foundation only** (system codecs) | Spec: "use system APIs, don't bundle codec frameworks." Consequence: HEVC/AV1 availability depends on installed Windows extensions (see R-02). |
| D-04 | Decoder discovery | MF Source Reader with `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` + DXGI device manager, hardware MFT first, software fallback | Simplest robust path; exposes actual transform for honest diagnostics. |
| D-05 | Wallpaper hosting | Runtime-discovered WorkerW/Progman hierarchy, per-monitor host windows | Spec requires runtime investigation, not hardcoded assumptions; per-monitor windows support independent playlists + per-monitor swap chains. |
| D-06 | Config format | **CLOSED — JSON** with a minimal hand-rolled parser/writer in `util/` (no third-party JSON lib) | Spec's config example is JSON; forbids unnecessary dependencies. Parser lives in `src/util/json.*`, strictly validates on load, ~300 lines. Only used for config/playlist state — never in frame paths. |
| D-07 | Unit test framework | **CLOSED — header-only `doctest`**, test-only target, never shipped | Single header, zero install, industry-standard; adds nothing to the product binary (tests are a separate executable). |
| D-17 | UI style | **CLOSED — standard Win32 common controls**, Windows 11 look, dark mode opt-in only if trivial | No custom theming; smallest practical UI per spec. |
| D-18 | Commit cadence | **CLOSED — git commit at each milestone boundary** (M0, M1, …) to record working states | Spec: "COMMIT/record working state" after each phase. Each commit still requires user permission at the time (repo rule). |
| D-08 | GPU utilization source | Windows "GPU Engine" performance counters (PDH / `gpuperfcounters.h`), per-engine where available; DXGI `QueryVideoMemoryInfo` for VRAM | Spec: use Windows GPU perf counters, not guesses; document limitations when engines aren't exposed (R-03). |
| D-09 | Threading | Fixed small pool: main/UI, decode worker(s) = number of distinct videos, one render/control worker, one monitoring worker, thumbnail worker only while UI open | Spec explicitly forbids per-monitor/per-video/per-metric threads beyond necessity. |
| D-10 | Pause policy authority | `ResourceGovernor` is the **only** component that starts/stops decoding/rendering | All three specs demand a central authority; prevents subsystems fighting. |
| D-11 | Autostart | HKCU `...\CurrentVersion\Run` value (per-user, no admin, no service) | Spec: optional, per-user, no admin, no service. |
| D-12 | Packaging | Portable ZIP (primary) + optional lightweight installer (MSI/Inno) | Spec §81 prefers portable ZIP; installer optional. |
| D-13 | Single instance | Named mutex + registered window message to focus existing instance | Lightweight, spec-sanctioned. |
| D-14 | Library index | No database; incremental scan + `ReadDirectoryChangesW` + metadata cache file | Spec §42: no SQLite "merely because convenient"; 10k-item playlists stay small (paths + cached metadata). |
| D-15 | Thumbnails | MF-grabbed small frames; LRU memory (≤~50) + bounded disk cache (default well under 100 MB); low-priority cancellable worker | Spec §33/§50–51. |
| D-16 | HDR | **Closed:** v1 = detect HDR + document behavior + safe SDR fallback only; no HDR rendering path in v1 | Spec §20: never claim untested HDR. Real HDR path is a v2 item (see `03-implementation-plan.md` §3.0). |

---

## 5.2 Risk register (R-xx)

| ID | Risk | Impact | Mitigation |
|---|---|---|---|
| R-01 | WorkerW technique differs across Windows builds / Explorer versions | Wallpaper not behind icons or not visible | Runtime discovery (never hardcode); log actual hierarchy; fallback to child-of-`Progman`; verify on target machine before M3 exit. |
| R-02 | HEVC/AV1 hardware decode unavailable (missing Store "HEVC Video Extensions" / driver) | Codec fails or falls back to software (CPU cost) | Detect capability per adapter (`CheckVideoDecoderFormat` + extension presence), pick HW→SW fallback automatically, report decoder mode honestly; document in README. |
| R-03 | GPU engine utilization counters unavailable (older drivers / virtualized GPUs) | No accurate single "GPU %" | Use per-engine counters when exposed; else use available memory + engine activity + documented "GPU busy estimate" with limitations; never fabricate precision. |
| R-04 | NV12/P010 → shader conversion complexity (plane SRVs, 10-bit) | Visual artifacts / wrong colors | Implement + test BT.709 matrix against known reference frames; P010 path tested with a real 10-bit file if available, else documented as not fully verified; safe fallback to CPU conversion only if GPU path fails. |
| R-05 | Device loss / driver reset mid-playback | Crash or permanent black wallpaper | Full teardown/recreate path (M13) with `GetDeviceRemovedReason`, controlled retry/backoff, no tight loop; tested via driver disable/enable on a test rig. |
| R-06 | Explorer restart corrupts host state | Wallpapers vanish until app restart | Host-invalidity watch + full rediscovery (M13); playlist/config untouched. |
| R-07 | Cross-adapter texture sharing (iGPU + dGPU) unstable | Clone mode on mixed-GPU systems breaks | Don't force cross-adapter sharing; per-adapter device fallback; correctness first (doc 1 §38). |
| R-08 | Hysteresis/debounce oscillation at thresholds | Pause/resume flapping (games, Alt+Tab) | Configurable thresholds + durations with separate pause/resume bands; unit-tested `PausePolicy`; Alt+Tab scenario tested explicitly (doc 1 §56). |
| R-09 | Per-frame allocations / string construction in hot paths | Unnecessary CPU churn, fragmentation | Allocate once, reuse; steady-state audit (M13); code-search audit (`new`, `vector`, `wstring` in frame path). |
| R-10 | Memory/COM leaks over 24 h | Monotonic growth; VRAM leak keeps old textures alive | RAII everywhere; leak-cycle tests + 24 h run (M13); COM reference audit after pause/transition. |
| R-11 | Busy loops creep in (Sleep(1) polling) | CPU waste — defeats the core goal | Zero-busy-wait policy enforced at review + audit searches; workers block on events/timers/CVs. |
| R-12 | UI work blocks engine (thumbnail generation, sync scans) | Stutter / CPU spikes | UI never decodes/renders/polls; thumbnails async+cancellable; UI destroyed when closed. |
| R-13 | High-refresh monitor pulls decode rate up | 144 Hz monitor → 144 decodes/s | Source-FPS pacing + per-monitor presentation deadlines (M6/M8); verified via presented-FPS stats. |
| R-14 | Environment lacks GPU / hardware decode (CI, VMs) | Cannot verify HW path or some acceptance items | Mark `NOT MEASURED` with reason; software path still tested; hardware verification deferred to a real machine — do not fake results. |

---

## 5.3 Resource lifetime map (ownership contract)

| Resource | Owner | Released when |
|---|---|---|
| D3D11 device, DXGI factory | `D3D11DeviceManager` (app lifetime) | Shutdown; recreated on device loss |
| Shaders, sampler, rasterizer, shared render targets | `D3D11Renderer` / `TextureManager` | Shutdown; recreated on device loss |
| Swap chains + host windows | `WallpaperHost` (per monitor) | Monitor disconnect, Explorer restart, shutdown |
| Decoder, source reader, MF DXGI manager binding | `DecoderManager` (playback session) | Video change, pause→SUSPENDED, shutdown, device loss |
| Frame queue + decoded frames | `VideoPlayer` / `FrameScheduler` | Pause, seek, video change, shutdown |
| Next-video preparation | `PlaylistManager` (per session) | Transition, long pause (SUSPENDED), shutdown |
| Thumbnails (memory + disk) | `ThumbnailCache` | UI close, eviction (LRU), shutdown |
| Config/playlist state | `ConfigurationManager` / `PlaylistStore` | Persisted on change/transition/shutdown |
| Control window, tray icon | `ApplicationController` / `TrayController` | Shutdown |

Every COM object uses `Microsoft::WRL::ComPtr`; raw owning pointers are banned in new code.

---

## 5.4 Open questions (status as of 2026-08-17)

1. ~~JSON vs INI~~ — **RESOLVED (D-06):** JSON with minimal in-repo parser.
2. ~~Test framework~~ — **RESOLVED (D-07):** header-only `doctest`, test-only target.
3. ~~Hardware availability~~ — **ANSWERED by M0 audit:** this machine has an **NVIDIA GeForce RTX 3050 Laptop GPU + AMD Radeon iGPU** (Windows 11, 13.8 GB RAM). Hardware-decode verification, multi-GPU adapter selection, and 24 h runs are feasible here. See `BUILD_NOTES.md`.
4. ~~Test videos~~ — **ANSWERED by M0 audit + codec probe:** real `.mp4` clips exist at `C:\Users\mbk43\Videos\bgcmp\` (incl. 4K/"4K60") and `C:\Users\mbk43\Downloads\Video\`. Fourcc probe (2026-08-17): **10× H.264, 1× HEVC** (`Furina-...mp4`); **no AV1/VP9/HDR10 content**. H.264 + HEVC coverage is ready; AV1/VP9 and a 10-bit/HDR clip must be sourced or synthesized (e.g. ffmpeg, not a runtime dependency) for full M13 stress coverage, else reported `NOT MEASURED`. Details in `BUILD_NOTES.md`.
5. ~~Audio support~~ — **RESOLVED:** out of scope for v1 (specs mandate audio disabled by default; only `hasAudio` metadata is recorded). See `03-implementation-plan.md` §3.0.
6. ~~HDR~~ — **RESOLVED (D-16):** v1 = detect + document + safe SDR fallback only; real HDR path deferred to v2.
7. ~~UI language/style~~ — **RESOLVED (D-17):** standard Win32 common controls, Windows 11 look, no custom theming.
8. ~~Commit cadence~~ — **RESOLVED (D-18):** git commit at each milestone boundary (each commit still requires user permission at the time).
9. ~~Toolchain install~~ — **RESOLVED (2026-08-17):** user approved **VS 2022 Build Tools + CMake**; installed via winget and **verified with a Debug+Release x64 hello-CMake build**. MSVC 14.44.35207, Windows SDK 10.0.26100.0, CMake 4.4.2. Full details in `BUILD_NOTES.md`. M0 exit criteria met.

---

## 5.5 First actions once implementation starts

1. ✅ All open questions resolved (Q3/Q4 answered by M0 audit; Q9 by toolchain install).
2. ✅ M0 toolchain audit + install + verification complete — see `BUILD_NOTES.md`.
3. **Next: scaffold M1 skeleton and get a green Debug+Release x64 build.**
4. Proceed milestone by milestone; never skip exit criteria (see `03-implementation-plan.md`).
