#!/usr/bin/env bash
# glmark2 via Zink + layer (weak test — prefer vkcube / the demo binary).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GLMARK2=glmark2-es2-wayland
command -v "$GLMARK2" >/dev/null 2>&1 || GLMARK2=glmark2
exec "$ROOT/scripts/xfsr-run" --mangohud \
  --preset "${XFSR_PRESET:-${PRESET:-performance}}" \
  --fsr "${XFSR_FSR:-1}" \
  -- "$GLMARK2" "$@"
