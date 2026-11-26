#!/usr/bin/env bash
# Wisp: WPE WebKit 2.54.2 for the PS4 (static libraries; the app links its three processes from them).
#
#   bash ~/wisp/scripts/build-webkit.sh [setup] [ninja targets...]
#
# Source: ~/wisp-build/src/WebKit (git, branch wisp: the release tarball + the PS4 commits, exported to
# patches/webkit/). Build: ~/wisp-build/build/webkit-wasm (v00.21 on: with WebAssembly), or
# WISP_WEBKIT_DIR (build/webkit-small: the same without WebAssembly, v00.04-v00.20, with
# WISP_WEBKIT_WASM=0; build/webkit: the first, -O3 build). Log: ~/wisp-build/logs/<build folder>-build.log.
# WISP_WEBKIT_JIT=1 (v00.27 on): the web process's copy, with JavaScriptCore's JIT and DFG tiers, in
# build/webkit-jit (the UI and network processes keep webkit-wasm, without them: ~6 MiB less image each).
# No targets: every library pack-webkit.sh packs.
# WISP_JOBS (default 8): WebCore objects take 1-3 GB each; WSL has 16 GB.
#
# Size: each process's whole program image comes out of the flexible memory pool the app's
# processes share (~448 MiB at start), so the code is built for size: one section per function and
# per datum (the app's --gc-sections then drops what a process never reaches; without them a unified
# source bundle stayed whole for one function, and the UI and network process carried all of
# WebKit: ~130 MiB each), -O2, and no unwind tables (WebKit has no C++ exceptions; the kit's crash
# log reads stacks without them). -mprefer-vector-width=128: the vectorizer's stores need at most 16-byte
# alignment, which is what malloc gives (v00.05 faulted on a 32-byte vmovaps into an over-aligned
# class that came from malloc; Jaguar splits 256-bit operations in two anyway). Applied when a build
# folder is configured, and to an existing one whose cached flags differ (then everything rebuilds).
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
SRC="$WISP_SRC/WebKit"
# JavaScriptCore's JIT (v00.27 on, the web process's build): Baseline and DFG, the regular expression
# and CSS selector JITs with them.
if [ "${WISP_WEBKIT_JIT:-0}" = 1 ]; then
    JIT_SWITCH=ON
    B="${WISP_WEBKIT_DIR:-$WB/build/webkit-jit}"
else
    JIT_SWITCH=OFF
    B="${WISP_WEBKIT_DIR:-$WB/build/webkit-wasm}"
fi
# WebAssembly's JIT (v00.34 on, the web process's build): BBQ, and OMG for the hottest functions. Up to
# v00.33 IPInt, the interpreter, ran every game: the kart game spent 12 s and more on the main thread
# loading. BBQ is built on B3's common parts and OMG on B3 itself, which come with the FTL, so the FTL is
# built in too (more image); the web process keeps it off for JavaScript (JSC_useFTLJIT, settings: js_ftl).
# WISP_WEBKIT_WASM_JIT=0: IPInt alone. Switching it reconfigures the build folder (everything rebuilds).
WASM_JIT_SWITCH=$([ "$JIT_SWITCH" = ON ] && [ "${WISP_WEBKIT_WASM:-1}" = 1 ] && [ "${WISP_WEBKIT_WASM_JIT:-1}" = 1 ] && echo ON || echo OFF)
# WebGL 1 and 2 (v00.30 on, the web process's build): ANGLE (Source/ThirdParty/ANGLE) on the GPU web
# process's Mesa EGL (Zink), found through wisp_angle_egl_lookup (WebKit patch 0029: no dlopen). The UI
# and network processes' build stays without it (no ANGLE in their images); patch 0029 keeps what the
# processes send each other the same in both builds. WISP_WEBKIT_WEBGL=0/1 overrides. Switching it
# reconfigures an existing build folder (everything rebuilds).
WEBGL_SWITCH=$([ "${WISP_WEBKIT_WEBGL:-${WISP_WEBKIT_JIT:-0}}" = 1 ] && echo ON || echo OFF)
LOG="$WISP_LOGS/$(basename "$B")-build.log"
SIZE_FLAGS="-O2 -DNDEBUG -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -mprefer-vector-width=128"
# WebGPU for pages (v00.40 on, the web process's build): WebCore's WebGPU Implementation layer ported to Dawn's
# webgpu.h (WebKit patch; docs/WEBGPU.md), compiled with HAVE_WEBGPU_IMPLEMENTATION (PlatformHave.h has it for
# Apple's systems only) and Dawn's headers. Nothing of it crosses between processes, so the UI and network
# processes' build stays without it. The GPU web binary links Dawn; the CPU one gets do-nothing wgpu functions.
# WISP_WEBKIT_WEBGPU=0/1 overrides. A change of flags rebuilds everything.
if [ "${WISP_WEBKIT_WEBGPU:-${WISP_WEBKIT_JIT:-0}}" = 1 ]; then
    SIZE_FLAGS="$SIZE_FLAGS -DHAVE_WEBGPU_IMPLEMENTATION=1 -isystem $WB/src/dawn/include -isystem $WB/build/dawn/gen/include"
