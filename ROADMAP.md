# xfsr roadmap

Living plan for making xfsr more useful on Linux / Proton / hybrid GPUs.
Status keys: **planned** · **in progress** · **done** · **blocked**

Related: [research.md](research.md) (offload transfer measurements), [CHANGELOG.md](CHANGELOG.md).

---

## Priority order (implementation)

| Order | Pillar | Why first |
|------:|--------|-----------|
| 1 | [#3 Profiles](#3-intelligent-per-game-profile-database) | Unblocks every title without new launch flags; feeds auto-tune |
| 2 | [#5 Auto-tune](#5-automated-benchmark--recommendation-tool) | Writes into profiles; needs #3 |
| 3 | [#1 Adaptive FSR](#1-dynamic-resolution-scaling-adaptive-fsr) | Needs stable proxy recreate (F1 path) + profiles for targets |
| 4 | [#4 DMA-BUF offload](#4-zero-copy-inter-gpu-dma-buf-sharing) | Hardened research; high risk on NVIDIA↔RADV |
| 5 | [#2 Temporal / FSR2 / NIS](#2-amd-fsr-20--21-spatial-temporal-upscaling) | Largest quality leap; deepest engine coupling |

---

## 1. Dynamic Resolution Scaling (Adaptive FSR)

**Status:** in progress (continuous scale shipped)  
**Problem:** Static presets (quality / balanced / performance / ultra) keep a fixed render
resolution whether the GPU is idle (menus) or overloaded (combat).  
**Goal:** A feedback loop in the Vulkan layer that tracks frame time and smoothly steps
render scale toward a target FPS.

### Design notes

- Target frame time from env / profile, e.g. `XFSR_TARGET_FPS=60` → 16.67 ms.
- Measure present-to-present (or acquire-to-present) in `QueuePresentKHR`.
- Hysteresis: step down quickly on spikes; scale up slowly when headroom returns
  (avoid oscillation / thrashing swapchain recreates).
- Prefer **live scale** via existing F1 / `OUT_OF_DATE` recreate when the title
  handles it; fall back to blit-only scale changes when recreate is unsafe.
- Clamp steps to discrete preset ladder (1.5 / 1.7 / 2.0 / 2.5) or continuous
  `window/scale` with a min/max.

### Shipped

- Env/CLI: `XFSR_ADAPTIVE=1`, `XFSR_TARGET_FPS`, `--adaptive`, `--target-fps`
- Profile keys: `adaptive`, `target_fps`
- Present-to-present EMA → **continuous float scale** (1.05–3.0) with pixel hysteresis
  (≥12 px) before `OUT_OF_DATE`; control file: `scale 1.35`
- Overlay shows `A <scale>x` when adaptive is on

### Acceptance (remaining)

- Under a synthetic load spike (bench complexity ramp), scale drops within ~0.5 s
  and recovers after the spike without DEVICE_LOST on Proton titles.

---

## 2. AMD FSR 2.0 / 2.1 Spatial-Temporal Upscaling

**Status:** near-term spatial **done**; true temporal **out of layer scope** on current design  
**Problem:** FSR 1.0 is spatial-only → shimmer, crawling edges, unstable fine detail.  
**Goal (realistic):** Cheap spatial alternatives (blit / NIS / FSR1). True FSR2/3 need
motion vectors + depth from the game; **FSR4** is a Proton DLL upgrade for **AMD RDNA2+**
titles that already ship FSR 3.1/4 — not runnable on NVIDIA GTX 1650-class dGPUs and not
something a swapchain-only layer can invent.

### Options

| Path | Status | Notes |
|------|--------|-------|
| C. NIS / blit / edge | **shipped** | `--upscaler fsr1\|blit\|nis\|edge`; `--hybrid` = iGPU edge + one-way |
| A. True FSR2/3 in layer | **blocked** | Layer only sees swapchain color; no MVs/depth |
| B. Generic TAA | research | Ghosting without vectors |
| D. Per-title FSR2 hooks | research | Not “works with any game” |
| E. Proton `PROTON_FSR4_UPGRADE` | docs-only | AMD machines with FSR 3.1/4 games; **zero benefit on GTX 1650** |

### Shipped (spatial)

- `NisPass` compute: edge-directional bicubic + contrast-adaptive sharpen
- `EdgePass` compute: quality (luma edge-stop cubic + chroma bilinear, Di Zenzo RGB
  structure tensor + ETF anisotropy, padded display CAS, linear anti-ring) and
  perf/lite; live sharpness + mode; direct STORAGE→WSI on AMD present
- Negative mip LOD bias via `vkCreateSampler` (`XFSR_LOD_BIAS=auto` → `-log2(scale)`)
- CLI/env: `--upscaler`, `XFSR_UPSCALER`, `--edge-mode`, `--edge-sharpness`, `--hybrid`
- Live F3 cycle: fsr1 → blit → nis → edge; F2 = RCAS or edge quality/perf
- README: same-GPU for peak FPS; `--hybrid` for iGPU one-way `edge` (no back-haul)

### Acceptance (remaining)

- Visual A/B doc with screenshots on a foliage-heavy scene.
- Optional true NIS SDK / integer scale.
- Safer adaptive recreate under Proton exclusive fullscreen.

---

## 3. Intelligent Per-Game Profile Database

**Status:** done (v0.5.0)  
**Problem:** Titles need different flags (caps shrink, Wow64, RCAS, offload). Users
re-learn launch options per game.  
**Goal:** Auto-apply known-good settings from a local profile DB.

### Layout

```
~/.config/xfsr/profiles.conf     # user overrides (created on first run)
share/xfsr/profiles.example.conf # shipped defaults (copied or merged)
```

Lookup by (first match wins):

1. `SteamAppId` / `SteamGameId`
2. Executable basename (e.g. `cs2`, `iw3sp.exe`)
3. Optional `exe_contains=` substring

Keys (env-compatible):

| Key | Maps to |
|-----|---------|
| `preset` | `--preset` / `XFSR_PRESET` |
| `fsr` | `1` / `0` (blit) / `native` |
| `rcas` | `XFSR_RCAS` |
| `offload` | `XFSR_OFFLOAD` |
| `one_way` | `XFSR_IGPU_PRESENT` |
| `caps_shrink` | `XFSR_CAPS_SHRINK` |
| `overlay` | `XFSR_OVERLAY` |
| `zink` | force / forbid Zink |
| `wow64` | hint / document `PROTON_USE_WOW64=1` |
| `target_fps` | reserved for adaptive (#1) |

CLI always wins over the profile. Log: `xfsr-run: profile match SteamAppId=730 …`.

### Acceptance

- CS2-like profile sets `caps_shrink=0`, `offload=0` without flags.
- CoD4-like profile documents Wow64 + borderless and sets overlay / preset.

---

## 4. Zero-Copy Inter-GPU DMA-BUF Sharing

**Status:** planned (blocked on NVIDIA↔RADV by default)  
**Problem:** `--offload` round-trip uses shared_host / memcpy; dma-buf DEVICE_LOSTs on
many NVIDIA↔Mesa hybrids ([research.md](research.md)).  
**Goal:** Harden explicit sync + selective dma-buf so hybrid offload is lower latency.

### Workstreams

- Probe + quarantine: keep `XFSR_DMABUF=0` default on NVIDIA↔Mesa; allowlist known-good.
- Explicit sync (`VK_KHR_external_semaphore_fd` / syncobj) around imports.
- Prefer one-way present when safe (already: native Vulkan, no MangoHud); Zink stays
  round-trip on NVIDIA.
- Revisit copy-engine queues for D2H (research showed better wall time).

### Acceptance

- Documented matrix: vendor pair × backend × FPS vs shared_host.
- No default path that DEVICE_LOSTs on GTX 16xx + RADV.

---

## 5. Automated Benchmark & Recommendation Tool

**Status:** done (matrix + `--apply`; real Steam AppID hooks still optional)  
**Problem:** Users don’t know which preset/RCAS mix fits their GPU.  
**Goal:** `xfsr-auto-tune` runs a short matrix and writes a profile entry.

### Flow

1. Detect AppID / binary name.
2. Run ~5 s uncapped passes: presets × rcas on/off via **same-GPU + blit**
   (`--layer-dir` build; never `--offload`).
3. Collect avg FPS, 1% lows, frame-time stddev.
4. Pick best under a quality floor (prefer lower scale if within ~5% of peak).
5. Write `preset` + `upscaler=blit` + `offload=0` into `~/.config/xfsr/profiles.conf`.

### Acceptance

- [x] One command produces a reusable profile line for the current title.
- [x] `--apply` vs `--dry-run`.
- [ ] Optional: auto-tune against a live Steam AppID launch (not only `xfsr_load_vk`).

---

## Near-term checklist

- [x] Document this roadmap
- [x] Ship `profiles.example.conf` + `xfsr-run` loader (#3)
- [x] Scaffold `xfsr-auto-tune` (`scripts/xfsr-auto-tune`, `--apply` → profiles) (#5)
- [x] Bench `p1_fps` / `ft_stdev_ms`; auto-tune scores 1% lows + jitter; recommends blit
- [x] Docs: hybrid max-FPS = same-GPU + blit/nis + ultra/adaptive (not FSR2/3/4)
- [x] `xfsr-run --check` via `$XDG_RUNTIME_DIR/xfsr-status` (proxy vs identity)
- [x] Overlay default when MangoHud absent
- [x] Adaptive prototype: `XFSR_ADAPTIVE=1` / `--adaptive` + `--target-fps` (#1)
- [x] Adaptive continuous float scale + pixel hysteresis (#1)
- [x] `--upscaler fsr1|blit|nis` + `NisPass` (#2 near-term)
- [x] `--upscaler edge` + `--hybrid` one-way iGPU present (no round-trip fallback)
- [ ] Adaptive: safer recreate under Proton / exclusive fullscreen
- [ ] `xfsr-auto-tune` against real games / Steam AppID hooks
- [x] Universal nest path: `xfsr-run --any` + NVIDIA `--backend sdl` (gamescope)
- [ ] DMA-BUF allowlist + sync hardening (#4)

---

## Non-goals (for now)

- Replacing Magpie / Lossless Scaling for every Windows DX9 exclusive-fullscreen title
  without borderless / Wow64.
- True FSR 2/3 inside the Vulkan layer (no game MVs/depth) or FSR4 on NVIDIA
  (Proton `PROTON_FSR4_UPGRADE` is AMD RDNA2+ only).
- Guaranteeing one-way offload under Zink + NVIDIA proprietary (opaque WSI SIGSEGV).
- Using `--offload` as the default / max-FPS path on PCIe hybrids.
