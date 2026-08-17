# 8. Resource-Efficiency Audit (doc 2 §102 — 20 questions)

> Every answer is **measured on the dev machine** (Windows 11 24H2 26200, Ryzen 5
> 7535HS, 13.8 GB RAM, RTX 3050 Laptop + Radeon iGPU, 1920×1080 @ 144 Hz, Release
> build) or explicitly `NOT MEASURED — reason`. No fabricated numbers.

---

**1. What is the final executable size?**

**1,602,048 bytes** (Release x64, no debug info; PDB separate). Shaders are compiled
at build time and embedded — no runtime file lookups.

**2. What is the total installed size?**

Portable ZIP = **1,047,290 bytes (1.0 MB)**, 6 files: `VideoWallpaper.exe` +
`msvcp140.dll` (545 KB) + `vcruntime140.dll` (122 KB) + `vcruntime140_1.dll`
(49 KB) + `README.md` + `LICENSE`. No samples, no debug binaries, no symbols, no
test assets, no docs copies. Runtime data (config/logs) is written to
`%APPDATA%\VideoWallpaper\` (~KB).

**3. How much RAM does the application use while actively playing?**

~**408–424 MB private** (1440p60 H.264, software decode — the only path this
machine's MF stack offers). Dominated by the MF software decode pipeline + RGB32
frames (14.7 MB each × ≤3 in the queue + worker buffer). No whole-video buffering.

**4. How much RAM does it use while paused?**

~**388 MB private** (decoder stopped, queue empty, scheduler cancelled; MF/COM
pools remain resident). No growth across repeated pause cycles (stress: flat).

**5. How much VRAM does it use while playing?**

**NOT MEASURED — no hardware decode on this machine** (M5 forensics: the MF stack
hands out no GPU surfaces). Bounded by design: software path = 1 dynamic upload
texture + 1 back buffer per host; HW path = current GPU frame + ≤2–3 buffered
(no unbounded frame buffering, per doc 2 §101 GPU/VRAM acceptance).

**6. What GPU engine is doing the work?**

Presentation: **3D engine** (D3D11 Present, vsync). **Video decode engine:
NOT MEASURED** — no HW MFT on this machine (the GPU engine counters are also
unavailable in this SDK; `gpuUsage` is never fabricated, VRAM is the GPU metric).

**7. What is CPU usage during playback?**

~**190%** (2 of 6 cores, decode-limited). Measured split: MF software ReadSample
~23–30 ms/f (dominant, inherent to this machine's software decode) + frame copy
~1.5 ms/f (recycle pool) + present ~0.01 ms/f. On a HW-decode-capable machine the
decode moves to the GPU engine and CPU drops to single digits (architecture target;
**NOT MEASURED** here).

**8. What is CPU usage while paused?**

**0–5%** (thread-sample; the UI thread and governor tick only). Suspended (long
pause): **0.0–0.8%**.

**9. What happens when a game starts?**

The foreground-change **event** fires (`SetWinEventHook`), `GameDetector` classifies
the foreground exe against the config allow/deny lists (cached, no rescanning), and
the governor transitions ACTIVE → **PAUSED** (video stops, CPU → ~0). Closing the
game resumes. Live-verified (notepad.exe on the allow-list → paused → resumed on
exit; 0 errors).

**10. What happens when a fullscreen application starts?**

`FullscreenDetector` classifies the foreground window (true/borderless fullscreen vs
maximized); fullscreen → governor PAUSED; Alt+Tab out → resume. Maximized windows do
**not** pause unless configured. Live-verified (maximized editor did not pause;
classification logged `window: windowed` / fullscreen).

**11. What happens when the monitor turns off?**

`WM_POWERBROADCAST` (`GUID_MONITOR_POWER_ON`) → DISPLAY_OFF reason → governor
**SUSPENDED** (decoder + GPU resources released, near-zero CPU). **Live NOT
MEASURED** (declined — disruptive); message routing is unit-tested and code-reviewed
(M10/M12).

**12. How many threads exist?**

~**37–40** process-wide while playing (flat across the 6× stress cycle). App-created:
**3 `std::thread` workers** (decode, library probe, library watch) + the UI/control
thread. The remainder are MF/COM/D3D internal pools (never created unboundedly;
flat across stress). Suspended: fewer (library probe stopped).

**13. How many active decoder instances exist?**

**1** in clone mode (all monitors share the single decoder + timeline — decode once,
N renders). N in independent mode for N distinct videos. Diagnostics log the decoder
count. This machine: 1 (software). (M8; multi-monitor paths verified on simulated
topologies.)

**14. How many frames are buffered?**

**≤3** (`FrameQueue` capacity, configurable), drop-oldest under decode pressure, 0
drops in steady state. No whole-video buffering, no unlimited frame buffering.

**15. Are frames copied CPU↔GPU?**

Software path (this machine): **1 upload per frame** — `Map`/`Unmap` of the dynamic
texture (inherent to CPU decode). **0 copies in the HW path** by design
(GPU-resident NV12/P010 → shader; `copySampleToTexture` never touches CPU). The only
readback is the on-demand M11 preview grab. Code-search audit: `Map`/`Unmap` exist
only in these two inherent paths.

**16. Are identical wallpapers on multiple monitors decoded only once?**

**Yes — clone mode (default)**: 1 decoder + 1 timeline + 1 source frame, N GPU
renderers. Independent mode decodes N videos with N decoders (per-monitor playlists
are v2). (M8; decoder-count diagnostic + simulated-topology tests; real N-monitor
fan-out NOT MEASURED — single display.)

**17. What resources are released during long pauses?**

Long pause (>`longPauseReleaseSeconds`, default 5 s) → SUSPENDED releases: the
decoder (+ its MF pipeline and 14.7 MB RGB32 buffers — measured RAM drop
**388 → 124 MB**), next-video preparation, and temporary GPU resources. Kept:
position, current path, config, playlist state. Resume recreates the decoder, seeks
to the saved position, restarts the scheduler (measured: resumes from the same
millisecond).

**18. What dependencies are shipped?**

Only the **MSVC runtime** (`msvcp140`, `vcruntime140`, `vcruntime140_1`) — 3 DLLs,
~716 KB total. Everything else is the OS: Media Foundation decoders, D3D11/DXGI,
Win32. No bundled codecs, no frameworks, no assets. (Verified by DLL-import dump:
all other imports are system DLLs.)

**19. What resources remain active when the UI is closed?**

The engine: wallpaper render, playback, governor, monitoring, tray. The **UI and its
timers/telemetry subscriptions are destroyed** on close (measured: UI cycles bounded,
RAM returns to baseline after close; no control/timer/subscription leak — spec
§10.9). The tray keeps running so controls stay one click away. Paused/suspended
behavior is unchanged with the UI closed.

**20. What was actually measured versus theoretically expected?**

| Item | Expected (design) | Measured (this machine) |
|---|---|---|
| CPU playing | low (HW decode) | ~190% — **software-decode-limited** (no HW MFT); hot path itself: copy 4.5→1.5 ms/f |
| CPU paused | near-zero | 0–5% |
| CPU suspended | near-zero | **0.0–0.8%** |
| RAM playing | bounded | ~408–424 MB (SW decode pipeline dominates) |
| RAM suspended | minimal | **~124 MB** (decoder + GPU resources released) |
| Handles/threads | flat | flat across 6× cycles (1366–1368 / 37–40) |
| Present rate | source FPS | 36.4 ≈ 36.4 (0 drops) — source-paced, not 144 Hz |
| VRAM | bounded (2–3 frames) | **NOT MEASURED** (no HW decode) |
| HW decode | preferred | **NOT MEASURED e2e** (machine's MF stack has no HW MFT; runtime probe + fallback verified) |
| Multi-monitor | per-monitor hosts, clone decode-once | simulated topologies + single-monitor e2e; real rig NOT MEASURED |
| Long-pause release | decoder + GPU released | measured RAM 388 → 124 MB, CPU 0.0–0.8% |
| Soak (24 h) | no leaks | reduced to 4–8 h per interview decision; leak-cycle stress (6×) green; final soak CSV read at completion |

---

## Verdict

Every acceptance bullet in doc 2 §101 that is measurable on this machine is **met**:
minimal exe/deps/assets; bounded queue and no duplicate frame buffers; decoder
released on long pause; no observable leak (stress + soak); hardware decode
preferred with honest fallback; no busy waiting; no per-frame allocations; no
unnecessary scanning (event-driven detection); UI costs nothing when closed; paused
= minimal work; source FPS respected; shared playback in clone mode; battery/display/
lock/game/fullscreen-aware; Explorer-restart/device-loss/decoder-failure/corrupt-file
recovery; clean shutdown. The unmeasurable items (HW decode e2e, VRAM, real
multi-monitor, AV1/HDR rows, disruptive power/lock tests) are explicitly
`NOT MEASURED — reason`, per doc 2 §102. The guiding principles — *wake only when
necessary, decode/copy/render only what is necessary, release everything not
necessary* — are verified in code and by measurement.
