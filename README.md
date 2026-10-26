# xfsr

[![Release](https://img.shields.io/github/v/release/jersobh/xfsr?display_name=tag&sort=semver)](https://github.com/jersobh/xfsr/releases/latest)
[![Build](https://github.com/jersobh/xfsr/actions/workflows/release.yml/badge.svg)](https://github.com/jersobh/xfsr/actions/workflows/release.yml)
[![Platform](https://img.shields.io/badge/platform-Linux%20x86__64-lightgrey.svg)](https://github.com/jersobh/xfsr/releases)

FSR 1.0 for Linux games — wrap a title like `gamemoderun`, get more FPS by rendering lower and upscaling.

**Current version:** [0.5.0](https://github.com/jersobh/xfsr/releases/tag/v0.5.0)

**Roadmap:** [ROADMAP.md](ROADMAP.md) — adaptive FSR, profiles, auto-tune, dma-buf.  
**Not in the layer:** true FSR 2/3/4 (need game motion vectors / AMD+Proton DLL upgrade — see ROADMAP §2).

## What it does

Games spend most of their time drawing each frame. **xfsr** asks the game to draw at a smaller resolution (for example 960×540 instead of 1920×1080), then upscales the image to your screen with **FSR 1.0**, **NIS-style**, or a cheap **linear blit**.

You do **not** need the game to support FSR. xfsr hooks in through a **Vulkan layer** (same idea as MangoHud): it sits between the game and the GPU driver and rewrites the swapchain.

**Works with:** native Vulkan, Proton/DXVK, and OpenGL via Zink (with a small `LD_PRELOAD` shim that fixes viewports).  
**Modes:** same-GPU (default, peak FPS), `--hybrid` (dGPU render + iGPU `edge` upscale + one-way present), or `--offload` (often slower round-trip).  
**Tested on:** hybrid laptops (e.g. NVIDIA dGPU + AMD iGPU).

### Max FPS on hybrid NVIDIA + AMD (e.g. GTX 1650 + Vega)

1. **Same-GPU first** for peak FPS — round-trip `--offload` often halves bench FPS.
2. **Cheapest upscaler first:** `blit` → `nis` → `edge` → `fsr1 --no-rcas`.
3. **Use the iGPU usefully with `--hybrid`:** dGPU renders low-res only; Vega runs the `edge` upscaler and presents (no full-res back-haul). Confirm `mode=hybrid-edge`. If one-way WSI fails, falls back to same-GPU `edge` (never silent round-trip).
4. **Harder preset when GPU-bound:** `ultra` / `performance`, or `--adaptive --target-fps 60`.
5. **Confirm engagement:** `xfsr-run --layer-dir ./build --check` → `mode=proxy*` / `hybrid-edge` and NVIDIA discrete for the game.

```text
xfsr-run --mangohud --upscaler blit --preset ultra --no-rcas --overlay %command%
xfsr-run --hybrid --preset ultra --overlay %command%
xfsr-run --adaptive --target-fps 60 --upscaler nis --no-rcas --overlay %command%
xfsr-run --zink --upscaler blit --preset ultra --no-rcas glmark2
```

**Any title (CS2 / Village / DXVK that stay identity):** on **AMD/Deck**, nest in gamescope:

```text
xfsr-run --any --preset ultra --overlay %command%
```

On **NVIDIA proprietary** the nest often stays black (game can still render). `xfsr-run` skips gamescope there and uses the Vulkan layer — set a low **Borderless** res in-game (e.g. 1280×720) so the layer can upscale. `XFSR_GAMESCOPE_FORCE=1` retries the nest.

```text
xfsr-run --preset quality --overlay %command%
```

**Vulkan layer** only upscales when the game creates a smaller swapchain (`proxy ready`). If you see `identity` at desktop size, lower in-game res (NVIDIA) or use `--any` (AMD).

Avoid for FPS: plain `--offload` round-trip, `XFSR_IGPU_PRESENT=force` with MangoHud, Chrome + Wayland as an FSR test.

### Modes

| Mode | How | When to use |
|------|-----|-------------|
| **Same-GPU** (default) | Game renders low-res on the GPU it already uses; that same GPU upscales and presents | **Peak FPS** |
| **`--hybrid`** | dGPU low-res → transfer → iGPU `edge` upscale + **one-way present** (no back-haul) | Free dGPU for the game when one-way WSI works; quality on Vega |
| **`--offload`** | Dual-GPU with possible round-trip back to dGPU WSI | Experiments — usually **not** max FPS on PCIe hybrids |

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

**Any Steam / Proton / DXVK / Source 2 title** (recommended):

```text
xfsr-run --any --preset ultra --overlay %command%
```

**Layer-only** (when `xfsr-run --check` shows `proxy*` and the game honors shrink):

```text
xfsr-run --overlay --upscaler edge --preset ultra --no-rcas --no-offload %command%
```

Do **not** add `--zink` for native Vulkan. Prefer same-GPU over `--offload` for peak FPS.

You should see `xfsr-run: layer …` then **`[xfsr] layer active:`** (and later `force-scale` / `proxy ready` / `hotkeys:`) in the Steam console. If Mega Man / Proton never prints `[xfsr]`, the layer `.so` failed to load inside steamrt — reinstall from this tree (`./scripts/install-xfsr-run.sh`; the layer is linked with a pressure-vessel host-lib RPATH), fully quit Steam, clear leftover env, relaunch Steam, and retry.

**32-bit Windows titles** (CoD4:MW, many 2000s D3D9 games): the layer is **x86_64 only**. Classic Proton loads 32-bit DXVK, so you can get `[xfsr] layer active` from helpers while the game never hits the layer (no `hotkeys:` / `proxy ready` / overlay). Force 64-bit DXVK via Wine Wow64:

```text
PROTON_USE_WOW64=1 gamemoderun xfsr-run --mangohud --preset performance --overlay --no-rcas %command%
```

Confirm with `hotkeys:` or `identity overlay armed` / `proxy ready` in the Steam console. Prefer **borderless windowed** so FSR can shrink (exclusive fullscreen often locks `app == display`).

Clear leftover test env before starting Steam (empty `VK_ICD_FILENAMES=` or stale `XFSR_OUTPUT_*=800x600` will break games):

```fish
set -e GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE ENABLE_XFSR
set -e XFSR_OUTPUT_W XFSR_OUTPUT_H XFSR_DISPLAY_EXTENT XFSR_OFFLOAD XFSR_VERBOSE
set -e VK_ADD_IMPLICIT_LAYER_PATH VK_LAYER_PATH VK_IMPLICIT_LAYER_PATH VK_ICD_FILENAMES
```

Confirm the layer is live: `xfsr-run --check`, or `XFSR_VERBOSE=1` (look for `proxy ready` / `force-scale` / `first present ok`). Auto-tune a preset: `./scripts/xfsr-auto-tune --apply`.

**OpenGL only** (glmark2, native GL games) — X11 (do not set `XFSR_ZINK_WAYLAND=1`):

```text
xfsr-run --zink --mangohud --upscaler blit --preset ultra --no-rcas %command%
```

## Usage

```sh
xfsr-run --help
xfsr-run --mangohud --upscaler blit --preset ultra --no-rcas --overlay vkcube
xfsr-run --zink --upscaler blit --preset ultra --no-rcas glmark2
xfsr-run --layer-dir ./build --check
```

| Flag | What it does |
|------|----------------|
| `--preset quality\|balanced\|performance\|ultra` | How much to lower render resolution (`ultra` = 2.5×) |
| `--fsr 1` | FSR1 upscale (default) |
| `--fsr 0` / `--blit` | Same scale, linear blit instead of FSR1 (launch as F3 off) |
| `--upscaler fsr1\|blit\|nis\|edge` | Spatial upscaler (`edge` = heavier; hybrid default) |
| `--edge-mode quality\|perf` | EdgePass quality (luma cubic + padded CAS) or perf/lite |
| `--edge-sharpness 0..1` | EdgePass post-scale sharpen (default 0.45) |
| `--hybrid` | Offload + one-way + `edge`; same-GPU fallback if one-way fails |
| `--gamescope` | Nest game at preset render size; gamescope upscales to `--output` (CS2) |
| `--gamescope-filter fsr\|nis\|linear\|…` | Override gamescope `-F` (default from `--upscaler`) |
| `--native` | No upscale — passthrough swapchain |
| `--no-rcas` | Skip sharpening — usually more FPS |
| `--offload` | dGPU render, iGPU FSR + present (one-way). Skip for CS2 if unstable |
| `--one-way` / `--no-one-way` | iGPU present (default with `--offload`) or round-trip. One-way skips MangoHud and pins the xfsr overlay |
| `--mangohud` | FPS overlay. Not loaded on one-way; use the xfsr overlay instead |
| `--controls` / `--no-controls` | In-game hotkeys + status toast (default on) |
| `--overlay` | Pin the status strip at launch (`XFSR_OVERLAY=1`; Proton/Steam often eat F10) |
| `--zink` | OpenGL via Zink + viewport shim (off by default) |
| `--print` | Show the env xfsr would set, then exit |
| `--no-profile` | Ignore `~/.config/xfsr/profiles.conf` |
| `--caps-shrink` / `--no-caps-shrink` | Allow/forbid smaller `maxImageExtent` (CS2 often needs off) |
| `--adaptive` / `--target-fps N` | Continuous render scale toward a frame-time target |
| `--check` | Smoke-test: confirm proxy FSR vs identity after a short bench |

### Per-game profiles

On first run, `xfsr-run` copies [share/xfsr/profiles.example.conf](share/xfsr/profiles.example.conf)
to `~/.config/xfsr/profiles.conf`. Matching uses `SteamAppId`, then executable name, then a
command-line substring. CLI flags always override the profile. See [ROADMAP.md](ROADMAP.md) §3.

Auto-pick a preset for the built-in Vulkan load app (or your `SteamAppId`):

```sh
./scripts/xfsr-auto-tune --seconds 5          # recommend
./scripts/xfsr-auto-tune --apply              # write ~/.config/xfsr/profiles.conf
SteamAppId=730 ./scripts/xfsr-auto-tune --apply
```

Adaptive (continuous scale toward a target FPS) and a smoke check:

```sh
xfsr-run --adaptive --target-fps 60 --upscaler blit --no-rcas --overlay %command%
xfsr-run --layer-dir ./build --check
```

### In-game controls (default on)

| Key | Action |
|-----|--------|
| **F1** | Cycle preset (**live** via swapchain recreate when the title handles `OUT_OF_DATE`) |
| **F2** | Toggle RCAS (**live**); on `edge`, toggles quality ↔ perf |
| **F3** | Cycle upscaler FSR1 → blit → NIS → edge (**live** when passes exist) |
| **F4** | Toggle offload same-GPU ↔ round-trip (**live**; brief hitch) |
| **F5** | Toggle one-way preference (**restart** to move WSI) |
| **F10** | Pin / unpin the status overlay (**live**) |

A toast appears for a few seconds after each change (and stays if pinned). Keys are read via X11 and/or `/dev/input` (evdev). Set `XFSR_HUD_MOD=1` if you want **Shift+F*** to avoid clashing with game binds. For Proton titles where F10 does nothing, launch with `--overlay` or:

```sh
echo overlay 1 > "$XDG_RUNTIME_DIR/xfsr-control"
```

If hotkeys still do nothing, check the console for `hotkeys: x11=… evdev=…` and use the control file:

```sh
echo preset ultra > "$XDG_RUNTIME_DIR/xfsr-control"
echo rcas 0 >> "$XDG_RUNTIME_DIR/xfsr-control"
```

Commands: `preset …`, `cycle_preset`, `rcas 0|1`, `edge_sharp 0..1`, `edge_mode quality|perf`,
`fsr 0|1`, `upscaler …`, `offload 0|1`, `one-way 0|1`, `overlay 0|1`, `status`.
`offload` applies live (round-trip); `one-way` needs a restart.

**Exclusive fullscreen / D3D9 / CS2:** Many titles create a full-desktop swapchain and
ignore caps shrink → **identity** (looks unchanged; no upscale). For a real proxy:
use **borderless windowed** and set an **in-game resolution below the desktop**
(e.g. 1280×720 on 1080p). Look for `proxy ready: render=… display=…` or
`mode=proxy-edge` / `hybrid-edge` — not `identity` / “wanted render … identity”.
CS2 profile keeps `caps_shrink=0`; drop `-fullscreen` from launch options.
Opt out of caps clamping with `XFSR_CAPS_SHRINK=0` when needed.

Offload knobs (env; same as the flags above):

| Env | Effect |
|-----|--------|
| `XFSR_HUD=0` | Disable in-game controls / overlay |
| `XFSR_OVERLAY=1` | Pin status strip at launch (same as `--overlay`) |
| `XFSR_HUD_MOD=1` | Require Shift+F* for hotkeys (default: bare F-keys) |
| `XFSR_CONTROL=/path` | Override control-file path (default `$XDG_RUNTIME_DIR/xfsr-control`) |
| `XFSR_IGPU_PRESENT=0` | Force round-trip (dGPU WSI) — same as `--no-one-way` |
| `XFSR_IGPU_PRESENT=1` | One-way iGPU present (any vendor). Skips MangoHud |
| `XFSR_IGPU_PRESENT=force` | One-way even if MangoHud is still loaded (often SIGSEGV) |
| `XFSR_DMABUF=1` | Force dma-buf transfer try (default skips NVIDIA↔Mesa) |
| `XFSR_DMABUF=0` | Never use dma-buf |
| `XFSR_VERBOSE=1` | Log `offload: …`, `transfer backend=…`, `first present ok` |

### OpenGL / Zink

Pass **`--zink`** so `xfsr-run` sets Mesa Zink and preloads `libxfsr_gl_shim.so` (remaps `glViewport` / `glScissor`). Without `--zink`, native GL never hits the Vulkan layer — `--no-zink` on an OpenGL binary cannot upscale. Vulkan, DXVK, and Zink share one scale: render = window / preset, present = window, windowed or fullscreen. Native Vulkan `--offload` presents on the iGPU (one-way). Zink still round-trips: the NVIDIA driver presents the app swapchain itself, and an opaque one-way handle crashes it. `--no-one-way` forces round-trip for Vulkan too.

**Wayland:** Vulkan FSR works (shrink-kick). OpenGL/Zink FSR works too — the GL shim shrinks `wl_egl_window` to render size so Zink asks for a shrunk swapchain. `xfsr-run --zink` still prefers X11 when both are available (often faster on hybrid NVIDIA+AMD); keep pure Wayland with `XFSR_ZINK_WAYLAND=1`. Native VK: `XFSR_VK_WAYLAND=1`.

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

- [`VERSION`](VERSION) — **0.5.0**
- [`CHANGELOG.md`](CHANGELOG.md)
- Tag `vX.Y.Z` → GitHub Actions builds the Linux package the install script downloads

```sh
git tag v0.5.0 && git push origin v0.5.0
```

## Credits

FSR 1.0 headers from AMD FidelityFX (`third_party/ffx-fsr/`).
