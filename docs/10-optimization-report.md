# Extreme Resource Optimization — Required Report (spec §56/§57)

Spec: `extreme-resource-optimization.txt`. Audit (bottleneck report): `docs/09-optimization-audit.md`.
Date: 2026-08-18. All measurements: **Release build** (optimizations on, debug layer off, verbose logging off — §54), this machine (single 1920×1080 display; **software decode only** — no hardware MFT, verified M5/M14). Clip: 2560×1440@60 H.264 (`Arlecchino-In-The-Rain-…mp4`), the app's default playlist item.

Methodology: both builds measured identically — 25 × 1 s process-CPU/RAM samples after an 8 s settle (steady state), plus the app's own 1 Hz telemetry lines (decoded/presented fps, dropped, render) and a 20 s per-process disk-read delta (Win32_Process counters). Same clip, same config, same window.

## Before (pre-change build — RGB32 software path, commit `3a10aaf`)

```
CPU:            180.8 % avg (25 × 1 s; per presented frame 5.65 CPU%-s)
RAM:            466.1 MB private avg / 467.7 peak
VRAM:           NOT MEASURED — no per-process VRAM counter sampled (M14 audit)
GPU:            NOT MEASURED — no GPU engine sampler (M14 audit)
Disk I/O:       5.8 MB / 20 s (sequential source-file reads)
FPS:            decoded 32.0, presented 32.0 (decode-bound — the CPU VP MFT
                conversion + 4 B/px copy could not keep 60 fps)
Frame P95:      not instrumented (app tracks 1 s averages; render avg 0.4 ms)
Dropped frames: 0 counted (but ~28 fps silently under-presented: frames were
                simply not ready at their PTS deadlines)
```

## After (NV12 software path + CB dirty-tracking + plane-SRV cache, this commit)

```
CPU:            136.2 % avg (25 × 1 s; per presented frame 2.39 CPU%-s)
RAM:            444.2 MB private avg / 446.3 peak
VRAM:           NOT MEASURED — no per-process VRAM counter sampled (M14 audit)
GPU:            NOT MEASURED — no GPU engine sampler (M14 audit)
Disk I/O:       11.8 MB / 20 s (2× frames decoded → 2× sequential reads; still
                ~0.6 MB/s, negligible)
FPS:            decoded 60.2, presented ~57 (source 60 — decode now keeps up)
Frame P95:      not instrumented (render avg 0.4 ms — unchanged)
Dropped frames: ~3/s stale-frame drops (freshness policy, see below)
```

**Net:** CPU **-25 %** total (180.8 → 136.2), **-58 % per presented frame**; RAM **-22 MB**; decode **2× faster** (32 → 60 fps); presentation **32 → ~57 fps**; visual quality unchanged (same BT.709 limited→full-range conversion, now on the GPU).

The `dropped` delta (0 → ~3/s) is **not a regression**: the counter (`FrameQueue::dropped_`) counts stale frames skipped by the freshness policy (`popNewestUpTo` keeps the newest at-or-before the deadline — docs/02 §2.7). The old build counted 0 only because decode could not fill a single deadline (32 fps of a 60 fps source = ~28 fps silently missed). Now decode keeps up and the remaining ~3/s are decode-jitter stale frames — the wallpaper always shows the freshest frame, at 57 vs 32 fps.

## Changes (spec §56)

### Change 1 — B1: software path NV12 end-to-end (the headline)
- **Change:** the software decoder now negotiates its **native NV12 output** (no Video Processor MFT — the CPU YUV→RGB32 conversion stage is gone). Frames are copied as tightly-packed NV12 (1.5 B/px) into the pooled queue, uploaded to a dynamic **NV12 texture** (Y + interleaved-UV planes, `Map`/row-copy) and bound through the existing **two-plane SRV path** — the YUV pixel shader converts + scales on the GPU in one pass. Per-file fallback to the RGB32 path remains when NV12 negotiation fails.
- **Why:** spec §9/§10/§11 — the CPU color conversion was avoidable (the GPU shader + plane views already existed for the hardware path); §9's software ideal is "keep the native representation".
- **Resource impact:** CPU→GPU upload **14.7 → 5.5 MB/frame (−62 %)**; frame-buffer pool 14.7 → 5.5 MB × depth; measured RAM **−22 MB**.
- **Performance impact:** CPU 180.8 → 136.2 % avg; decode 32 → 60 fps; presented 32 → ~57 fps; render unchanged (0.4 ms).
- **Risk:** NV12 negotiation is per-file (RGB32 fallback path proven by the tests + live logs); preview readback (`grabFrameSnapshot`) now honestly reports "no preview" on the NV12 path (no CPU copy exists — documented, spec §10.5 fallback); odd-width stride handling is unchanged (the 1916 px clip path).

