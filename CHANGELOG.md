# Changelog

All notable changes to **xfsr** / **xfsr-run** are documented here.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- `xfsr-run --any` / `--universal` — gamescope nest that forces the game window to
  the preset render size (upscales titles that keep the Vulkan layer on identity).
  NVIDIA hybrids auto-use `--backend sdl` instead of disabling the nest
- `--gamescope-backend` / `XFSR_GAMESCOPE_BACKEND`; profiles for CS2 / CoD4 / Village
  default `gamescope=1`
- Gamescope nest: `ENABLE_GAMESCOPE_WSI=1` on AMD/Deck (XWayland bypass). On
  NVIDIA default **WSI off** (Valve#1182 black/white screen; DXVK uses nested
  X11/xcb instead). Override with `XFSR_GAMESCOPE_WSI=0|1`. Never set
  `PROTON_ENABLE_WAYLAND` on NVIDIA; Wayland nest via `XFSR_GAMESCOPE_WAYLAND=1`
- Nest HUD: do not enable `--mangoapp` on NVIDIA (EGL/GLFW crash-loop); opt-in
  via `XFSR_GAMESCOPE_MANGOAPP=1`
- Nest present on Plasma Wayland: SDL host with `SDL_VIDEODRIVER=x11` (real
  KWin Xwayland window). `xfsr-gamescope-focus` sets ChimeraOS-style
  `GAMESCOPECTRL_BASELAYER_*` so the nest composites the game (desktop Steam
  leaves `FOCUSED_APP_GFX` empty → black). `-e` only with
  `XFSR_GAMESCOPE_STEAM=1`. No default `-f`
- EdgePass quality pack: shared-mem source tile, edge-stopping cubic, anti-ring,
  post-scale CAS, linear/sRGB, scale-aware sharpen; `XFSR_EDGE_MODE=perf` / `--edge-mode
  perf` lite path; live `edge_sharp` / `--edge-sharpness`; F2 toggles edge quality↔perf
  when on `edge`; STORAGE→swapchain direct on non-NVIDIA present (hybrid iGPU)
- EdgePass v2: padded display-space CAS (no 16×16 tile seams), luma-only cubic +
  chroma bilinear, Sobel/structure-tensor along-edge mix, linear anti-ring
- `xfsr_load_vk` / `xfsr_load_gl` default to run until Ctrl+C / Esc (keep `--seconds` for benches)

### Fixed

- `--gamescope-fs` / profile `gamescope_fs=1` / `XFSR_GAMESCOPE_FS=1` — gamescope
  host fullscreen (`-f`) while the game stays at nest `-w/-h` (CoD4 windowed fix)
- Gamescope nest floor 640×480 (CoD4 / D3D9 reject ultra 768×432)
- NVIDIA nest: skip gamescope by default (host stays black); use layer instead;
  `XFSR_GAMESCOPE_FORCE=1` to allow nest anyway
- Proxy acquire: finite `appImgFence` wait + re-signal on timeout (Village froze
  after `first present ok` on UINT64_MAX). Village profile: blit + caps-shrink
- `--gamescope`: no xfsr overlay (layer off) — MangoHud on the game; Zink without
  xfsr shim; match load-app window to nest size; drop nested `--debug-hud` /
  `--force-windows-fullscreen` (caused blink/swapchain thrash); warn that
  `--hybrid`/`--offload` are ignored
- Docs: prefer `xfsr-run --any` for titles that stay identity; NVIDIA nest uses
  SDL backend instead of disabling gamescope

## [0.5.0] — 2026-10-25

### Added

- `--gamescope` / profile `gamescope=1` — nest at preset render size, upscale with
  gamescope `-F` (from `--upscaler` or `--gamescope-filter`). Unblocks CS2/Source 2
  where the Vulkan layer stays identity. Layer off unless `XFSR_GAMESCOPE_LAYER=1`
