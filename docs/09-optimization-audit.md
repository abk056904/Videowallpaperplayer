# Extreme Resource Optimization — Bottleneck Report & Audit

Spec: `extreme-resource-optimization.txt` (§55 mandates: inspect → profile → bottleneck report → highest-impact changes first → benchmark each).

Date: 2026-08-17. Machine: single display 1920×1080, software decode only (no hardware MFT — verified M5/M14; documented in the M14 report).

## 1. How the pipeline actually runs (audit result)

```
MF SourceReader (software decode)          ← §4/§5: HW decode attempted, absent on this machine
  ↓  NV12 (decoder native)
Video Processor MFT: NV12 → RGB32 (CPU)    ← §9/§10 VIOLATION: conversion on CPU (avoidable)
  ↓  RGB32 system memory (4 B/px)
copySampleToFrame → pooled frame.bytes     ← M13: buffers recycled (no per-frame VirtualAlloc) ✓
  ↓
uploadFrameBytes: Map + row memcpy + Unmap ← inherent CPU→GPU upload, but 4 B/px (2.7× the NV12 size)
  ↓
D3D11 dynamic texture (B8G8R8A8) + SRV     ← recreated only on size change ✓ (M5)
  ↓
psTex shader: RGB passthrough + GPU scale  ← §11 ✓ scaling is GPU; conversion already done on CPU
  ↓
Present(1) (vsync)                         ← §16 ✓ no tearing, no software vsync
```

**Render/scheduling core (audited clean):** waitable-timer + new-frame-event message loop (M6 — §12/§13/§40 ✓, zero busy-wait, PTS-based pacing via `FrameScheduler`, no `Sleep(1000/fps)`); bounded pooled frame queue (M4/M6/M13 — §17/§38 ✓); loop by `replay()` reusing reader+decoder (M7 — §33 ✓); pause joins the worker + kills timers, measured ~0 % CPU (M10/M13 — §31/§32 ✓); fullscreen pause via WinEventHook foreground events (M9 — §29 ✓, desktop-click false-pause fixed); multi-monitor decode-once in Clone + shared D3D11 device (M8 — §24/§25 ✓); logging transition-only at INFO, ~1 Hz DEBUG stats (M6 — §37 ✓); workload sampling ~2 s with hysteresis (M9 — §47 ✓); device-loss recreate path reported once per loss (M12 — §52 ✓); no `timeBeginPeriod`, no affinity/priority forcing (M6/M13 — §41/§42 ✓).

## 2. Bottlenecks found (highest impact first)

### B1 — Software path converts NV12→RGB32 on the CPU and uploads 4 B/px (the big one)
The decoder natively outputs **NV12**; the software path (`openSoftware`) forces **RGB32** output, so MF's Video Processor MFT does the YUV→RGB conversion on the CPU, and then we upload **4 bytes/pixel** to the GPU. At 2560×1440@60 that is ~14.7 MB/frame of upload + a CPU conversion of the same size, every frame. Measured playing CPU is ~190–230 % (M13 leak-cycle) — dominated by decode + this conversion.

**The spec's own software-fallback ideal (§3, §9, §10, §11) is already 80 % built:** the YUV pixel shader (`psYuv`, limited→full range added M5), plane SRVs (`createPlaneSrv`), and the two-plane bind path (`setVideoPlanes`) all exist for the hardware path. Switching the software path to **NV12 end-to-end** (decode to NV12 system memory → upload 1.5 B/px as Y + interleaved-UV planes → `psYuv` converts + scales on the GPU in one pass) removes the CPU conversion entirely and cuts upload bandwidth ~62 %.

Risk: NV12 negotiation must be verified per-file (fallback to the current RGB32 path on failure); the preview readback (`grabFrameSnapshot`, user-initiated) must convert NV12→BGRA; stride handling for odd widths (the 1916×1080 clip).

### B2 — Redundant per-frame constant-buffer update (§22)
`D3D11Renderer::render()` calls `UpdateSubresource(frameCb_)` every frame, but the CB content (tint = constant 1.0, scaleOffset = changes only on video size/scaling change) is **identical** across frames. A per-frame CPU→GPU write that only needs to happen when the values change. Small absolute cost, but it is exactly the redundant per-frame work §22 forbids.

### B3 — Per-frame plane-SRV creation on the hardware path (deferred M13 item)
`bindGpuFrame`/`bindGpuFrameFor` call `CreateShaderResourceView` twice **per decoded frame** — GPU resource allocation per frame (spec §21/§38). Latent on this machine (no hardware MFT) but a real per-frame allocation on machines that do have hardware decode. Cache the two plane SRVs per decoder texture.

### B4 — NOT bottlenecks (verified clean)
No polling, no busy-wait, no per-frame heap/COM/string allocation on the hot path (buffer pool M13; `std::format` only in DEBUG-gated stats lines), no per-frame texture/SRV recreation on the software path, no file reopen on loop, no monitor re-enumeration, no per-frame `GetSystemMetrics`-style calls (WorkloadMonitor ~2 s), tray/UI telemetry 2 Hz only while the UI is visible (UI review fix), no global timer resolution changes.

## 3. Change plan (implemented in this order, benchmarked individually)

1. **B2 fix** — dirty-track the frame CB (update only on change). Low risk, independent.
2. **B3 fix** — cache plane SRVs per decoder texture on the hardware path. Low risk (latent here).
3. **B1 fix** — software NV12 end-to-end with per-file fallback to RGB32. The headline change: CPU conversion → 0, upload 14.7 → 5.5 MB/frame, GPU does YUV→RGB+scale in one pass. Includes NV12-aware preview readback + stride handling.

Each change: build Debug + Release, full test suite, then benchmark 2560×1440@60 playback (CPU/RAM) against the previous build.

## 4. Baseline (before any change, M13 measurements)

- Playing 2560×1440@60 H.264 (software RGB32 path): **CPU ~190–230 %**, private RAM ~423–426 MB, handles ~1365, threads ~37 (leak-cycle stress, M13; soak CSV minutes 1–6 clean UI-closed baseline).
- Paused: CPU ~0–5 %, RAM ~388 MB (M13).
- Render hot path: ~1.46 ms/frame software memcpy (resize+zero eliminated by pooling, M13) + GPU present.
- 4K/AV1/HDR rows: NOT MEASURED (no hardware MFT on this machine — M5/M14).

## 5. Acceptance mapping (spec §57)

1080p60 targets (CPU avg <1 %, RAM <200 MB, VRAM <250 MB, GPU ~1–3 %, 0 drops) are **hardware-decode targets**; on this machine decode is inherently software (documented deviation — §57: "if hardware limitations prevent the target, maintain quality and explain the bottleneck"). The deliverable here is: (a) software path at its floor (no CPU color conversion, minimum upload), (b) zero per-frame redundant GPU work, (c) measured before/after (spec §56), (d) no quality/FPS regression.
