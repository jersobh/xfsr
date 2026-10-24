#!/usr/bin/env bash
# Quick interactive smoke of the layer (uncapped load — not vkcube@60).
# For a full comparison table: ./scripts/xfsr-bench.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${XFSR_BUILD:-$ROOT/build}"
XFSR="$ROOT/scripts/xfsr-run"
PRESET="${1:-performance}"
BIN="$BUILD/xfsr_load_vk"

EXTRA=()
shift || true
while [[ $# -gt 0 ]]; do
  EXTRA+=("$1")
  shift
done

if [[ ! -x "$BIN" ]]; then
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD" -j --target xfsr_load_vk VkLayer_XFSR
fi

export XFSR_VERBOSE="${XFSR_VERBOSE:-1}"
echo "xfsr-demo: $BIN --preset $PRESET ${EXTRA[*]:-}"
echo "  (Ctrl+C or Esc to quit; for timed table use ./scripts/xfsr-bench.sh)"
exec "$XFSR" --mangohud --no-zink --preset "$PRESET" "${EXTRA[@]}" -- \
  "$BIN" --seconds 3600 --complexity 64