fi
# Media (v00.17 on: <video>, Media Source for YouTube, Web Audio): GStreamer 1.26 with every library and
# plugin in one static library, FFmpeg 8 decoders (scripts/build-media.sh, ~/wisp-build/media). Its .pc
# files come first, in pkg-config's path and in CMAKE_PREFIX_PATH (CMake's FindPkgConfig makes that
# pkg-config's PKG_CONFIG_PATH, which goes before PKG_CONFIG_LIBDIR): the kit's sysroot has FFmpeg 7.1's,
# and gstreamer-full needs FFmpeg 8's. WISP_WEBKIT_MEDIA=0: a build without media. Switching it on or off
# reconfigures an existing build folder (most of WebCore rebuilds).
MEDIA="${WISP_WEBKIT_MEDIA:-1}"
if [ "$MEDIA" = 1 ]; then
    export PKG_CONFIG_LIBDIR="$WB/media/lib/pkgconfig:$PKG_CONFIG_LIBDIR"
    MEDIA_SWITCH=ON
    PREFIXES="$WB/media;$WISP_SYSROOT;$PS4_SYSROOT"
else
    MEDIA_SWITCH=OFF
    PREFIXES="$WISP_SYSROOT;$PS4_SYSROOT"
fi
MEDIA_FLAGS=(-DENABLE_VIDEO=$MEDIA_SWITCH -DENABLE_WEB_AUDIO=$MEDIA_SWITCH -DENABLE_MEDIA_SOURCE=$MEDIA_SWITCH
    -DUSE_GSTREAMER=$MEDIA_SWITCH -DUSE_GSTREAMER_FULL=$MEDIA_SWITCH -DUSE_GSTREAMER_GL=OFF
    -DUSE_GSTREAMER_MPEGTS=OFF -DUSE_GSTREAMER_WEBRTC=OFF)
# WebAssembly (v00.20 on): JavaScriptCore's in-place interpreter (IPInt) runs it without a JIT, as the
# LLInt runs JavaScript (no BBQ or OMG tiers: they are JITs). Every Safari has WebAssembly, and
# Cloudflare's check saw none beside Wisp's Safari user agent (v00.15-18). WISP_WEBKIT_WASM=0: a build
# without it. Switching it reconfigures an existing build folder (everything rebuilds: cmakeconfig.h).
if [ "${WISP_WEBKIT_WASM:-1}" = 1 ]; then WASM_SWITCH=ON; else WASM_SWITCH=OFF; fi
TARGETS=(WPEPlatform WPEPlatformHeadless JavaScriptCore JavaScriptCoreJIT LowLevelInterpreterLib WTF bmalloc PAL Skia
    WebCore WebCoreDOMAndRendering WebCoreInspector WebCoreJSBindings WebCoreStyle WebKitShared WebKitUIProcess
    WebKitWebProcess WebKitNetworkProcess WebKitGPUProcess WebKit xdgmime)

