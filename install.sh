#!/bin/sh
# xfsr-run installer — paste this in a terminal:
#
#   curl -fsSL https://raw.githubusercontent.com/jersobh/xfsr/main/install.sh | sh
#
# Optional env:
#   XFSR_VERSION=0.1.0   pin a release (default: latest)
#   PREFIX=$HOME/.local  install root (default)
#   XFSR_REPO=owner/repo override GitHub repo
set -eu

REPO="${XFSR_REPO:-jersobh/xfsr}"
PREFIX="${PREFIX:-${HOME}/.local}"
BIN_DIR="${PREFIX}/bin"
LIB_DIR="${PREFIX}/lib"
LAYER_DIR="${PREFIX}/share/vulkan/implicit_layer.d"
SHADER_DIR="${PREFIX}/share/xfsr/shaders"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
  Linux) ;;
  *)
    echo "xfsr-run: only Linux is supported (got $OS)" >&2
    exit 1
    ;;
esac

case "$ARCH" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  *)
    echo "xfsr-run: unsupported CPU architecture: $ARCH" >&2
    exit 1
    ;;
esac

for cmd in curl tar; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "xfsr-run: '$cmd' is required" >&2
    exit 1
  fi
done

echo ">>> Installing xfsr-run for linux-${ARCH}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -n "${XFSR_VERSION:-}" ]; then
  VER="${XFSR_VERSION#v}"
  TAG="v${VER}"
else
  echo ">>> Resolving latest release..."
  # Prefer redirect Location from /releases/latest (no JSON parser needed).
  LOC="$(curl -fsSIL "https://github.com/${REPO}/releases/latest" 2>/dev/null \
    | tr -d '\r' \
    | awk 'tolower($1)=="location:" {print $2; exit}')"
  if [ -z "$LOC" ]; then
    echo "xfsr-run: could not find latest release for ${REPO}" >&2
    echo "  Tip: XFSR_VERSION=0.1.0 curl -fsSL …/install.sh | sh" >&2
    exit 1
  fi
  TAG="$(echo "$LOC" | sed 's|.*/||')"
  VER="${TAG#v}"
fi

ASSET="xfsr-${VER}-linux-${ARCH}.tar.gz"
URL="https://github.com/${REPO}/releases/download/${TAG}/${ASSET}"

echo ">>> Downloading ${TAG} (${ASSET})"
if ! curl -fL --progress-bar -o "${TMP}/${ASSET}" "$URL"; then
  echo "xfsr-run: download failed:" >&2
  echo "  ${URL}" >&2
  exit 1
fi

echo ">>> Extracting"
tar -xzf "${TMP}/${ASSET}" -C "$TMP"
STAGE="$(find "$TMP" -maxdepth 1 -type d -name 'xfsr-*' | head -n 1)"
if [ -z "$STAGE" ] || [ ! -f "${STAGE}/bin/xfsr-run" ]; then
  echo "xfsr-run: unexpected archive contents" >&2
  exit 1
fi

echo ">>> Installing into ${PREFIX}"
mkdir -p "$BIN_DIR" "$LIB_DIR" "$LAYER_DIR" "$SHADER_DIR"
install -m 755 "${STAGE}/bin/xfsr-run" "${BIN_DIR}/xfsr-run"
install -m 755 "${STAGE}/lib/libVkLayer_XFSR.so" \
  "${LIB_DIR}/libVkLayer_XFSR.so"
if [ -f "${STAGE}/lib/libxfsr_gl_shim.so" ]; then
  install -m 755 "${STAGE}/lib/libxfsr_gl_shim.so" "${LIB_DIR}/libxfsr_gl_shim.so"
fi
# shaders
if [ -d "${STAGE}/share/xfsr/shaders" ]; then
  cp -a "${STAGE}/share/xfsr/shaders/." "${SHADER_DIR}/"
fi

# Point the layer JSON at the installed .so (absolute path).
SO_PATH="$(cd "$LIB_DIR" && pwd)/libVkLayer_XFSR.so"
JSON_SRC="${STAGE}/share/vulkan/implicit_layer.d/VkLayer_XFSR.json"
JSON_DST="${LAYER_DIR}/VkLayer_XFSR.json"
if command -v python3 >/dev/null 2>&1; then
  SO_PATH="$SO_PATH" JSON_SRC="$JSON_SRC" JSON_DST="$JSON_DST" python3 - <<'PY'
import json, os, pathlib
src = pathlib.Path(os.environ["JSON_SRC"])
dst = pathlib.Path(os.environ["JSON_DST"])
data = json.loads(src.read_text())
data["layer"]["library_path"] = os.environ["SO_PATH"]
dst.write_text(json.dumps(data, indent=2) + "\n")
PY
else
  # Fallback: rewrite library_path with sed (JSON stays valid enough for the loader).
  sed "s|\"library_path\"[[:space:]]*:[[:space:]]*\"[^\"]*\"|\"library_path\": \"${SO_PATH}\"|" \
    "$JSON_SRC" > "$JSON_DST"
fi

# PATH hint
case ":${PATH}:" in
  *":${BIN_DIR}:"*) ;;
  *)
    echo
    echo "Add ${BIN_DIR} to your PATH, e.g.:"
    echo "  echo 'export PATH=\"${BIN_DIR}:\$PATH\"' >> ~/.bashrc && source ~/.bashrc"
    ;;
esac

echo
echo ">>> Installed xfsr-run ${VER}"
echo "    ${BIN_DIR}/xfsr-run"
echo "    ${LIB_DIR}/libVkLayer_XFSR.so"
echo
echo "Steam → Launch Options:"
echo "  xfsr-run --mangohud --preset performance --no-rcas %command%"
echo
echo "Verify:  xfsr-run --help"
