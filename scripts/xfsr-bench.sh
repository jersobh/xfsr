#!/usr/bin/env bash
# Benchmark xfsr-run modes with uncapped load apps (not vkcube@60).
# Every case runs with MangoHud (overlay + frametime) for visual/FPS parity.
#
# Usage:
#   ./scripts/xfsr-bench.sh              # 6s each, complexity 128
#   ./scripts/xfsr-bench.sh 12 96        # seconds, complexity
#   XFSR_BENCH_VK_ONLY=1 ./scripts/xfsr-bench.sh
#   XFSR_BENCH_NO_MANGOHUD=1 ./scripts/xfsr-bench.sh   # opt out
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${XFSR_BUILD:-$ROOT/build}"
XFSR="$ROOT/scripts/xfsr-run"
SECONDS_N="${1:-6}"
# Higher default complexity so shrink→FSR shows a clear FPS win (64 was too light).
COMPLEXITY="${2:-128}"
WIN_W="${XFSR_BENCH_W:-1920}"
WIN_H="${XFSR_BENCH_H:-1080}"

VK_BIN="$BUILD/xfsr_load_vk"
GL_BIN="$BUILD/xfsr_load_gl"

USE_MANGOHUD=1
if [[ "${XFSR_BENCH_NO_MANGOHUD:-0}" == "1" ]]; then
  USE_MANGOHUD=0
fi

if [[ ! -x "$VK_BIN" ]] || [[ ! -x "$GL_BIN" ]]; then
  echo "xfsr-bench: building load apps..."
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DXFSR_BUILD_BENCH=ON
  cmake --build "$BUILD" -j --target xfsr_load_vk xfsr_load_gl VkLayer_XFSR xfsr_gl_shim
fi

if [[ $USE_MANGOHUD -eq 1 ]]; then
  if ! command -v mangohud >/dev/null 2>&1 && [[ ! -e /usr/lib/mangohud/libMangoHud.so ]]; then
    echo "xfsr-bench: mangohud not found (install mangohud, or XFSR_BENCH_NO_MANGOHUD=1)" >&2
    exit 1
  fi
fi

ARGS=(--seconds "$SECONDS_N" --complexity "$COMPLEXITY" --width "$WIN_W" --height "$WIN_H")

# Prefix for baseline apps (no xfsr-run). Empty when HUD disabled.
MH_BASE=()
if [[ $USE_MANGOHUD -eq 1 ]]; then
  MH_BASE=(mangohud)
fi

run_case() {
  local name="$1"
  shift
  echo
  echo "======== $name ========"
  # Capture XFSR_BENCH_END line for the summary table.
  local out
  out="$( "$@" "${ARGS[@]}" 2>&1 | tee /dev/stderr | grep '^XFSR_BENCH_END' | tail -n1 || true )"
  if [[ -z "$out" ]]; then
    echo "  (no XFSR_BENCH_END — failed?)"
    RESULTS+=("$name|FAIL|0|0|0x0")
    return
  fi
  # XFSR_BENCH_END api=... avg_fps=X best_fps=Y extent=WxH ...
  local avg best extent
  avg="$(echo "$out" | sed -n 's/.*avg_fps=\([0-9.]*\).*/\1/p')"
  best="$(echo "$out" | sed -n 's/.*best_fps=\([0-9.]*\).*/\1/p')"
  extent="$(echo "$out" | sed -n 's/.*extent=\([0-9x]*\).*/\1/p')"
  RESULTS+=("$name|$avg|$best|$extent")
  echo "  → avg=${avg} best=${best} extent=${extent}"
}

RESULTS=()

echo "xfsr-bench: ${SECONDS_N}s  complexity=${COMPLEXITY}  ${WIN_W}x${WIN_H}  mangohud=$USE_MANGOHUD"
echo "Binaries: $VK_BIN / $GL_BIN"

OUT=(--output "${WIN_W}x${WIN_H}")
# Always pass --mangohud into xfsr-run when enabled (also sets XFSR_MANGOHUD).
MH=()
if [[ $USE_MANGOHUD -eq 1 ]]; then
  MH=(--mangohud)
fi

# --- Vulkan ---
run_case "vk baseline (no layer)" env -u ENABLE_XFSR -u VK_ADD_IMPLICIT_LAYER_PATH \
  -u XFSR_DISPLAY_EXTENT -u XFSR_OUTPUT_W -u XFSR_OUTPUT_H "${MH_BASE[@]}" "$VK_BIN"
run_case "vk xfsr same-GPU quality" "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset quality -- "$VK_BIN"
run_case "vk xfsr same-GPU performance" "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance -- "$VK_BIN"
run_case "vk xfsr performance --no-rcas" "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --no-rcas -- "$VK_BIN"
run_case "vk xfsr performance --offload" "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --offload -- "$VK_BIN"

# --- OpenGL → Zink → layer ---
# Always pass --zink for GL load app (auto-detect also knows xfsr_load_gl).
# Without Zink, caps-shrink + viewport shim paints a postage-stamp in the window.
if [[ "${XFSR_BENCH_VK_ONLY:-0}" != "1" && -x "$GL_BIN" ]]; then
  run_case "gl baseline (native GL)" env -u ENABLE_XFSR -u VK_ADD_IMPLICIT_LAYER_PATH \
    -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER -u LD_PRELOAD \
    -u XFSR_DISPLAY_EXTENT -u XFSR_OUTPUT_W -u XFSR_OUTPUT_H -u XFSR_SCALE \
    "${MH_BASE[@]}" "$GL_BIN"
  run_case "gl zink+xfsr performance" "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance -- "$GL_BIN"
  run_case "gl zink+xfsr performance --no-rcas" "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --no-rcas -- "$GL_BIN"
  run_case "gl zink+xfsr performance --offload" "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --offload -- "$GL_BIN"
fi

echo
echo "╔══════════════════════════════════════════════════════════════════════════════╗"
echo "║                         xfsr-bench summary                                   ║"
echo "╠════════════════════════════════════════╦═════════╦═════════╦════════════════╣"
echo "║ case                                   ║ avg FPS ║ best    ║ extent         ║"
echo "╠════════════════════════════════════════╬═════════╬═════════╬════════════════╣"
for row in "${RESULTS[@]}"; do
  IFS='|' read -r name avg best extent <<<"$row"
  printf "║ %-38s ║ %7s ║ %7s ║ %-14s ║\n" "$name" "$avg" "$best" "$extent"
done
echo "╚════════════════════════════════════════╩═════════╩═════════╩════════════════╝"
echo
echo "Tip: xfsr rows should show a *lower* extent than baseline (e.g. 960x540 vs 1920x1080)."
echo "     If extent stays full-window, FSR never engaged (Wayland needs --output / DISPLAY_EXTENT)."
echo "     --offload only wins if the log shows dual-GPU (else same-GPU fallback)."