if [ "${1:-}" = setup ]; then rm -rf "$B"; shift; fi
if [ ! -f "$B/build.ninja" ]; then
    mkdir -p "$B"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$WISP/scripts/wisp-toolchain.cmake" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIXES" \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_FLAGS" -DCMAKE_CXX_FLAGS_RELEASE="$SIZE_FLAGS" \
        -DPKG_CONFIG_EXECUTABLE="$WB/host/bin/ps4-pkg-config" -DCMAKE_INSTALL_PREFIX="$WB/install/webkit" \
        -DPORT=WPE -DWISP_PS4=ON \
        -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_DRM=OFF -DENABLE_WPE_PLATFORM_HEADLESS=ON \
        -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
        -DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
        -DUSE_ATK=OFF -DUSE_FLITE=OFF -DENABLE_SPEECH_SYNTHESIS=OFF -DUSE_GBM=OFF -DUSE_LIBDRM=OFF \
        -DUSE_LIBBACKTRACE=OFF -DUSE_LIBHYPHEN=OFF -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LCMS=OFF \
        -DUSE_WOFF2=OFF -DENABLE_XSLT=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_WEBDRIVER=OFF \
        -DUSE_SYSPROF_CAPTURE=OFF -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
        -DUSE_SYSTEM_UNIFDEF=ON -DUNIFDEF_EXECUTABLE=/usr/bin/unifdef \
        -DENABLE_MEDIA_STREAM=OFF -DENABLE_MEDIA_RECORDER=OFF \
        -DENABLE_WEB_CODECS=OFF -DENABLE_ENCRYPTED_MEDIA=OFF -DENABLE_MEDIA_SESSION=OFF \
        "${MEDIA_FLAGS[@]}" \
        -DENABLE_WEBGL=$WEBGL_SWITCH -DENABLE_GPU_PROCESS=OFF -DENABLE_WEBXR=OFF -DENABLE_THUNDER=OFF \
        -DENABLE_GAMEPAD=ON -DENABLE_PDFJS=ON -DENABLE_MINIBROWSER=OFF -DENABLE_API_TESTS=OFF \
        -DENABLE_COG=OFF -DENABLE_WK_WEB_EXTENSIONS=OFF -DUSE_SYSTEM_MALLOC=ON \
        -DENABLE_RESOURCE_USAGE=OFF -DENABLE_MEMORY_SAMPLER=OFF -DENABLE_GEOLOCATION=OFF \
        -DENABLE_JIT=$JIT_SWITCH -DENABLE_DFG_JIT=$JIT_SWITCH -DENABLE_FTL_JIT=$WASM_JIT_SWITCH -DENABLE_WEBASSEMBLY=$WASM_SWITCH \
        -DENABLE_WEBASSEMBLY_BBQJIT=$WASM_JIT_SWITCH -DENABLE_WEBASSEMBLY_OMGJIT=$WASM_JIT_SWITCH -DENABLE_SAMPLING_PROFILER=OFF \
        -DGLIB_COMPILE_RESOURCES_EXECUTABLE="$WB/host/bin/glib-compile-resources" \
        -DUSE_SKIA=ON -DENABLE_DEVELOPER_MODE=OFF \
        2>&1 | tee "$WISP_LOGS/$(basename "$B")-configure.log" | tail -60
    [ -f "$B/build.ninja" ] || { echo "configure failed: $WISP_LOGS/$(basename "$B")-configure.log" >&2; exit 1; }
elif ! grep -qxF "CMAKE_CXX_FLAGS_RELEASE:STRING=$SIZE_FLAGS" "$B/CMakeCache.txt"; then
    echo "build-webkit: new compiler flags for $B (everything rebuilds)"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -DCMAKE_C_FLAGS_RELEASE="$SIZE_FLAGS" -DCMAKE_CXX_FLAGS_RELEASE="$SIZE_FLAGS" \
        >> "$WISP_LOGS/$(basename "$B")-configure.log" 2>&1
