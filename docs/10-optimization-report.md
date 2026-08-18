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

## Acceptance (spec §57)

The §57 targets (1080p60: CPU <1 %, RAM <200 MB, VRAM <250 MB, GPU ~1–3 %, FPS 60, 0 drops) are **hardware-decode targets**. This machine has **no working hardware MFT** (verified M5/M14 — NV12 negotiates but the decoder hands back system-memory samples), so decode is inherently software — the documented deviation: *"if hardware limitations prevent the target, maintain quality and explain the bottleneck"* (kept, this document + `docs/09`).

What this work delivers on the software path:
- (a) **the software path at its floor** — no CPU color conversion, minimum (native-format) upload, no redundant per-frame GPU work;
- (b) **measured before/after** (above) — CPU −25 % total / −58 % per frame, RAM −22 MB, decode 2×, presentation 32 → ~57 fps;
- (c) **no quality/FPS regression** — same BT.709 limited→full-range conversion (now GPU), render time unchanged (0.4 ms), and the remaining drops are the documented freshness policy, not lost frames;
- (d) remaining gap to §57 is the absent hardware decoder (explained above) — with a hardware MFT, B3 removes the per-frame SRV allocation and B1's NV12 upload feeds the same shader.

**Not measured (reasons recorded):** VRAM + GPU engine % (no per-process sampler — M14 audit), Frame P95 (app tracks 1 s averages, not percentiles), 4K/AV1/HDR rows (no hardware MFT / no such clips — M14 report), real multi-monitor (single display).