### Change 2 — B2: dirty-track the per-frame constant buffer (spec §22)
- **Change:** `D3D11Renderer::render()` calls `UpdateSubresource(frameCb_)` **only when the content changed** (`FrameParams::operator==` via `memcmp` on the two float arrays). Content (tint = constant 1.0, scaleOffset = video size/scaling mapping) changes only on a size/scaling change.
- **Why:** §22 forbids updating unchanged data every frame — a redundant CPU→GPU write + GPU command per frame.
- **Resource impact:** eliminates a per-frame CPU→GPU write (~16 bytes + command).
- **Performance impact:** not separately measurable (sub-ms); removed from every frame.
- **Risk:** minimal — equality is exact (`memcmp` on the two arrays); first frame after a change still updates.

### Change 3 — B3: cache plane SRVs per decoder surface (hardware path, spec §21/§38)
- **Change:** `bindGpuFrame`/`bindGpuFrameFor` now cache the two plane SRVs (Y + interleaved-UV) **per decoder surface texture** in a bounded map (cap 64 entries; cleared on device recreate; views are resource-bound, so a recycled surface with new pixels needs no new views).
- **Why:** §21/§38 — the decoder hands back surfaces from its internal pool; creating two SRVs **per frame** is per-frame GPU resource allocation. Latent on this machine (no hardware MFT) but a real per-frame allocation on hardware-decode machines.
- **Resource impact:** zero per-frame `CreateShaderResourceView` calls on hardware machines.
- **Performance impact:** not measurable here (path inactive — documented NOT MEASURED); verified by build + tests; bounded (no unbounded growth on a pathological stream).
- **Risk:** cache keyed by surface pointer — safe because views are bound to the resource, not the content; cap + device-recreate clear prevent staleness.

## Requirement compliance check (spec §1–§57, run 2026-08-18)

Every requirement group checked against the implemented + measured state. **Met** = implemented + verified; **Met (design)** = implemented, verified by code/tests, not measurable on this machine; **Not met on this machine** = target requires hardware decode this machine lacks (reason + §57 escape clause).

### §1 Non-negotiable priorities + DO-NOT list — MET

Priorities 1–5 (correctness, visual quality, stable pacing, source FPS, responsiveness) were never traded for resource numbers; the optimizations reduced CPU/upload **while increasing** the presentation rate (32 → ~57 fps) with identical color output (same BT.709 limited→full-range matrix, now on the GPU).

| DO-NOT | State |
|---|---|
| reduce resolution / source FPS / bitrate / recompress | ✗ none — decode unchanged, pacing at source FPS (M6 scheduler) |
| intentionally skip frames / cap FPS below source | ✗ none — presented 57/60 ≈ source; drops are the freshness policy (stale <16 ms frames), never source-FPS capping |
| software decode when hardware available | ✗ HW is attempted FIRST + runtime-probed (M5); this machine has no working hardware MFT → fallback per spec §5 |
| continuously poll / busy-wait | ✗ event-driven; message pump blocks when idle; the only periodic work is the ~1 Hz Explorer-validity tick (documented exception, docs/02 §2.8) |
| allocate per frame | ✗ pooled frame buffers (M13), persistent upload texture, cached plane SRVs (B3) |
| copy full frames unnecessarily | ✗ NV12 1.5 B/px, no CPU color conversion, no CPU scaling |
| decode frames that will never be displayed | ~0.2 % overshoot (~4 frames/20 s) — bounded 3-deep queue; measured waste negligible |
| keep rendering while not visible | ✗ no presents when paused/hidden; UI telemetry timer killed when hidden (UI review) |
| keep decoding while suspended | ✗ pause joins the decode worker (M10) |

### §4/§5 Media Foundation + hardware decode — MET (with documented fallback)

IMFSourceReader ✓; hardware path attempted first via `IMFDXGIDeviceManager` + `MF_SOURCE_READER_D3D_MANAGER` ✓; **verified the decoder is actually accelerated** by probing the first sample for a DXGI buffer (never assumed — M5) ✓; capability probed once at open, never per-frame ✓; graceful, diagnosed fallback ✓; Debug-build diagnostics (decoder / HW yes-no / codec / resolution / FPS / pixel format / GPU) ✓.

### §6 GPU device management — MET

