# 4. Testing & Profiling

Strategy for proving correctness, resource efficiency, and stability. Every measured claim in the final report must come from the procedures here; **numbers are never invented** — unmeasurable items are reported as `NOT MEASURED — reason`.

---

## 4.1 Test tooling

- Unit tests: header-only framework or minimal custom harness (decision D-07). Runs via CTest (`ctest`), no external install.
- Integration/hardware tests: separate targets that require a real desktop + GPU; not part of the default `ctest` run.
- Profiling: Windows Performance Recorder/Analyzer (WPR/WPA), GPUView, PIX, Visual Studio profiler, Process Explorer, Task Manager (sanity only). See §4.6.

---

## 4.2 Unit test plan (pure logic, no GPU/desktop required)

| Component | Tests |
|---|---|
| `ConfigurationManager` | Defaults on first run; every field validated/clamped; unknown fields ignored; corrupted file → `.bak` + defaults + continue; round-trip save/load; atomic write (write-temp-then-rename) |
| `PlaylistManager` | add/remove/move/replace/clear; next/prev wrap rules; loop vs sequential vs single; shuffle with no immediate repeat (n>1); persistence round-trip; start/end time trimming; disabled items skipped; broken item → marked unavailable + advance |
| `PausePolicy` / hysteresis | Threshold crossing with timing: HIGH_GPU after 3 s above 90%; clear after 5 s below 70%; no oscillation at boundary; immediate reasons bypass delay; only-resume-when-all-clear; bitmask composition (game + high GPU) |
| `ResourceGovernor` transitions | Doc 3 §95 table: ACTIVE+game→PAUSED; ACTIVE+fullscreen→PAUSED; ACTIVE+high GPU→PAUSED (delayed); high GPU persists→stay paused; PAUSED long→SUSPENDED; SUSPENDED+desktop→ACTIVE; LOCKED/DISPLAY_OFF→SUSPENDED; USER pause overrides |
| `FrameQueue` | Capacity bound respected; producer blocks when full; drop-oldest under pressure; newest-≤-deadline selection; stale-frame discard; counters (droppedFrames); thread-safety under stress (TSan/ASan builds where available) |
| `FullscreenDetector` | Classify: true fullscreen vs borderless fullscreen vs maximized vs normal (synthetic window rects/styles); config override (treat maximized as fullscreen when set) |
| `GameDetector` | Allow/deny list matching (case-insensitive exe names, paths); classification cache validity/invalidation; no full-process enumeration (assert only single-process queries) |
| `VideoMetadata` parsing | Synthetic MF media types → correct width/height/fps/duration/codec/HDR flags |
| Logging | Level filtering; rotation at size cap; no per-frame spam (aggregate counter test) |
| Config JSON | Parser round-trip; malformed input recovery (util-level) |

---

## 4.3 Integration & hardware test matrix

### 4.3.1 Video playback (doc 1 §55)

| Video | H.264 | HEVC/H.265 | AV1 | VP9 |
|---|---|---|---|---|
| 1080p 30 | ✔ | – | – | – |
| 1080p 60 | ✔ | – | – | – |
| 1440p 60 | ✔ | ✔ | – | – |
| 4K 30 | – | ✔ | – | – |
| 4K 60 | – | ✔ | ✔* | – |

\* AV1 only where hardware/system supports it. Also test: `.mp4`, `.mkv`, `.mov`, `.webm`, `.avi`; a 10-bit/HDR file (P010 path) if available; a corrupt file; a missing/renamed/deleted file mid-playback; an unsupported codec (graceful skip + diagnostics).

### 4.3.2 Monitor configurations (doc 1 §55)

- 1 / 2 / 3 monitors; mixed resolutions (4K 120 Hz + 1440p 165 Hz + 1080p 60 Hz); portrait; ultrawide.
- Clone mode (same video, decode-once verified via decoder-count diagnostics + GPU engine activity).
- Independent mode (per-monitor videos, per-monitor playlists).
- Shared-playlist mode — v2 only (v1 covers Clone + Independent).
- Hot-plug: connect/disconnect monitor, change resolution, change refresh rate, change primary — no restart; no leaked handles/VRAM (verify via counters before/after).

### 4.3.3 Game / fullscreen (doc 1 §56–57, doc 3 §83)

- Fullscreen game, borderless game, windowed game; high-CPU workload; high-GPU workload; game launch/exit; Alt+Tab.
- Expected: game starts → wallpaper pauses; Alt+Tab away while game runs → follows configured policy (no flapping); game exits → resume after configured delay.
- Fullscreen YouTube/local video/media player → pause; browser maximized → no pause; browser normal → no pause.

### 4.3.4 Power / session (doc 1 §58)

- AC → battery (pause per default); battery saver; lock → SUSPENDED (decode+render stop, GPU resources released); unlock → resume; display off → SUSPENDED; display on → resume; sleep/wake → correct recovery (single resume event, controlled retry, no tight loop).

### 4.3.5 Recovery (doc 1 §59, doc 3 §90)

- Explorer restart (kill/restart `explorer.exe`) → hosts rebuilt, wallpapers restored, playlist/config intact.
- GPU device loss: driver reset / disable-enable adapter on test rig → clean recreate + resume (or graceful degraded state), no crash.
- Decoder failure mid-stream → item marked unavailable, next item plays; file reappears → recovers.
- Config corruption → backup + defaults + continue.

---

## 4.4 Performance test scenarios (doc 2 §83, doc 3 §99–100)

