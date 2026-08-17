# Video Wallpaper Engine — Implementation Plan (Docs)

This folder contains the complete implementation plan for building a **native Windows 11 video wallpaper engine** with an extreme focus on **minimum practical resource consumption** (RAM, CPU, GPU, VRAM, disk, power, background activity) while delivering smooth, hardware-accelerated video wallpaper playback.

> **Execution contract:** [`../implement-docs-plan-spec.md`](../implement-docs-plan-spec.md) — the spec on top of this plan: interview decisions (app name `Video Wallpaper`, behavior defaults, testing constraints), per-milestone acceptance mapping (§9.1), and UI panel specifications (§10). It is the source of truth for *how* the plan gets executed.

## Source documents

The plan consolidates three overlapping task specifications stored in `docs/sources/`:

| Source file | Title / focus |
|---|---|
| `sources/New Text Document.txt` | "Build a High-Performance Windows 11 Video Wallpaper Engine" — full feature spec, staged build order (14 stages), code-quality and security rules |
| `sources/New Text Document (2).txt` | "Build a Minimal-Resource Windows 11 Video Wallpaper Engine" — resource budgets, 100-item resource discipline, performance audits |
| `sources/New Text Document (3).txt` | "Project: Ultra-Low-Resource Windows 11 Video Wallpaper Engine" — architecture, resource governor, 16-phase development process, acceptance criteria |

Where the documents differ in wording they agree in substance; where they disagree, this plan takes the **most resource-conservative** interpretation.

## Document map

| File | Contents |
|---|---|
| [`01-requirements.md`](01-requirements.md) | Consolidated functional + non-functional requirements, prohibitions, default settings |
| [`02-architecture.md`](02-architecture.md) | Target architecture: module catalog, threading model, resource state machine, frame pipeline, wallpaper hosting, data structures, directory layout |
| [`03-implementation-plan.md`](03-implementation-plan.md) | The phased build plan (M0–M14, 15 milestones): tasks, files, Windows APIs, verification, exit criteria per milestone; v1 scope vs deferred items |
| [`04-testing-profiling.md`](04-testing-profiling.md) | Unit/integration test plan, performance test matrix, profiling methodology, resource budget targets, full acceptance checklist |
| [`05-decisions-risks.md`](05-decisions-risks.md) | Key technology decisions, risk register with mitigations, open questions to confirm before/during implementation |
| [`06-progress-checklist.md`](06-progress-checklist.md) | Live milestone-by-milestone checklist (M0–M14): tasks, exit criteria, and a status table to tick off as implementation proceeds |
| [`../implement-docs-plan-spec.md`](../implement-docs-plan-spec.md) | **Execution contract**: interview decisions, environment constraints, engineering standards, delivery workflow, per-milestone acceptance mapping (§9.1), UI panel specs (§10), definition of done |

## One-paragraph summary of the plan

Build a **single-process, single-executable native C++23 x64 Windows application**: (C++23 per the execution contract §7; the plan docs predate this decision)

- **Playback engine:** Windows Media Foundation (Source Reader → hardware-decoded MFT → D3D11/DXGI GPU surface), falling back to software decoding only when hardware is unavailable, with the actual decoder mode reported in diagnostics.
- **Renderer:** Direct3D 11, one fullscreen-triangle draw call per frame, NV12/P010 → RGB color conversion in a minimal pixel shader, frames kept on the GPU, never copied through CPU memory unless unavoidable.
- **Frame pacing:** a bounded 2–3 frame queue, source-FPS-aware scheduling (a 30 FPS video is not decoded or presented at 144 Hz), frame dropping and backpressure instead of latency growth.
- **Lifecycle:** a central `ResourceGovernor` decides between `ACTIVE / REDUCED / PAUSED / SUSPENDED` states based on a bitmask of pause reasons (user, game, fullscreen, high CPU/GPU/memory, battery, locked, display off, hidden). Paused ⇒ decoder and renderer stop; long pause ⇒ decoder and temporary GPU resources are released. Near-zero active work when the wallpaper cannot be seen.
- **Desktop integration:** Win32 `WorkerW`/Progman wallpaper hosting (discovered at runtime, not hardcoded), one wallpaper host per monitor, survives Explorer restarts, no keyboard focus, no click interception.
- **Controls:** Win32 UI (Home, Library, Playlists, Monitors, Performance, Settings), system tray, single instance, optional start-with-Windows via HKCU Run key. (Hotkeys, debug overlay, thumbnails, drag & drop, HDR rendering, audio, installer — deferred to v2; see `03-implementation-plan.md` §3.0.)
- **Build:** CMake with Debug/Release x64 presets, MSVC + Windows SDK only, no third-party frameworks.

The implementation is done incrementally (15 milestones, M0–M14), each ending with **build → test → fix → verify** before moving on, exactly as the source specs demand.

## How to use this plan

1. Read `01-requirements.md` to fix the target, then `02-architecture.md` for the design.
2. Follow `03-implementation-plan.md` milestone by milestone; do not skip the exit criteria.
3. Use `04-testing-profiling.md` for the test matrix and acceptance checklist at each gate.
4. Consult `05-decisions-risks.md` before committing to any flagged decision and for recovery strategies when something breaks.