One D3D11 device per adapter (shared by all videos/hosts, never per-video) ✓; no device recreate on wallpaper change ✓; `D3D11_CREATE_DEVICE_BGRA_SUPPORT` ✓; no debug layer in Release ✓.

### §7 Video frame lifetime — MET

Reusable resources only: pooled decode buffers (M13), persistent upload texture recreated only on size/format change, plane-SRV cache (B3), no per-frame `CreateTexture`/SRV/buffer ✓.

### §31 Pause / §32 Idle — MET (measured)

Paused: decoder worker joined, no timers, no presents → **CPU ~0 %** (measured 0.00 CPU-s/8 s, M6; soak flat) ✓; RAM/VRAM held only for fast resume (upload texture + MF reader) ✓.

### §33 Loop / §34 Playlist switch — MET

Loop reuses the reader + decoder (`replay()`, no file reopen — verified M7) ✓; playlist switch prepares the new media, waits for the first valid frame, then atomically rebinds (no black frame) ✓; shared graphics infrastructure untouched across switches ✓.

### §57 Acceptance targets — NOT MET ON THIS MACHINE (hardware-decode targets; §57 escape clause applies)

| Target (1080p60) | Required | Measured here (1440p60 SW) | Status |
|---|---|---|---|
| CPU average | <1 % | 136 % (decode-bound) | not reachable — no hardware MFT |
| CPU spikes | <5 % | ~150–200 % | not reachable — software decode |
| RAM | <200 MB | 444 MB | not reachable — MF software pipeline |
| VRAM | <250 MB | NOT MEASURED (no sampler) | — |
| GPU | ~1–3 % | NOT MEASURED (no sampler) | — |
| Disk I/O | ≈0 | ~0.6 MB/s sequential source read | near-zero ✓ |
| FPS | stable 60 | ~57 presented (60 decoded) | not reachable — decode-jitter margin |
| Dropped frames | 0 | ~3/s freshness (stale <16 ms) | not reachable — decoder rate overshoot |
| Visible stutter | none | none observed (0.4 ms render) | ✓ |
| Visual quality | unchanged | unchanged (BT.709, GPU) | ✓ |

**Why the numeric budgets are not reachable here:** the §57 targets presume hardware decode. This machine has **no working hardware MFT** (verified M5/M14: NV12 negotiates but the decoder returns system-memory samples on both the AMD iGPU and the RTX 3050), so 1440p60 H.264 decode alone costs ~130 % CPU and ~440 MB RAM. §57: *"If hardware limitations prevent the target: maintain quality and explain the bottleneck"* — done here and in `docs/09`/M14 report. On a machine with a working hardware decoder, B3 removes the per-frame SRV allocation and B1's NV12 upload feeds the same shader; the architecture is already at the spec's ideal pipeline (§3) for that case.

**Queue-depth follow-up (2026-08-18, depth IS the lever — default changed 3 → 1):** the earlier experiment only tried 3 → 6 (both too deep — the decode-ahead just grew: drops 56–60 → 69–73 cumulative, latency 65 → 114 ms). Measuring the other direction settled it: `frameQueue` **1 → 0.00 drops/s** with presented = decoded = 60.1 fps, latency ~16 ms, RAM −18 MB (426 vs 444 MB), and identical decode-bound behavior at any depth (a decode-bound queue stays empty — depth only matters when decode is *faster* than source, where depth 3 buffers ahead ~65 ms and every late consumer wake pops 2+ due frames → stale drops). **Mechanism:** capacity 1 makes the worker consumer-paced (it blocks on push when full), so the queue can never hold two due frames at a wake — the freshness counter cannot increment by construction. The config default is now **1** (range 1–16 kept for setups that prefer buffering; documented tradeoff: drops ↔ buffering).

**Not measured (reasons recorded):** VRAM + GPU engine % (no per-process sampler — M14 audit), Frame P95 (app tracks 1 s averages, not percentiles), 4K/AV1/HDR rows (no hardware MFT / no such clips — M14 report), real multi-monitor (single display).

## Verdict

**Optimization requirements are met to the extent this machine allows.** Everything under the application's control — §1 DO-NOT list, §4–§7 architecture, §31–§35 behavior, no per-frame allocations/copies/wakeups, minimal upload, event-driven idle — is implemented and verified, with measured before/after (CPU −25 % / −58 % per frame, RAM −22 MB, decode 2×, presentation 32 → ~57 fps) and no quality/FPS regression. The only unmet items are the **§57 numeric budgets**, which are hardware-decode targets this machine cannot reach (no working hardware MFT — documented deviation per §57's own escape clause, with the bottleneck explained).