| Scenario | Expectation |
|---|---|
| A. Wallpaper playing (normal desktop) | small necessary resource usage |
| B. Manually paused | near-zero active playback resources |
| C. Fullscreen app active | near-zero rendering/decoding |
| D. Game active | near-zero rendering/decoding |
| E. Windows locked | near-zero rendering/decoding |
| F. Display off | near-zero rendering/decoding |

Metrics collected per scenario: CPU %, GPU % (per engine where available), RAM, VRAM, thread count, handle count, decoder count, frame queue depth, presented FPS, dropped frames, GPU engine activity (Video Decode vs 3D), power draw if measurable.

---

## 4.5 Stability & leak tests (doc 2 §84–85, doc 3 §80–81)

- **24 h run** (where environment permits): RAM/VRAM/threads/handles at 0 h/1 h/6 h/12 h/24 h; **any monotonic growth must be fixed, not documented**.
- **Leak cycles:** repeat {play → pause → resume → next → previous → change monitor → change video → open/close UI → lock/unlock} for many iterations; assert RAM/VRAM/handles/COM refs stable.
- **COM audit:** every `ComPtr` released; check for retained device contexts/textures/decoders after pause (doc 2 §53).
- **Audit searches** (doc 2 §99–100, doc 3 §87): `while(true)`, `while (running)`, `Sleep(`, `sleep_for`, `new`, `malloc`, `memcpy`, `CopyResource`, `Map`, `Unmap`, `CreateTexture`, `CreateThread`, `TODO`, `FIXME`, `stub`, `placeholder`, `not implemented` — each occurrence reviewed (necessary? optimized? synchronized?) and core functionality contains no placeholders.

---

## 4.6 Profiling methodology (doc 1 §54, doc 3 §85)

1. Establish baseline in Release x64 for scenarios A–F (don't profile Debug).
2. Capture traces: WPR (CPU), GPUView (GPU queue/present), PIX (D3D11), VS profiler (hot paths), Process Explorer (handles/threads/VRAM), Task Manager (sanity).
3. Hot paths to measure: decoder, frame queue, scheduler, render, stats sampling — allocations, locks, copies, syscalls, GPU submissions.
4. Optimize one change at a time; measure before/after; **revert if no measurable win** (doc 2 §97).
5. Never claim literal zeros; report near-zero with measurements (doc 2 §98).

---

## 4.7 Resource budget targets (targets, not guarantees)

| Metric | ACTIVE (playing) | PAUSED | SUSPENDED |
|---|---|---|---|
| CPU | as low as HW decode allows (e.g. single-digit % for 4K HW path) | near-zero active work | near-zero |
| GPU rendering | only necessary presents (source FPS) | 0 wallpaper presents | 0 |
| RAM | bounded; no whole-video buffers; ≤ a few × frame size for 4K if CPU path ever needed (avoid) | minimal | minimal |
| VRAM | current frame + minimal intermediates (2–3 frames max) | minimal | minimal |
| Decoder | active | stopped | released |
| Frame queue | 2–3 | empty | empty |
| Threads | ≈4–6 | fewer (thumbnail stopped) | minimal |
| CPU wakeups | frame deadlines only | none | none |

Startup targets: no library scan, no thumbnails, no process scan at launch; wallpaper visible within ~1–2 s of launch.

---

## 4.8 Acceptance checklist (consolidated from all three specs)

### Build
- [ ] Release x64 build succeeds (Debug x64 too)
- [ ] No unnecessary runtime framework (.NET/Electron/Qt/…) 
- [ ] Minimal executable; minimal dependencies; no bundled sample videos/debug files in production package

### Playback
- [ ] Video plays smoothly; H.264, HEVC, AV1*, VP9*; MP4/MKV/WebM/MOV/AVI
- [ ] Loop, playlist, shuffle, next/prev, single/sequential modes work
- [ ] Scaling modes (Fill default / Fit / Stretch / Center) work via GPU shader
- [ ] Audio disabled by default

### Hardware acceleration
- [ ] Hardware decoding selected where available; actual decoder mode reported honestly
- [ ] Software fallback works and is reported
- [ ] No unnecessary CPU↔GPU copies in happy path (GPU-resident NV12/P010 → shader)

### Multi-monitor
- [ ] Independent per-monitor wallpapers/playlists
- [ ] Clone/shared mode decodes once
- [ ] Hot-plug/resolution/refresh/primary changes without restart

### Auto-pause & efficiency
- [ ] Game, fullscreen, high CPU/GPU/RAM detection with hysteresis + debounce (configurable)
- [ ] Lock/display-off/battery/suspend pause; resume after clear; immediate conditions bypass delay
- [ ] Paused ⇒ no decoding, no render loop, minimal CPU/GPU activity
- [ ] Long pause ⇒ decoder + temporary GPU resources released
- [ ] No busy waits anywhere; workers block; monitoring ≤1–2 s

### Stability
- [ ] 24 h: no obvious memory growth, no thread/COM/GPU-resource leaks
- [ ] Explorer restart, device loss, decoder failure, missing/corrupt files, config corruption all handled
- [ ] Deterministic clean shutdown, no hanging process

### UI
- [ ] Native Win32 UI: Home, Library (v1: add/remove/list/metadata/preview), Playlists, Monitors, Performance, Settings
- [ ] System tray (play/pause/next/prev/settings/exit); UI closed ⇒ engine keeps running, UI resources released
- [ ] Single instance
- [ ] V2 (not v1 acceptance): async bounded thumbnails, drag & drop, global hotkeys, debug overlay, shared-playlist mode

### Reporting
- [ ] README covers overview/architecture/build/run/codecs/HW accel/multi-monitor/perf/game detection/config/troubleshooting/limitations/dev/testing
- [ ] Final report with measured numbers or explicit `NOT MEASURED` for every metric in doc 3 §101's 22-point list