fi
if ! grep -qx "ENABLE_VIDEO:BOOL=$MEDIA_SWITCH" "$B/CMakeCache.txt" || ! grep -qxF "CMAKE_PREFIX_PATH:UNINITIALIZED=$PREFIXES" "$B/CMakeCache.txt"; then
    echo "build-webkit: media $MEDIA_SWITCH for $B (most of WebCore rebuilds)"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" "${MEDIA_FLAGS[@]}" -DCMAKE_PREFIX_PATH="$PREFIXES" >> "$WISP_LOGS/$(basename "$B")-configure.log" 2>&1 \
        || { echo "configure failed: $WISP_LOGS/$(basename "$B")-configure.log" >&2; exit 1; }
fi
if ! grep -qx "ENABLE_WEBASSEMBLY:BOOL=$WASM_SWITCH" "$B/CMakeCache.txt"; then
    echo "build-webkit: WebAssembly $WASM_SWITCH for $B (everything rebuilds)"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -DENABLE_WEBASSEMBLY=$WASM_SWITCH -DENABLE_WEBASSEMBLY_BBQJIT=$WASM_JIT_SWITCH -DENABLE_WEBASSEMBLY_OMGJIT=$WASM_JIT_SWITCH \
        >> "$WISP_LOGS/$(basename "$B")-configure.log" 2>&1 \
        || { echo "configure failed: $WISP_LOGS/$(basename "$B")-configure.log" >&2; exit 1; }
fi
if ! grep -qx "ENABLE_WEBGL:BOOL=$WEBGL_SWITCH" "$B/CMakeCache.txt"; then
    echo "build-webkit: WebGL $WEBGL_SWITCH for $B (everything rebuilds)"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -DENABLE_WEBGL=$WEBGL_SWITCH \
        >> "$WISP_LOGS/$(basename "$B")-configure.log" 2>&1 \
        || { echo "configure failed: $WISP_LOGS/$(basename "$B")-configure.log" >&2; exit 1; }
fi
if ! grep -qx "ENABLE_JIT:BOOL=$JIT_SWITCH" "$B/CMakeCache.txt"; then
    echo "build-webkit: JIT $JIT_SWITCH for $B (everything rebuilds)"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -DENABLE_JIT=$JIT_SWITCH -DENABLE_DFG_JIT=$JIT_SWITCH -DENABLE_FTL_JIT=$WASM_JIT_SWITCH \
        -DENABLE_WEBASSEMBLY_BBQJIT=$WASM_JIT_SWITCH -DENABLE_WEBASSEMBLY_OMGJIT=$WASM_JIT_SWITCH \
        >> "$WISP_LOGS/$(basename "$B")-configure.log" 2>&1 \
        || { echo "configure failed: $WISP_LOGS/$(basename "$B")-configure.log" >&2; exit 1; }
fi
if ! grep -qx "ENABLE_WEBASSEMBLY_BBQJIT:BOOL=$WASM_JIT_SWITCH" "$B/CMakeCache.txt"; then
    echo "build-webkit: WebAssembly JIT $WASM_JIT_SWITCH for $B (everything rebuilds)"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -DENABLE_FTL_JIT=$WASM_JIT_SWITCH \
        -DENABLE_WEBASSEMBLY_BBQJIT=$WASM_JIT_SWITCH -DENABLE_WEBASSEMBLY_OMGJIT=$WASM_JIT_SWITCH \
        >> "$WISP_LOGS/$(basename "$B")-configure.log" 2>&1 \
        || { echo "configure failed: $WISP_LOGS/$(basename "$B")-configure.log" >&2; exit 1; }
fi
# (WebGL: ANGLE's two libraries too)
[ "$WEBGL_SWITCH" = ON ] && TARGETS+=(ANGLE GLESv2)
[ $# -gt 0 ] || set -- "${TARGETS[@]}"
# (grep finds nothing when nothing failed or linked: that is no failure)
ninja -C "$B" -j"$WISP_JOBS" "$@" 2>&1 | tee "$LOG" | { grep -E "^FAILED|error:|^\[[0-9]+/[0-9]+\] Linking" || true; } | head -100
test "${PIPESTATUS[0]}" -eq 0
