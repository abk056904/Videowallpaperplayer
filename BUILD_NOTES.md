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
- RTX 3050 NVDEC supports H.264 / HEVC / AV1 hardware decode — M5 hardware-decode verification is feasible on this machine.
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
