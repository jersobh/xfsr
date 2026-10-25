#!/usr/bin/env bash
# Benchmark xfsr-run across WSI (X11/Wayland), window modes, APIs, and presets.
# Every case runs with MangoHud (overlay + frametime) for visual/FPS parity.
#
# Usage:
#   ./scripts/xfsr-bench.sh                 # full matrix (x11+wl × win+fs)
#   ./scripts/xfsr-bench.sh 4 96            # seconds, complexity
#   XFSR_BENCH_QUICK=1 ./scripts/xfsr-bench.sh          # short subset, both WSI, windowed
#   XFSR_BENCH_X11_ONLY=1 ./scripts/xfsr-bench.sh
#   XFSR_BENCH_WAYLAND_ONLY=1 ./scripts/xfsr-bench.sh
#   XFSR_BENCH_VK_ONLY=1 / XFSR_BENCH_GL_ONLY=1
#   XFSR_BENCH_WINDOWED_ONLY=1 / XFSR_BENCH_FULLSCREEN_ONLY=1
#   XFSR_BENCH_NO_MANGOHUD=1
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${XFSR_BUILD:-$ROOT/build}"
XFSR="$ROOT/scripts/xfsr-run"
SECONDS_N="${1:-6}"
COMPLEXITY="${2:-128}"
WIN_W="${XFSR_BENCH_W:-1920}"
WIN_H="${XFSR_BENCH_H:-1080}"

VK_BIN="$BUILD/xfsr_load_vk"
GL_BIN="$BUILD/xfsr_load_gl"

USE_MANGOHUD=1
[[ "${XFSR_BENCH_NO_MANGOHUD:-0}" == "1" ]] && USE_MANGOHUD=0

QUICK="${XFSR_BENCH_QUICK:-0}"
VK_ONLY="${XFSR_BENCH_VK_ONLY:-0}"
GL_ONLY="${XFSR_BENCH_GL_ONLY:-0}"
WIN_ONLY="${XFSR_BENCH_WINDOWED_ONLY:-0}"
FS_ONLY="${XFSR_BENCH_FULLSCREEN_ONLY:-0}"
X11_ONLY="${XFSR_BENCH_X11_ONLY:-0}"
WL_ONLY="${XFSR_BENCH_WAYLAND_ONLY:-0}"

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

# Remember the ambient session — we switch per case instead of clearing globally.
SAVE_DISPLAY="${DISPLAY:-:0}"
SAVE_WAYLAND="${WAYLAND_DISPLAY:-}"
SAVE_WAYLAND_SOCKET="${WAYLAND_SOCKET:-}"
if [[ -z "$SAVE_WAYLAND" ]]; then
  _rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  for s in wayland-1 wayland-0; do
    if [[ -S "$_rt/$s" ]]; then
      SAVE_WAYLAND=$s
      break
    fi
  done
  unset _rt
fi

MH_BASE=()
[[ $USE_MANGOHUD -eq 1 ]] && MH_BASE=(mangohud)
MH=()
[[ $USE_MANGOHUD -eq 1 ]] && MH=(--mangohud)

OUT=(--output "${WIN_W}x${WIN_H}")

# --- WSI backends ---
WSI_MODES=()
if [[ "$X11_ONLY" == "1" ]]; then
  WSI_MODES=(x11)
elif [[ "$WL_ONLY" == "1" ]]; then
  WSI_MODES=(wayland)
else
  WSI_MODES=(x11)
  if [[ -n "$SAVE_WAYLAND" ]]; then
    WSI_MODES+=(wayland)
  else
    echo "xfsr-bench: no Wayland socket — skipping wayland (set WAYLAND_DISPLAY or XFSR_BENCH_WAYLAND_ONLY=1)"
  fi
fi
if [[ "$WL_ONLY" == "1" && -z "$SAVE_WAYLAND" ]]; then
  echo "xfsr-bench: XFSR_BENCH_WAYLAND_ONLY=1 but no WAYLAND_DISPLAY/socket" >&2
  exit 1
fi

# --- windowed / fullscreen ---
DISPLAY_MODES=()
if [[ "$QUICK" == "1" || "$WIN_ONLY" == "1" ]]; then
  DISPLAY_MODES=(windowed)
elif [[ "$FS_ONLY" == "1" ]]; then
  DISPLAY_MODES=(fullscreen)
else
  DISPLAY_MODES=(windowed fullscreen)
fi

RESULTS=()
DISP_ARGS=()
WSI=""          # x11 | wayland
WSI_TAG=""      # x11 | wl
WIN_TAG=""      # win | fs

