# xfsr

[![Release](https://img.shields.io/github/v/release/jersobh/xfsr?display_name=tag&sort=semver)](https://github.com/jersobh/xfsr/releases/latest)
[![Build](https://github.com/jersobh/xfsr/actions/workflows/release.yml/badge.svg)](https://github.com/jersobh/xfsr/actions/workflows/release.yml)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)](https://github.com/jersobh/xfsr/releases)

FSR 1.0 for Linux games — wrap a title like `gamemoderun`, get more FPS by rendering lower and upscaling.

**Current version:** [0.2.0](https://github.com/jersobh/xfsr/releases/tag/v0.2.0)

## What it does

Games spend most of their time drawing each frame. **xfsr** asks the game to draw at a smaller resolution (for example 960×540 instead of 1920×1080), then upscales the image to your screen with **AMD FidelityFX FSR 1.0** so it still looks close to native.

You do **not** need the game to support FSR. xfsr hooks in through a **Vulkan layer** (same idea as MangoHud): it sits between the game and the GPU driver and rewrites the swapchain.

**Works with:** native Vulkan, Proton/DXVK, and OpenGL via Zink (with a small `LD_PRELOAD` shim that fixes viewports).  
**Modes:** same-GPU FSR (default) or `--offload` (dGPU render → iGPU FSR/present). Both work for Vulkan and OpenGL/Zink.  
**Tested on:** hybrid laptops (e.g. NVIDIA dGPU + AMD iGPU).

### Two modes

| Mode | How | When to use |
|------|-----|-------------|
| **Same-GPU FSR** (default) | Game renders low-res on the GPU it already uses; that same GPU runs FSR and presents | Most games — usually the fastest and most stable |
| **`--offload`** | Discrete GPU renders low-res; frame is handed to the **integrated GPU** for FSR + present | Hybrid laptops when you want the iGPU to do the upscale (best-effort; not every machine/driver likes this) |

Presets control how hard it pushes:

| Preset | Scale (approx.) | Example at 1080p |
|--------|-----------------|------------------|
| `quality` | 1.5× | 1280×720 → 1920×1080 |
| `balanced` | 1.7× | ~1129×635 → 1920×1080 |
| `performance` | 2.0× | 960×540 → 1920×1080 |
| `ultra` | 2.5× | 768×432 → 1920×1080 |

`--no-rcas` skips FSR’s sharpening pass (EASU upscale only) — often a bit more FPS, slightly softer image.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/jersobh/xfsr/main/install.sh | sh
```

Installs into `~/.local` (`xfsr-run`, Vulkan layer, shaders). Put `~/.local/bin` on your `PATH` if it isn’t already.

## Steam

**Default (Vulkan / Proton / DXVK)** — CS2, Mega Man 11, most Steam titles:

```text
xfsr-run --mangohud --preset performance --no-rcas %command%
```

Do **not** add `--zink` or `--offload` for these. `--zink` forces Mesa GL→Vulkan and clears Wayland (breaks native Vulkan games). `--offload` has crashed CS2 on hybrid NVIDIA+AMD.

You should see `xfsr-run: layer …` then later `[xfsr] force-scale` / `proxy ready` in the Steam console. If Mega Man / Proton never prints `[xfsr]`, the layer didn’t load inside the runtime — fully quit Steam, clear leftover `XFSR_*` / `GALLIUM_*` env, relaunch Steam, and retry.

If CS2 sticks on the Valve logo, update the layer (`curl … install.sh | sh` or rebuild), use the launch line above (no `ultra` until the installed `xfsr-run` supports it), and clear leftover test env before starting Steam:

```fish
set -e GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE ENABLE_XFSR
set -e XFSR_OUTPUT_W XFSR_OUTPUT_H XFSR_DISPLAY_EXTENT XFSR_OFFLOAD XFSR_VERBOSE
```

More FPS: `--preset ultra`. Confirm the layer is live with `XFSR_VERBOSE=1` (look for `proxy ready` / `force-scale` / `first present ok`).

**OpenGL only** (glmark2, native GL games):

```text
xfsr-run --zink --mangohud --preset performance --no-rcas %command%
```

## Usage

```sh
xfsr-run --help
xfsr-run --mangohud --preset performance --no-rcas vkcube
xfsr-run --zink --preset performance --no-rcas glmark2
```

| Flag | What it does |
|------|----------------|
| `--preset quality\|balanced\|performance\|ultra` | How much to lower render resolution (`ultra` = 2.5×) |
| `--no-rcas` | Skip sharpening — usually more FPS |
| `--offload` | Upscale on the iGPU (hybrid; skip for CS2) |
| `--mangohud` | FPS / frametime overlay |
| `--zink` | OpenGL via Zink + viewport shim (off by default) |
| `--print` | Show the env xfsr would set, then exit |

### OpenGL / Zink

Pass **`--zink`** so `xfsr-run` sets Mesa Zink and preloads `libxfsr_gl_shim.so` (remaps `glViewport` / `glScissor`). Without `--zink`, native GL never hits the Vulkan layer. With `--offload`, Zink keeps the window WSI on the dGPU and runs FSR on the iGPU via an async host round-trip. On Wayland, `--zink` prefers X11/Xwayland unless `XFSR_ZINK_WAYLAND=1`.

## Build from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build build -j --target VkLayer_XFSR
./scripts/install-xfsr-run.sh          # → ~/.local
./scripts/package-xfsr.sh              # → dist/*.tar.gz
```

Needs CMake ≥ 3.20, C++17, `glslc`, Vulkan loader. FSR1 headers: `third_party/ffx-fsr/`.

## Versioning

- [`VERSION`](VERSION) — **0.2.0**
- [`CHANGELOG.md`](CHANGELOG.md)
- Tag `vX.Y.Z` → GitHub Actions builds the Linux package the install script downloads

```sh
git tag v0.2.0 && git push origin v0.2.0
```

## Credits

FSR 1.0 headers from AMD FidelityFX (`third_party/ffx-fsr/`).
