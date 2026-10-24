#!/usr/bin/env bash
# Benchmark xfsr-run modes with uncapped load apps (not vkcube@60).
#
# Usage:
#   ./scripts/xfsr-bench.sh              # 8s each, complexity 64
#   ./scripts/xfsr-bench.sh 12 96        # seconds, complexity
#   XFSR_BENCH_VK_ONLY=1 ./scripts/xfsr-bench.sh
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

if [[ ! -x "$VK_BIN" ]]; then
  echo "xfsr-bench: building load apps..."
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" -j --target xfsr_load_vk xfsr_load_gl VkLayer_XFSR
fi

ARGS=(--seconds "$SECONDS_N" --complexity "$COMPLEXITY" --width "$WIN_W" --height "$WIN_H")

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

echo "xfsr-bench: ${SECONDS_N}s  complexity=${COMPLEXITY}  ${WIN_W}x${WIN_H}"
echo "Binaries: $VK_BIN / $GL_BIN"

OUT=(--output "${WIN_W}x${WIN_H}")

# --- Vulkan ---
run_case "vk baseline (no layer)" env -u ENABLE_XFSR -u VK_ADD_IMPLICIT_LAYER_PATH \
  -u XFSR_DISPLAY_EXTENT -u XFSR_OUTPUT_W -u XFSR_OUTPUT_H "$VK_BIN"
run_case "vk xfsr same-GPU quality" "$XFSR" "${OUT[@]}" --no-zink --preset quality -- "$VK_BIN"
run_case "vk xfsr same-GPU performance" "$XFSR" "${OUT[@]}" --no-zink --preset performance -- "$VK_BIN"
run_case "vk xfsr performance --no-rcas" "$XFSR" "${OUT[@]}" --no-zink --preset performance --no-rcas -- "$VK_BIN"
run_case "vk xfsr performance --offload" "$XFSR" "${OUT[@]}" --no-zink --preset performance --offload -- "$VK_BIN"

# --- OpenGL → Zink → layer ---
if [[ "${XFSR_BENCH_VK_ONLY:-0}" != "1" && -x "$GL_BIN" ]]; then
  run_case "gl baseline (native GL)" env -u ENABLE_XFSR -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER \
    -u XFSR_DISPLAY_EXTENT -u XFSR_OUTPUT_W -u XFSR_OUTPUT_H "$GL_BIN"
  run_case "gl zink+xfsr performance" "$XFSR" "${OUT[@]}" --preset performance -- "$GL_BIN"
  run_case "gl zink+xfsr performance --no-rcas" "$XFSR" "${OUT[@]}" --preset performance --no-rcas -- "$GL_BIN"
  run_case "gl zink+xfsr performance --offload" "$XFSR" "${OUT[@]}" --preset performance --offload -- "$GL_BIN"
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