# Run a command under the active WSI. Wayland cases force xfsr-run to keep WL
# (otherwise it clears WAYLAND_DISPLAY for FSR).
run_wsi() {
  # coreutils env: OPTIONS (-u) must precede NAME=VALUE, else -u is the command.
  if [[ "$WSI" == wayland ]]; then
    if [[ -n "$SAVE_WAYLAND_SOCKET" ]]; then
      env DISPLAY="$SAVE_DISPLAY" WAYLAND_DISPLAY="$SAVE_WAYLAND" \
        WAYLAND_SOCKET="$SAVE_WAYLAND_SOCKET" \
        XFSR_VK_WAYLAND=1 XFSR_ZINK_WAYLAND=1 "$@"
    else
      env DISPLAY="$SAVE_DISPLAY" WAYLAND_DISPLAY="$SAVE_WAYLAND" \
        XFSR_VK_WAYLAND=1 XFSR_ZINK_WAYLAND=1 "$@"
    fi
  else
    env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET \
      -u XFSR_VK_WAYLAND -u XFSR_ZINK_WAYLAND \
      DISPLAY="$SAVE_DISPLAY" "$@"
  fi
}

run_case() {
  local name="$1"
  shift
  echo
  echo "======== $name ========"
  local out
  local app_args=(--seconds "$SECONDS_N" --complexity "$COMPLEXITY")
  if [[ "${DISP_ARGS[*]}" == *"--windowed"* ]]; then
    app_args+=(--width "$WIN_W" --height "$WIN_H")
  fi
  app_args+=("${DISP_ARGS[@]}")

  out="$( run_wsi "$@" "${app_args[@]}" 2>&1 | tee /dev/stderr | grep '^XFSR_BENCH_END' | tail -n1 || true )"
  if [[ -z "$out" ]]; then
    echo "  (no XFSR_BENCH_END — failed?)"
    RESULTS+=("$name|FAIL|—|—")
    return
  fi
  local avg best extent
  avg="$(echo "$out" | sed -n 's/.*avg_fps=\([0-9.]*\).*/\1/p')"
  best="$(echo "$out" | sed -n 's/.*best_fps=\([0-9.]*\).*/\1/p')"
  extent="$(echo "$out" | sed -n 's/.*extent=\([0-9x]*\).*/\1/p')"
  RESULTS+=("$name|$avg|$best|$extent")
  echo "  → avg=${avg} best=${best} extent=${extent}"
}

run_vk_suite() {
  local tag="$WSI_TAG/$WIN_TAG"
  run_case "vk $tag baseline" env -u ENABLE_XFSR -u VK_ADD_IMPLICIT_LAYER_PATH \
    -u XFSR_DISPLAY_EXTENT -u XFSR_OUTPUT_W -u XFSR_OUTPUT_H "${MH_BASE[@]}" "$VK_BIN"

  if [[ "$QUICK" == "1" ]]; then
    run_case "vk $tag same-GPU quality" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset quality -- "$VK_BIN"
    run_case "vk $tag same-GPU performance" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance -- "$VK_BIN"
    run_case "vk $tag perf --no-rcas" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --no-rcas -- "$VK_BIN"
    run_case "vk $tag perf --offload" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --offload --no-one-way -- "$VK_BIN"
    return
  fi

  run_case "vk $tag same-GPU quality" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset quality -- "$VK_BIN"
  run_case "vk $tag same-GPU balanced" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset balanced -- "$VK_BIN"
  run_case "vk $tag same-GPU performance" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance -- "$VK_BIN"
  run_case "vk $tag same-GPU ultra" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset ultra -- "$VK_BIN"
  run_case "vk $tag perf --no-rcas" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --no-rcas -- "$VK_BIN"
  run_case "vk $tag perf --offload" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --offload --no-one-way -- "$VK_BIN"
  run_case "vk $tag perf --offload --no-rcas" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --no-zink --preset performance --offload --no-one-way --no-rcas -- "$VK_BIN"
}

run_gl_suite() {
  local tag="$WSI_TAG/$WIN_TAG"
  run_case "gl $tag baseline (native)" env -u ENABLE_XFSR -u VK_ADD_IMPLICIT_LAYER_PATH \
    -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER -u LD_PRELOAD \
    -u XFSR_DISPLAY_EXTENT -u XFSR_OUTPUT_W -u XFSR_OUTPUT_H -u XFSR_SCALE \
    "${MH_BASE[@]}" "$GL_BIN"

  if [[ "$QUICK" == "1" ]]; then
    run_case "gl $tag zink+xfsr performance" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance -- "$GL_BIN"
    run_case "gl $tag zink perf --no-rcas" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --no-rcas -- "$GL_BIN"
    run_case "gl $tag zink perf --offload" \
      "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --offload --no-one-way -- "$GL_BIN"
    return
  fi

  run_case "gl $tag zink+xfsr quality" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset quality -- "$GL_BIN"
  run_case "gl $tag zink+xfsr balanced" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset balanced -- "$GL_BIN"
  run_case "gl $tag zink+xfsr performance" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance -- "$GL_BIN"
  run_case "gl $tag zink+xfsr ultra" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset ultra -- "$GL_BIN"
  run_case "gl $tag zink perf --no-rcas" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --no-rcas -- "$GL_BIN"
  run_case "gl $tag zink perf --offload" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --offload --no-one-way -- "$GL_BIN"
  run_case "gl $tag zink perf --offload --no-rcas" \
    "$XFSR" "${OUT[@]}" "${MH[@]}" --zink --preset performance --offload --no-one-way --no-rcas -- "$GL_BIN"
}

