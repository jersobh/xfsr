#!/usr/bin/env bash
# Install xfsr-run + layer into the user profile from a local build (no root).
#   ~/.local/bin/xfsr-run
#   ~/.local/lib/libVkLayer_XFSR.so
#   ~/.local/share/vulkan/implicit_layer.d/VkLayer_XFSR.json
#   ~/.local/share/xfsr/shaders/*.spv
#
# Prefer the one-liner for binary installs:
#   curl -fsSL https://raw.githubusercontent.com/jersobh/xfsr/main/install.sh | bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${XFSR_BUILD:-$ROOT/build}"
PREFIX="${PREFIX:-$HOME/.local}"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build "$BUILD" -j"$(nproc)" --target VkLayer_XFSR xfsr_gl_shim

mkdir -p "$PREFIX/bin" "$PREFIX/lib" \
  "$PREFIX/share/vulkan/implicit_layer.d" \
  "$PREFIX/share/xfsr/shaders"

install -m 755 "$ROOT/scripts/xfsr-run" "$PREFIX/bin/xfsr-run"
install -m 755 "$BUILD/libVkLayer_XFSR.so" "$PREFIX/lib/libVkLayer_XFSR.so"
install -m 755 "$BUILD/libxfsr_gl_shim.so" "$PREFIX/lib/libxfsr_gl_shim.so"

# Drop pre-rename layer artifacts so the loader does not load two XFSR layers.
rm -f "$PREFIX/lib/libVkLayer_CROSSVULKAN_xfsr.so" \
  "$PREFIX/share/vulkan/implicit_layer.d/VkLayer_CROSSVULKAN_xfsr.json"

# FSR (+ blit) SPIR-V the layer loads via XFSR_SHADER_DIR
shopt -s nullglob
for f in "$BUILD"/shaders/fsr_*.spv "$BUILD"/shaders/blit.*.spv; do
  install -m 644 "$f" "$PREFIX/share/xfsr/shaders/"
done
shopt -u nullglob

python3 - <<PY
import json, pathlib
src = pathlib.Path("$BUILD/VkLayer_XFSR.json")
dst = pathlib.Path("$PREFIX/share/vulkan/implicit_layer.d/VkLayer_XFSR.json")
data = json.loads(src.read_text())
data["layer"]["library_path"] = str(pathlib.Path("$PREFIX/lib/libVkLayer_XFSR.so").resolve())
dst.write_text(json.dumps(data, indent=2) + "\n")
print("wrote", dst)
print("library_path =", data["layer"]["library_path"])
PY

echo
echo "Installed:"
echo "  $PREFIX/bin/xfsr-run"
echo "  $PREFIX/lib/libVkLayer_XFSR.so"
echo "  $PREFIX/lib/libxfsr_gl_shim.so"
echo "  $PREFIX/share/vulkan/implicit_layer.d/VkLayer_XFSR.json"
echo "  $PREFIX/share/xfsr/shaders/"
echo
if ! command -v xfsr-run >/dev/null 2>&1; then
  echo "Add to PATH if needed:  export PATH=\"$PREFIX/bin:\$PATH\""
fi
echo "Steam launch option:  xfsr-run --mangohud --preset performance --no-rcas %command%"
echo "Smoke test:           xfsr-run --print true"
