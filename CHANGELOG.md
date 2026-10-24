# Changelog

All notable changes to **xfsr** / **xfsr-run** are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [0.3.1] — 2026-10-24

### Fixed

- `glmark2-wayland` postage-stamp: don’t caps-shrink from `DISPLAY_EXTENT` alone;
  skip GL viewport remap when the EGL drawable is still window-sized (Wayland Zink
  identity path). X11 `glmark2` keeps FSR via caps-shrink.

## [0.3.0] — 2026-10-24

### Fixed

- Zink/glmark2 windowed zoom-crop: GL shim remaps with **window/scale**, not the
  1080p `XFSR_RENDER_*` hint (800×600 was forced to 1280×720 / 768×432 → bottom-left
  blow-up). `glfwGetFramebufferSize` uses the same rule.

## [0.2.4] — 2026-10-24

### Added

- `xfsr-run --one-way` / `--no-one-way` (sets `XFSR_IGPU_PRESENT`; `--one-way` implies
  `--offload`)

### Fixed

- NVIDIA `--one-way`: opaque WSI SIGSEGV on swapchain recreate / MangoHud — ignore
  one-way on NVIDIA unless `XFSR_IGPU_PRESENT=force`
- `glmark2-wayland`: keep `WAYLAND_DISPLAY` for `*-wayland` binaries (was cleared for
  Zink and failed canvas init)
- Zink windowed postage-stamp: don’t treat `DISPLAY_EXTENT` hint as the real window;
  only caps-honor when app extent matches expected render size; tighten GL shim remap;
  caps-shrink for Zink on iGPU too

### Changed

- Document offload knobs (`XFSR_IGPU_PRESENT`, `XFSR_DMABUF`) in README

## [0.2.3] — 2026-10-24

### Added

- Offload present policy: try **one-way iGPU present** on Mesa dGPUs; auto-fallback
  to **round-trip** (dGPU WSI) on create/present failure; `XFSR_IGPU_PRESENT=0|1`
- `FrameTransfer` backends: **dma-buf** → shared-host → memcpy (pipelined for
  dma-buf/host); `XFSR_DMABUF=0|1` override. NVIDIA↔Mesa skips dma-buf by default
  (probes OK then DEVICE_LOSTs on RADV)

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
- NVIDIA hybrids: `--offload` defaults to round-trip (one-way opt-in via
  `XFSR_IGPU_PRESENT=1`)

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

[0.3.1]: https://github.com/jersobh/xfsr/releases/tag/v0.3.1
[0.3.0]: https://github.com/jersobh/xfsr/releases/tag/v0.3.0
[0.2.4]: https://github.com/jersobh/xfsr/releases/tag/v0.2.4
[0.2.3]: https://github.com/jersobh/xfsr/releases/tag/v0.2.3
[0.2.0]: https://github.com/jersobh/xfsr/releases/tag/v0.2.0
[0.1.0]: https://github.com/jersobh/xfsr/releases/tag/v0.1.0
