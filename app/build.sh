#!/usr/bin/env bash
# Wisp: build the three processes and package them (WSL).
#   WISP_VERSION=00.01 bash ~/wisp/app/build.sh [clean]
# Needs ~/wisp-build ready: scripts/setup-tree.sh, build-deps.sh, build-mesa.sh, build-webkit.sh,
# pack-webkit.sh. Output: out/OrbisWeb-vXX.YY.pkg (title WISP00001, shown as OrbisWeb); upload from Windows (Git Bash):
#   ~/ps4-kit/scripts/upload.sh out/OrbisWeb-vXX.YY.pkg <console-ip>
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
VERSION="${WISP_VERSION:-00.00}"
HERE="$WISP/app"
B="$WB/build/app"
[ "${1:-}" = clean ] && rm -rf "$B"
mkdir -p "$B" "$WISP_LOGS"

if [ ! -f "$B/build.ninja" ]; then
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$HERE" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WISP/scripts/wisp-toolchain.cmake" -DWISP_NO_POISON=ON \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWISP_VERSION="$VERSION" \
        > "$WISP_LOGS/app-configure.log" 2>&1 || { tail -30 "$WISP_LOGS/app-configure.log"; exit 1; }
else
    cmake -DWISP_VERSION="$VERSION" "$B" > /dev/null
fi
ninja -C "$B" -j"$WISP_JOBS" > "$WISP_LOGS/app-build.log" 2>&1 \
    || { grep -E "error|undefined|duplicate" "$WISP_LOGS/app-build.log" | head -60; exit 1; }
bash "$HERE/package.sh" "$VERSION"
