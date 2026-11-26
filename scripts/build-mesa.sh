#!/usr/bin/env bash
# Wisp: Mesa for the web process - EGL (surfaceless) + OpenGL ES on Zink on the kit's RADV, all static.
#
#   bash ~/wisp/scripts/build-mesa.sh [setup]
#
# Source: ~/wisp-build/src/mesa, a worktree of the kit's Mesa (~/ps4kit-build/src/mesa, branch ps4-kit:
# 26.0.8 + the PS4 series) on branch wisp-egl: Etterna's six Zink commits (the OpenCE Halo port's Zink
# loader and headless kopper surface, glthread cores, the tc fence-lock leak fix) cherry-picked, then
# Wisp's own: EGL and the Gallium DRI library as static libraries (exported to patches/mesa/).
# Output: ~/wisp-build/build/mesa, the libraries listed in .wisp-libs (the web process links them all).
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
SRC="$WISP_SRC/mesa"
B="$WB/build/mesa"
CROSS="$PSB/mesa-ps4-cross.ini"
[ -d "$SRC" ] || { echo "no $SRC (git -C ~/ps4kit-build/src/mesa worktree add -b wisp-egl $SRC ps4-kit, then the patches)" >&2; exit 1; }

BUILD_ID="$(git -C "$SRC" rev-parse HEAD | sha1sum | cut -c1-40)"
[ "${1:-}" = setup ] && rm -rf "$B"
if [ ! -f "$B/build.ninja" ]; then
    meson setup "$B" "$SRC" -Dbuildtype=release -Db_ndebug=true -Ddefault_library=static -Dvulkan-drivers=amd \
        -Dgallium-drivers=zink -Dplatforms= -Dllvm=disabled -Damd-use-llvm=false -Dopengl=true -Dgles1=disabled \
        -Dgles2=enabled -Dglx=disabled -Degl=enabled -Dgbm=disabled -Dglvnd=disabled -Dzstd=disabled \
        -Dexpat=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
        -Dbuild-tests=false -Dshader-cache=enabled -Dvulkan-layers= -Dtools= -Dvideo-codecs= \
        -Dspirv-tools=disabled -Dperfetto=false -Dselinux=false -Dgallium-va=disabled -Dgallium-rusticl=false \
        -Dgallium-extra-hud=false -Dps4-gl=false -Dradv-build-id="$BUILD_ID" --cross-file="$CROSS" \
        > "$B.setup.log" 2>&1 || { tail -40 "$B.setup.log"; exit 1; }
fi

ninja -C "$B" -t targets all 2>/dev/null | cut -d: -f1 | grep '\.a$' | sort > "$B/.targets"
# Every static library except tools'/tests' (the web process links them in one group).
grep -v -e '/tests\?/' -e 'gtest' "$B/.targets" > "$B/.wisp-libs"
ninja -C "$B" -j"$WISP_JOBS" $(cat "$B/.wisp-libs")
echo "mesa: $(git -C "$SRC" log --oneline ps4-kit..HEAD | wc -l) commits on the kit's Mesa, $(wc -l < "$B/.wisp-libs") libraries in $B"