- `--upscaler edge` / `EdgePass` — heavier edge-aware spatial upscaler (F3: fsr1→blit→nis→edge)
- `--hybrid` / `XFSR_HYBRID=1` — dGPU low-res → iGPU `edge` + **one-way present** (no
  full-res back-haul). Status `mode=hybrid-edge`. If one-way WSI fails, same-GPU
  `edge` fallback (refuses silent round-trip). Skips MangoHud; pins xfsr overlay
- [ROADMAP.md](ROADMAP.md) — adaptive FSR, temporal/NIS, profiles, dma-buf, auto-tune
- Per-game profiles: `~/.config/xfsr/profiles.conf` (from
  `share/xfsr/profiles.example.conf`); match Steam AppID / exe / substring;
  `--no-profile`, `--caps-shrink` / `--no-caps-shrink`
- `scripts/xfsr-auto-tune` — short preset×RCAS matrix on `xfsr_load_vk`; prints a
  profile section; `--apply` writes `~/.config/xfsr/profiles.conf`; scores
  `avg` + `p1_fps` (1% lows) and frame-time stdev
- Adaptive FSR: `--adaptive` / `XFSR_ADAPTIVE=1` + `--target-fps` — continuous float
  scale (not only the preset ladder) with pixel hysteresis before recreate
  (control: `adaptive`, `target_fps`, `scale`)
- `--upscaler fsr1|blit|nis` / `XFSR_UPSCALER` — NIS-style edge-directional spatial
  upscaler (`NisPass`)
- `xfsr-run --check` — short `xfsr_load_vk` smoke; reads
  `$XDG_RUNTIME_DIR/xfsr-status` (`mode=proxy|proxy-nis|proxy-edge|hybrid-edge|identity`)
- Overlay pins by default when MangoHud is not requested
- Bench lines include `p1_fps=` and `ft_stdev_ms=`
- One-way offload skips MangoHud and pins the xfsr overlay with FPS, CPU%, GPU%
  and temperatures (sysfs). Native Vulkan may present on the iGPU; Zink stays on
  round-trip (NVIDIA calls present with the app swapchain — opaque handles SIGSEGV)
- Uniform render size: `window / scale` for Vulkan, DXVK, and Zink (windowed and
  fullscreen). No special-case 1080p table that ignored smaller windows
- Docs: 32-bit Proton titles (CoD4) need `PROTON_USE_WOW64=1` so 64-bit DXVK loads
  the layer; borderless + lower in-game res for D3D9 FSR

### Fixed

- Zink on NVIDIA+AMD hybrids: put NVIDIA first in `VK_DRIVER_FILES` (keep Mesa
  Gallium/Zink — do **not** set `__GLX_VENDOR_LIBRARY_NAME=nvidia`, which bypassed
  Zink and killed the overlay); inject `--ozone-platform=x11` for Chrome when
  Wayland is cleared; warn on browser + Wayland
- NIS + live F4 offload: do not record iGPU `NisPass` into the dGPU present
  command buffer (SIGSEGV); use a blocking transfer→NIS→transfer round-trip
- CoD4 / D3D9: do not return proxy images smaller than CreateSwapchain extent
  (DEVICE_LOST after first present); identity + hint when the game ignores caps
- Caps advertise `currentExtent=UINT_MAX` + `max=render` for DXVK; Zink uses a
  concrete `currentExtent=render` (UINT_MAX made Mesa OOM / stay identity)
- Hook `vkGetPhysicalDeviceSurfaceCapabilities2KHR` (DXVK prefers this)
- FSR proxy + caps shrink on integrated GPUs (`discrete=0`, e.g. Vega)
- Identity / exclusive-fullscreen F10 overlay (CoD4): adopt app device, preserve
  `PRESENT_SRC` frame contents, reset overlay sync on DestroyDevice
- Zink X11: publish `XFSR_GL_REMAP` so the shim remaps viewports when EGL still
  reports the window size (zoomed bottom-left crop)
- Skip futile shrink-kick when D3D9 already ignored the caps clamp

### Changed

- Docs / profiles: hybrid max-FPS path is **same-GPU + blit/nis + ultra/adaptive**
  (not `--offload`); FSR2/3/4 called out as out of layer scope (Proton FSR4 is
  AMD-only). Steam / example launch lines and `profiles.example.conf` defaults
  follow that guidance
