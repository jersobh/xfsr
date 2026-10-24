#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "$ROOT/scripts/xfsr-run" --mangohud --no-zink \
  --preset "${XFSR_PRESET:-performance}" \
  -- vkcube "$@"
