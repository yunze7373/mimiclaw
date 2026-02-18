#!/bin/bash
# Download Lua 5.4.7 source for the ESP-IDF lua component
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LUA_VERSION="5.4.7"
LUA_URL="https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz"
DEST_DIR="${SCRIPT_DIR}/src"

if [ -f "${DEST_DIR}/lua.h" ]; then
    echo "[OK] Lua ${LUA_VERSION} source already present in ${DEST_DIR}"
    exit 0
fi

echo "Downloading Lua ${LUA_VERSION}..."
TMPDIR=$(mktemp -d)
curl -sL "${LUA_URL}" -o "${TMPDIR}/lua.tar.gz"

echo "Extracting to ${DEST_DIR}..."
mkdir -p "${DEST_DIR}"
tar -xzf "${TMPDIR}/lua.tar.gz" -C "${TMPDIR}"
cp "${TMPDIR}/lua-${LUA_VERSION}/src/"*.c "${DEST_DIR}/"
cp "${TMPDIR}/lua-${LUA_VERSION}/src/"*.h "${DEST_DIR}/"

rm -rf "${TMPDIR}"
echo "[OK] Lua ${LUA_VERSION} source installed to ${DEST_DIR}"
