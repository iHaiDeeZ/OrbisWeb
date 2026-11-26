#!/usr/bin/env bash
# Wisp: Dawn (Google's WebGPU, Chromium's) for the PS4, for OrbisWeb's WebGPU (docs/WEBGPU.md): static,
# the Vulkan backend only (the kit's RADV, in the GPU web process), and Tint (WGSL to SPIR-V).
#
#   bash ~/wisp/scripts/build-dawn.sh [setup] [ninja targets...]
#
# Source: ~/wisp-build/src/dawn (git, branch wisp): Dawn v20261120.121236 and the dependencies a
# Vulkan-only build needs, at the revisions its DEPS file pins (abseil-cpp, SPIRV-Tools, SPIRV-Headers,
# Vulkan-Headers, Vulkan-Utility-Libraries, jinja2, markupsafe, webgpu-headers: fetched 2026-11-24 with
# the user's go-ahead), then the PS4 commits, exported to patches/dawn/. Build: ~/wisp-build/build/dawn
# (WISP_DAWN_DIR); one static library, libwebgpu_dawn.a. Log: ~/wisp-build/logs/dawn-build.log.
# No targets: webgpu_dawn. Built for size as WebKit is (scripts/build-webkit.sh): one section per
# function, -O2, no unwind tables (Dawn and Tint use no C++ exceptions).
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
SRC="$WISP_SRC/dawn"
B="${WISP_DAWN_DIR:-$WB/build/dawn}"
LOG="$WISP_LOGS/dawn-build.log"
SIZE_FLAGS="-O2 -DNDEBUG -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -mprefer-vector-width=128"

if [ "${1:-}" = setup ]; then rm -rf "$B"; shift; fi
if [ ! -f "$B/build.ninja" ]; then
    mkdir -p "$B"
    # Only Vulkan: no D3D, Metal, GL or the null backend, no window systems (the PS4 presents nothing
    # itself: frames go to WebKit's compositor). Tint: the WGSL reader and the SPIR-V writer Vulkan needs
    # (and the WGSL writer Dawn uses for messages); no SPIR-V input, GLSL, HLSL, MSL, tools or tests.
    # TINT_BUILD_AS_OTHER_OS: Tint's *_other.cc files for an OS it doesn't know.
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    cmake -S "$SRC" -B "$B" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$WISP/scripts/wisp-toolchain.cmake" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="$SIZE_FLAGS" -DCMAKE_CXX_FLAGS_RELEASE="$SIZE_FLAGS" \
        -DPython3_EXECUTABLE=/usr/bin/python3 \
        -DBUILD_SHARED_LIBS=OFF -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DDAWN_FETCH_DEPENDENCIES=OFF \
        -DDAWN_ENABLE_VULKAN=ON -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_METAL=OFF \
        -DDAWN_ENABLE_NULL=OFF -DDAWN_ENABLE_WEBGPU_ON_WEBGPU=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF \
        -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_SPIRV_VALIDATION=OFF -DDAWN_ENABLE_SWIFTSHADER=OFF \
        -DDAWN_USE_WAYLAND=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_GLFW=OFF -DDAWN_USE_WINDOWS_UI=OFF \
        -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_NODE_BINDINGS=OFF -DDAWN_BUILD_BENCHMARKS=OFF \
        -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_BUILD_FUZZERS=OFF -DDAWN_ENABLE_INSTALL=OFF \
        -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_WGSL_READER=ON \
        -DTINT_BUILD_SPV_WRITER=ON -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_GLSL_WRITER=OFF \
        -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_HLSL_WRITER=OFF -DTINT_BUILD_MSL_WRITER=OFF \
        -DTINT_BUILD_NULL_WRITER=OFF -DTINT_BUILD_IR_BINARY=OFF -DTINT_BUILD_TESTS=OFF \
        -DTINT_BUILD_BENCHMARKS=OFF -DTINT_BUILD_FUZZERS=OFF -DTINT_ENABLE_IR_DUMPING=OFF \
        -DTINT_ENABLE_IR_VALIDATION_ASSERTS=OFF -DTINT_BUILD_AS_OTHER_OS=ON \
        2>&1 | tee "$WISP_LOGS/dawn-configure.log" | tail -40
    [ -f "$B/build.ninja" ] || { echo "configure failed: $WISP_LOGS/dawn-configure.log" >&2; exit 1; }
fi
[ $# -gt 0 ] || set -- webgpu_dawn
# (grep finds nothing when nothing failed or linked: that is no failure)
ninja -C "$B" -j"$WISP_JOBS" "$@" 2>&1 | tee "$LOG" | { grep -E "^FAILED|error:|^\[[0-9]+/[0-9]+\] Linking" || true; } | head -100
test "${PIPESTATUS[0]}" -eq 0
