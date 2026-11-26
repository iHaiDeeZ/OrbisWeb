# Wisp's build settings (WSL). Source after nothing else: it sources the kit's toolchain env.
#
#   source ~/wisp/scripts/env.sh
#
# ~/wisp is a space-free link to E:\PS4 Projects\Wisp (made by scripts/setup-tree.sh); the build tree
# is ~/wisp-build (never on the Windows drive: much faster, and flags break on the space).

source "$HOME/ps4-kit/toolchain/env.sh"

export WISP="${WISP:-$HOME/wisp}"
export WB="${WB:-$HOME/wisp-build}"
export WISP_SYSROOT="${WISP_SYSROOT:-$WB/sysroot}"   # our dependencies install here (static)
export WISP_SRC="$WB/src"
export WISP_DEPS_BUILD="$WB/build/deps"
export WISP_LOGS="$WB/logs"
# Not all 20 cores: WebKit's and Mesa's compiles take 1-3 GB each and WSL has 16 GB (PSChrome's
# 20-way WebCore build crashed the VM).
export WISP_JOBS="${WISP_JOBS:-8}"

# .pc files: ours (meson puts FreeBSD hosts' in libdata/pkgconfig), then the kit's (zlib, libpng,
# OpenSSL). PKG_CONFIG_LIBDIR, not _PATH: the host's own .pc files must never be found.
export PKG_CONFIG_LIBDIR="$WISP_SYSROOT/lib/pkgconfig:$WISP_SYSROOT/libdata/pkgconfig:$WISP_SYSROOT/share/pkgconfig:$PS4_SYSROOT/lib/pkgconfig:$PS4_SYSROOT/libdata/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
unset PKG_CONFIG_PATH

# Configure-time poison for the OpenOrbis C library's Linux-only functions (musl's eventfd, pipe2,
# accept4, epoll, inotify, ... make Linux system calls, which are other calls or nothing on the PS4's
# FreeBSD kernel). Linked ahead of libc in every configure check, so those checks fail and the
# libraries take their portable paths. Never linked into the app. (scripts/build-nolinux.sh)
export WISP_NOLINUX="$WISP_SYSROOT/lib/libwisp_nolinux.a"

export PATH="$PSB/host-tools/bin:$PSB/mesa-venv/bin:$WB/host/bin:$PATH"

# The C/C++ flags every dependency is compiled with (the kit toolchain's, plus our sysroot).
# -nostdlibinc: for this target -isysroot does not move the default include path, so a header missing
# from OpenOrbis was found in the HOST's /usr/include (GLib took <linux/netlink.h>); clang's own
# headers (stddef.h, immintrin.h, ...) stay.
WISP_CFLAGS_LIST=(--target=$PS4_TARGET -D_GNU_SOURCE -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -DPS4
    -D__BSD_VISIBLE -D_BSD_SOURCE -fPIC -funwind-tables -march=btver2 -isysroot "$OPENORBIS" -nostdlibinc
    -isystem "$PS4_SYSROOT/libc-overlay" -isystem "$WISP_SYSROOT/include" -isystem "$PS4_SYSROOT/include"
    -isystem "$OPENORBIS/include" -O2 -g1)
# C code is built as generic Unix (-U__FreeBSD__, as WinePS4): clang's FreeBSD target defines
# __FreeBSD__, which sends C libraries to FreeBSD libc calls (closefrom, ...) that musl's headers lack.
# C++ keeps it: the kit's libc++ headers were built with it (they take __PS4__ paths first).
export WISP_CFLAGS="${WISP_CFLAGS_LIST[*]} -U__FreeBSD__"
export WISP_CXXFLAGS="-nostdinc++ -isystem $PS4_SYSROOT/include/c++/v1 ${WISP_CFLAGS_LIST[*]}"
# Link line for configure test programs (ld.lld directly, as the kit's CMake toolchain links).
export WISP_LINK_PRE="-fuse-ld=/usr/bin/ld.lld-$CLANG_VER -nostdlib -Wl,-m,elf_x86_64 -Wl,-pie -Wl,--eh-frame-hdr -Wl,--script,$OPENORBIS/link.x -L$OPENORBIS/lib -L$WISP_SYSROOT/lib -L$PS4_SYSROOT/lib $OPENORBIS/lib/crt1.o"
export WISP_LINK_POST="$WISP_NOLINUX -lkernel -lc -lclang_rt.builtins-x86_64 -lSceLibcInternal -lSceNet"
export WISP_LINK_POST_CXX="$WISP_LINK_POST $PS4_SYSROOT/lib/libc++.a $PS4_SYSROOT/lib/libc++abi.a $OPENORBIS/lib/libunwind.a"

# CMake for a dependency: the kit's toolchain file, our sysroot first, configure checks poisoned.
wisp_cmake() {
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        cmake -DCMAKE_TOOLCHAIN_FILE="$WISP/scripts/wisp-toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$WISP_SYSROOT" \
        -DCMAKE_PREFIX_PATH="$WISP_SYSROOT;$PS4_SYSROOT" -DCMAKE_INSTALL_LIBDIR=lib \
        -DPKG_CONFIG_EXECUTABLE="$WB/host/bin/ps4-pkg-config" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -G Ninja "$@"
}

wisp_meson_cross() { echo "$WB/wisp-cross.ini"; }