- `--fsr 0` / `--blit` starts with scale + linear blit (FSR1 pass off); `--native`
  is passthrough (old `--fsr 0` behavior)
- `--offload` without `--no-one-way` prefers one-way and does not load MangoHud
- `xfsr-run --one-way` / offload docs: round-trip is `--no-one-way`; Zink offload
  is always round-trip on NVIDIA hybrids

## [0.4.0] — 2026-10-25

### Added

- In-game controls (default on): **F1** preset, **F2** RCAS, **F3** FSR,
  **F4** offload, **F5** one-way, **F10** pin overlay; toast on change.
  Disable with `XFSR_HUD=0` / `--no-controls`
- Hotkeys via X11 and/or evdev (`/dev/input`); `XFSR_HUD_MOD=1` requires Shift
- Control file `$XDG_RUNTIME_DIR/xfsr-control` (or `XFSR_CONTROL`): `preset`,
  `rcas`, `fsr`, `offload`, `one-way`, `overlay`, `status`
- **F4 live offload**: same-GPU ↔ round-trip without restart (keeps dGPU WSI)
- `--overlay` / `XFSR_OVERLAY=1` — pin status strip at launch (Proton often eats F10)

### Fixed

- SIGSEGV on rapid swapchain recreate (glmark2 scenes / preset hotkey): drain present
  fences before freeing proxy; keep status overlay on the device, not the swapchain
- Clearer log when exclusive fullscreen locks size (`app == display`) so FSR cannot
  shrink — use borderless windowed
- Live F4 offload: always inject host/dma-buf device extensions on discrete CreateDevice
  so mid-run transfer init does not call null ICD entry points
- Identity-path F10 overlay + fence hang; bench recreates on `OUT_OF_DATE` (F1)
- Ambient `XFSR_IGPU_PRESENT=force` no longer overrides `XFSR_OFFLOAD=0`
- F3 FSR-on after blit / live F4 with FSR off: always allocate FSR outputs so
  `fsr.record` does not SIGSEGV
- Bench `--fullscreen` / `--windowed` for VK and GL loaders
- `xfsr-run` / `xfsr-bench`: prefer X11 for native Vulkan FSR when `DISPLAY` is
  set (Wayland `UINT_MAX` extent skipped shrink → flat FPS vs baseline)
- `xfsr-bench`: full matrix — X11+Wayland × windowed+fullscreen, all presets,
  offload±RCAS, VK+GL (`XFSR_BENCH_QUICK=1` / `XFSR_BENCH_X11_ONLY=1` /
  `XFSR_BENCH_WAYLAND_ONLY=1` to narrow)
- **Wayland Vulkan FSR**: seed window size from the first swapchain (ICD
  `currentExtent` is `UINT_MAX`), kick one `OUT_OF_DATE`, clamp caps max to
  render size so the recreate builds a real proxy (was identity @ full res)
- **Wayland Zink/GL FSR**: shim shrinks `wl_egl_window` to render size (Zink
  ignores Vulkan caps); layer treats matching create as hint-honored proxy
- Offload+MangoHud SIGSEGV on Mesa one-way (opaque WSI): refuse one-way when
  MangoHud is active; skip `--offload` when the app is already on the iGPU
  (`--igpu` / Zink-on-Vega)

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

[0.5.0]: https://github.com/jersobh/xfsr/releases/tag/v0.5.0
[0.4.0]: https://github.com/jersobh/xfsr/releases/tag/v0.4.0
[0.3.1]: https://github.com/jersobh/xfsr/releases/tag/v0.3.1
[0.3.0]: https://github.com/jersobh/xfsr/releases/tag/v0.3.0
[0.2.4]: https://github.com/jersobh/xfsr/releases/tag/v0.2.4
[0.2.3]: https://github.com/jersobh/xfsr/releases/tag/v0.2.3
[0.2.0]: https://github.com/jersobh/xfsr/releases/tag/v0.2.0
[0.1.0]: https://github.com/jersobh/xfsr/releases/tag/v0.1.0