echo "xfsr-bench: ${SECONDS_N}s  complexity=${COMPLEXITY}  window=${WIN_W}x${WIN_H}"
echo "  wsi=${WSI_MODES[*]}  modes=${DISPLAY_MODES[*]}  mangohud=$USE_MANGOHUD  quick=$QUICK"
echo "  DISPLAY=$SAVE_DISPLAY  WAYLAND_DISPLAY=${SAVE_WAYLAND:-<none>}"
echo "Binaries: $VK_BIN / $GL_BIN"

for WSI in "${WSI_MODES[@]}"; do
  if [[ "$WSI" == wayland ]]; then
    WSI_TAG=wl
  else
    WSI_TAG=x11
  fi
  for mode in "${DISPLAY_MODES[@]}"; do
    if [[ "$mode" == fullscreen ]]; then
      DISP_ARGS=(--fullscreen)
      WIN_TAG=fs
    else
      DISP_ARGS=(--windowed)
      WIN_TAG=win
    fi
    echo
    echo "######## wsi=$WSI  display=$mode ########"

    if [[ "$GL_ONLY" != "1" ]]; then
      run_vk_suite
    fi
    if [[ "$VK_ONLY" != "1" && -x "$GL_BIN" ]]; then
      run_gl_suite
    fi
  done
done

echo
echo "╔══════════════════════════════════════════════════════╦═════════╦═════════╦════════════════╗"
echo "║                    xfsr-bench summary                ║ avg FPS ║ best    ║ extent         ║"
echo "╠══════════════════════════════════════════════════════╬═════════╬═════════╬════════════════╣"
for row in "${RESULTS[@]}"; do
  IFS='|' read -r name avg best extent <<<"$row"
  printf "║ %-52s ║ %7s ║ %7s ║ %-14s ║\n" "$name" "$avg" "$best" "$extent"
done
echo "╚══════════════════════════════════════════════════════╩═════════╩═════════╩════════════════╝"
echo

# Per (wsi, win-mode) baseline compare for VK.
_warn=0
declare -A _base_ext=()
for row in "${RESULTS[@]}"; do
  IFS='|' read -r name avg best extent <<<"$row"
  if [[ "$name" =~ ^vk\ (x11|wl)/(win|fs)\ baseline$ ]]; then
    _base_ext["${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"]="$extent"
  elif [[ "$name" =~ ^vk\ (x11|wl)/(win|fs)\  ]]; then
    key="${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
    base="${_base_ext[$key]:-}"
    if [[ "$avg" == "FAIL" || "$extent" == "—" || "$base" == "—" || -z "$base" ]]; then
      continue
    fi
    if [[ "$extent" == "$base" ]]; then
      if [[ "${BASH_REMATCH[2]}" == fs ]]; then
        echo "NOTE: '$name' extent=$extent == fullscreen baseline — exclusive FS often locks size."
      elif [[ "${BASH_REMATCH[1]}" == wl ]]; then
        echo "WARN: '$name' extent=$extent same as Wayland baseline — FSR shrink failed."
        _warn=1
      else
        echo "WARN: '$name' extent=$extent same as X11 windowed baseline — FSR did not engage."
        _warn=1
      fi
    fi
  fi
done
[[ $_warn -eq 1 ]] && echo "     Check layer + libxfsr_gl_shim install; Wayland GL needs the wl_egl_window shrink."
unset _base_ext _warn

echo "Tip: VK/GL xfsr should shrink vs baseline on X11 and Wayland (e.g. 960x540 vs 1920x1080)."
echo "     GL Wayland reports shrunk extent via the shim; judge FPS vs native GL too."
echo "     --offload is usually slower (host round-trip). X11 is often faster than Wayland on hybrids."
echo "Filters: XFSR_BENCH_QUICK=1  XFSR_BENCH_X11_ONLY=1  XFSR_BENCH_WAYLAND_ONLY=1"
echo "         XFSR_BENCH_WINDOWED_ONLY=1  XFSR_BENCH_FULLSCREEN_ONLY=1"
