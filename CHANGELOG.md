# Changelog

All notable changes to **xfsr** / **xfsr-run** are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [0.1.0] — 2026-10-24

### Added

- Project renamed to **xfsr** (layer `VK_LAYER_XFSR`, env `XFSR_*`; legacy `CROSSVULKAN_*` still accepted)
- `xfsr-run` Vulkan layer installer and Steam-friendly launcher
- Same-GPU FSR1 upscale (default) and optional `--offload` dual-GPU present
- Presets: quality / balanced / performance
- `--no-rcas` (EASU-only) for higher FPS
- Wayland display-extent hint (`--output` / `XFSR_DISPLAY_EXTENT`)
- OpenGL/Zink FSR via `libxfsr_gl_shim.so` (viewport remap) — same-GPU and `--offload`
  (Zink+`--offload`: dGPU WSI + iGPU FSR round-trip; avoids NVIDIA GLX crash on opaque iGPU WSI)
- Fix GL shim: export interpose symbols + wrap `glX`/`egl`/`glfw` GetProcAddress
  (hidden visibility had left `glViewport` out of the dynsym table → corner crop)
- Fix GL shim: interpose `dlsym` so glmark2 (dlopen libGL) gets remapped viewports
- `xfsr-bench.sh` / `xfsr_load_vk` / `xfsr_load_gl` uncapped load tools
- One-line install script and GitHub Actions release packages

### Fixed

- Postage-stamp on Wayland from invented `app × scale` display sizes
- Dual-GPU `SURFACE_LOST` / early `OUT_OF_DATE` (opaque app handle + iGPU-only window WSI)
- Caps-shrink vs Zink EGL drawable mismatch

[0.1.0]: https://github.com/jersobh/xfsr/releases/tag/v0.1.0
