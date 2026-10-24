#!/usr/bin/env bash
# Build a relocatable xfsr-run release tarball under dist/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
esac
NAME="xfsr-${VERSION}-linux-${ARCH}"
BUILD="${XFSR_BUILD:-$ROOT/build-release}"
STAGE="${XFSR_STAGE:-$ROOT/dist/.stage/$NAME}"
OUT_DIR="${XFSR_DIST:-$ROOT/dist}"
TAR="$OUT_DIR/${NAME}.tar.gz"

echo "packaging $NAME ..."

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DXFSR_BUILD_DEMO=OFF -DXFSR_BUILD_BENCH=OFF
cmake --build "$BUILD" -j"$(nproc)" --target VkLayer_XFSR xfsr_gl_shim

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/share/vulkan/implicit_layer.d" "$STAGE/share/xfsr/shaders"

install -m 755 "$ROOT/scripts/xfsr-run" "$STAGE/bin/xfsr-run"
install -m 755 "$BUILD/libVkLayer_XFSR.so" "$STAGE/lib/libVkLayer_XFSR.so"
install -m 755 "$BUILD/libxfsr_gl_shim.so" "$STAGE/lib/libxfsr_gl_shim.so"

# Layer JSON with relative library path (resolved from install prefix by install.sh)
python3 - <<PY
import json, pathlib
src = pathlib.Path("$BUILD/VkLayer_XFSR.json")
dst = pathlib.Path("$STAGE/share/vulkan/implicit_layer.d/VkLayer_XFSR.json")
data = json.loads(src.read_text())
# Placeholder rewritten by install.sh to an absolute path under ~/.local
data["layer"]["library_path"] = "libVkLayer_XFSR.so"
data["layer"]["implementation_version"] = "${VERSION}".split(".")[0]
dst.write_text(json.dumps(data, indent=2) + "\n")
print("wrote", dst)
PY

# SPIR-V the layer may load (FSR + blit helpers)
shopt -s nullglob
for f in "$BUILD"/shaders/fsr_*.spv "$BUILD"/shaders/blit.*.spv; do
  install -m 644 "$f" "$STAGE/share/xfsr/shaders/"
done
shopt -u nullglob

printf '%s\n' "$VERSION" > "$STAGE/VERSION"
cp -f "$ROOT/CHANGELOG.md" "$STAGE/CHANGELOG.md"
[[ -f "$ROOT/LICENSE" ]] && cp -f "$ROOT/LICENSE" "$STAGE/LICENSE"

# Tiny README inside the tarball
cat > "$STAGE/README.txt" <<EOF
xfsr-run ${VERSION} (linux-${ARCH})

Install:
  ./install-from-tarball.sh
  # or:  PREFIX=\$HOME/.local ./install-from-tarball.sh

Steam launch options:
  xfsr-run --mangohud --preset performance --no-rcas %command%
  gamemoderun xfsr-run --offload --mangohud --preset performance --no-rcas %command%
EOF

# Self-contained installer shipped inside the archive
cat > "$STAGE/install-from-tarball.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
mkdir -p "$PREFIX/bin" "$PREFIX/lib" "$PREFIX/share/vulkan/implicit_layer.d" "$PREFIX/share/xfsr/shaders"
install -m 755 "$HERE/bin/xfsr-run" "$PREFIX/bin/xfsr-run"
install -m 755 "$HERE/lib/libVkLayer_XFSR.so" "$PREFIX/lib/libVkLayer_XFSR.so"
install -m 755 "$HERE/lib/libxfsr_gl_shim.so" "$PREFIX/lib/libxfsr_gl_shim.so"
cp -a "$HERE/share/xfsr/shaders/." "$PREFIX/share/xfsr/shaders/"
python3 - <<PY
import json, pathlib
src = pathlib.Path("$HERE/share/vulkan/implicit_layer.d/VkLayer_XFSR.json")
dst = pathlib.Path("$PREFIX/share/vulkan/implicit_layer.d/VkLayer_XFSR.json")
data = json.loads(src.read_text())
data["layer"]["library_path"] = str(pathlib.Path("$PREFIX/lib/libVkLayer_XFSR.so").resolve())
dst.write_text(json.dumps(data, indent=2) + "\n")
print("wrote", dst)
PY
echo "Installed xfsr-run to $PREFIX"
echo "Ensure PATH contains $PREFIX/bin"
echo "Steam:  xfsr-run --mangohud --preset performance --no-rcas %command%"
EOF
chmod +x "$STAGE/install-from-tarball.sh"

mkdir -p "$OUT_DIR"
tar -C "$(dirname "$STAGE")" -czf "$TAR" "$(basename "$STAGE")"
rm -rf "${ROOT}/dist/.stage"

# Convenience copy without version (local dist/)
cp -f "$TAR" "$OUT_DIR/xfsr-linux-${ARCH}.tar.gz"
ls -lh "$TAR" "$OUT_DIR/xfsr-linux-${ARCH}.tar.gz"
echo "OK $TAR"
