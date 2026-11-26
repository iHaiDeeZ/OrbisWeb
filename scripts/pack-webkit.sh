#!/usr/bin/env bash
# Wisp: every WebKit object the app's three processes link, in one thin archive
# (~/wisp-build/build/webkit/libwisp-webkit.a). WPE's static libWPEWebKit-2.0.a has only the WebKit
# layer: WebCore, JavaScriptCore, WTF and WPEPlatform are CMake object libraries. Thin: the archive
# names the objects in the build tree instead of copying ~1 GB of them.
#   bash ~/wisp/scripts/pack-webkit.sh        (WISP_WEBKIT_JIT=1: the web process's JIT build, build/webkit-jit)
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
if [ "${WISP_WEBKIT_JIT:-0}" = 1 ]; then
    B="${WISP_WEBKIT_DIR:-$WB/build/webkit-jit}"
else
    B="${WISP_WEBKIT_DIR:-$WB/build/webkit-wasm}"
fi
OUT="$B/libwisp-webkit.a"
DIRS=(
    Source/JavaScriptCore/CMakeFiles/JavaScriptCore.dir
    Source/JavaScriptCore/CMakeFiles/JavaScriptCoreJIT.dir
    Source/JavaScriptCore/CMakeFiles/LowLevelInterpreterLib.dir
    Source/WTF/wtf/CMakeFiles/WTF.dir
    Source/bmalloc/CMakeFiles/bmalloc.dir
    Source/WebKit/WPEPlatform/CMakeFiles/WPEPlatform.dir
    Source/WebKit/WPEPlatform/wpe/headless/CMakeFiles/WPEPlatformHeadless.dir
)
# WebGL (v00.30 on, the web process's build): ANGLE's libraries
if grep -qx "ENABLE_WEBGL:BOOL=ON" "$B/CMakeCache.txt"; then
    DIRS+=(Source/ThirdParty/ANGLE/CMakeFiles/ANGLE.dir Source/ThirdParty/ANGLE/CMakeFiles/GLESv2.dir)
fi
# WebCore's and WebKit's parts, whatever they are called in this version
for d in "$B"/Source/WebCore/CMakeFiles/*.dir "$B"/Source/WebKit/CMakeFiles/*.dir; do
    case "$(basename "$d")" in
        WebProcess.dir|NetworkProcess.dir|GPUProcess.dir|WPEInjectedBundle.dir|*Test*|*test*) ;;   # executables, a module
        *) DIRS+=("${d#$B/}") ;;
    esac
done
LIST="$B/wisp-objects.txt"
: > "$LIST"
# the objects this configuration builds (ninja's list), not every object on disk: sources a changed
# option takes out of the build (the JIT's, v00.07) leave their old objects behind
ninja -C "$B" -t targets all | sed -n 's/^\(.*\.\(obj\|o\)\): .*/\1/p' | sort -u > "$B/wisp-ninja-objects.txt"
for d in "${DIRS[@]}"; do
    [ -d "$B/$d" ] || { echo "(no $d)"; continue; }
    grep -F "$d/" "$B/wisp-ninja-objects.txt" | while read -r o; do
        [ -f "$B/$o" ] && echo "$B/$o"
    done >> "$LIST"
done
rm -f "$OUT"
llvm-ar-$CLANG_VER rcsT "$OUT" @"$LIST"
echo "pack-webkit: $(wc -l < "$LIST") objects from ${#DIRS[@]} libraries -> $OUT"

# WebKit's xdgmime and GLib's both define _caches and _xdg_binary_or_text_fallback (the two names
# their XDG_PREFIX renaming misses): WebKit's copy gets its own names
llvm-objcopy-$CLANG_VER --redefine-sym _caches=_wisp_wk_xdg_caches \
    --redefine-sym _xdg_binary_or_text_fallback=_wisp_wk_xdg_binary_or_text_fallback \
    "$B/lib/libxdgmime.a" "$B/lib/libxdgmime-wisp.a"
