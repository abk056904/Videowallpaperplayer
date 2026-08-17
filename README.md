# Video Wallpaper

A native Windows 11 video wallpaper engine — play video files as animated desktop wallpapers with minimum practical resource consumption.

> **Status: under active development (M1 — build skeleton).** The full implementation plan lives in [`docs/`](docs/), the execution contract in [`implement-docs-plan-spec.md`](implement-docs-plan-spec.md), and milestone progress in [`docs/06-progress-checklist.md`](docs/06-progress-checklist.md).

## Highlights (target)

- Native C++23 x64, Win32 + Direct3D 11 + Media Foundation — no runtime frameworks
- Hardware-accelerated video decoding with GPU-resident frames
- Near-zero CPU/GPU activity when paused or invisible
- Multi-monitor, per-monitor playlists, loop/shuffle, system tray
- Apache-2.0 licensed

*(The complete README — build instructions, supported codecs, configuration, troubleshooting, performance behavior — is delivered at milestone M14 per the plan.)*

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
