# Changelog

All notable changes to **xfsr** / **xfsr-run** are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [0.2.3] — 2026-10-24

### Fixed

- Native Vulkan `DEVICE_LOST` after first present: caps-shrink so apps honor render
  extent (unsafe force-scale alone created FB > image); disable FSR direct-to-WSI
  STORAGE path on NVIDIA
- Zink+`--offload` SIGSEGV with MangoHud: serialize proxy present vs swapchain
  recreate (`g_proxyLife`) before tearing down `FrameTransfer`
- GL postage-stamp / missing HUD under Zink+FSR: remap viewports to render size;
  use Vulkan MangoHud (GL HUD was painted on a surface XFSR never samples)
- `xfsr-bench.sh`: MangoHud on all cases; force `--zink` for GL; build with
  `XFSR_BUILD_BENCH=ON`
- Bench `xfsr_load_vk` Y-flip vs OpenGL (Vulkan NDC)

### Changed

- Caps-shrink enabled for native Vulkan by default (`XFSR_CAPS_SHRINK=0` for Source2)

## [0.2.0] — 2026-10-24

### Added

- OpenGL via Zink FSR (same-GPU and `--offload`) with `libxfsr_gl_shim.so`
- `--preset ultra` (2.5× / 768×432 @ 1080p) for weak GPUs
- Async Zink `--offload` round-trip (`transferPipeline` both ways)

### Fixed

- GL viewport corner crop: export shim symbols, wrap `glX`/`egl`/`glfw` GetProcAddress, interpose `dlsym` (glmark2)
- Zink + NVIDIA `--offload` SIGSEGV (keep dGPU WSI; FSR on iGPU via host round-trip)
- CI release: use Ubuntu 24.04 so `glslc` is available
- Steam Vulkan/Proton titles (CS2, DXVK): Zink no longer on by default (was clearing
  `WAYLAND_DISPLAY` and forcing `GALLIUM_DRIVER=zink`)
- CS2 stuck on logo: stop caps-shrinking native Vulkan (recreate storm); skip
  `QueueWaitIdle` on swapchain destroy; don’t inherit ambient `XFSR_OUTPUT_*`

### Changed

- Gate `[xfsr] presented N frames` on `XFSR_VERBOSE`
- Layer prefers `XFSR_SCALE` from `xfsr-run` when set
- `xfsr-run`: `--zink` is opt-in; Vulkan/Proton use the layer with no Zink env

## [0.1.0] — 2026-10-24

### Added

- Project renamed to **xfsr** (layer `VK_LAYER_XFSR`, env `XFSR_*`; legacy `CROSSVULKAN_*` still accepted)
- `xfsr-run` Vulkan layer installer and Steam-friendly launcher
- Same-GPU FSR1 upscale (default) and optional `--offload` dual-GPU present
- Presets: quality / balanced / performance
- `--no-rcas` (EASU-only) for higher FPS
- Wayland display-extent hint (`--output` / `XFSR_DISPLAY_EXTENT`)
- `xfsr-bench.sh` / `xfsr_load_vk` / `xfsr_load_gl` uncapped load tools
- One-line install script and GitHub Actions release packages

### Fixed

- Postage-stamp on Wayland from invented `app × scale` display sizes
- Dual-GPU `SURFACE_LOST` / early `OUT_OF_DATE` (opaque app handle + iGPU-only window WSI)
- Caps-shrink vs Zink EGL drawable mismatch

[0.2.3]: https://github.com/jersobh/xfsr/releases/tag/v0.2.3
[0.2.0]: https://github.com/jersobh/xfsr/releases/tag/v0.2.0
[0.1.0]: https://github.com/jersobh/xfsr/releases/tag/v0.1.0
