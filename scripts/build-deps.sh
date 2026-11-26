#!/usr/bin/env bash
# Wisp: WPE WebKit's dependencies for the PS4, static, into ~/wisp-build/sysroot.
#
#   bash ~/wisp/scripts/build-deps.sh            # everything not built yet (build/deps/<name>.done)
#   bash ~/wisp/scripts/build-deps.sh glib soup  # just these (again)
#
# Sources: ~/wisp-build/src/<tarball> (scripts/fetch-deps.sh). Logs: ~/wisp-build/logs/deps-<name>.log.
# zlib, libpng and OpenSSL are the kit's (~/ps4kit-build/sysroot). Every configure check links with the
# poison library ahead of libc (scripts/env.sh), so musl's Linux-only calls are never chosen.
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
S="$WISP_SRC"; B="$WISP_DEPS_BUILD"; L="$WISP_LOGS"
mkdir -p "$B" "$L"
CROSS="$WB/wisp-cross.ini"
HOST_TRIPLE=x86_64-pc-freebsd12

# --- helpers --------------------------------------------------------------------------------------
unpack() {  # unpack <tarball> <dir>: the source tree $S/<dir>, unpacked once
    [ -d "$S/$2" ] || { mkdir -p "$S/.tmp.$2"; tar xf "$S/$1" -C "$S/.tmp.$2";
        mv "$S/.tmp.$2/"* "$S/$2"; rmdir "$S/.tmp.$2"; }
}
ms() {  # ms <name> <srcdir> [meson options]: meson cross build + install
    local name=$1 src=$2; shift 2
    rm -rf "$B/$name"
    meson setup "$B/$name" "$src" --cross-file "$CROSS" --prefix "$WISP_SYSROOT" --libdir lib \
        -Ddefault_library=static -Dbuildtype=release -Db_ndebug=true --wrap-mode=nodownload "$@"
    if [ -n "${MS_TARGETS:-}" ]; then
        # only these targets (tests/tools that cannot link for the PS4 stay unbuilt), then install
        # what was built
        ninja -C "$B/$name" -j"$WISP_JOBS" $MS_TARGETS
        meson install -C "$B/$name" --no-rebuild --skip-subprojects >/dev/null
    else
        ninja -C "$B/$name" -j"$WISP_JOBS"
        ninja -C "$B/$name" install
    fi
}
cm() {  # cm <name> <srcdir> [cmake options]: cmake cross build + install
    local name=$1 src=$2; shift 2
    rm -rf "$B/$name"
    wisp_cmake -S "$src" -B "$B/$name" "$@"
    ninja -C "$B/$name" -j"$WISP_JOBS"
    ninja -C "$B/$name" install
}
ac() {  # ac <name> <srcdir> [configure options]: autotools cross build + install
    local name=$1 src=$2; shift 2
    rm -rf "$B/$name"; mkdir -p "$B/$name"; cd "$B/$name"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
        CC=ps4-cc CXX=ps4-c++ AR=llvm-ar-$CLANG_VER RANLIB=llvm-ranlib-$CLANG_VER NM=llvm-nm-$CLANG_VER \
        STRIP=llvm-strip-$CLANG_VER OBJDUMP=llvm-objdump-$CLANG_VER CC_FOR_BUILD=gcc CPP_FOR_BUILD="gcc -E" \
        PKG_CONFIG=ps4-pkg-config \
        "$src/configure" --host=$HOST_TRIPLE --build=x86_64-pc-linux-gnu --prefix="$WISP_SYSROOT" \
        --libdir="$WISP_SYSROOT/lib" --disable-shared --enable-static "$@"
    make -j"$WISP_JOBS"
    make install
    cd - >/dev/null
}
pcfile() {  # pcfile <name> <version> <libs> [requires]: a .pc for a library built by hand
    cat > "$WISP_SYSROOT/lib/pkgconfig/$1.pc" <<EOF
prefix=$WISP_SYSROOT
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: $1
Description: $1 (Wisp, PS4)
Version: $2
Requires.private: ${4:-}
Libs: -L\${libdir} $3
Cflags: -I\${includedir}
EOF
}

# --- the libraries, in dependency order -------------------------------------------------------------
b_base() {  # the kit's zlib/libpng/OpenSSL .pc files (theirs name CEMUPS4's sysroot) + Khronos headers
    local f
    for f in zlib libpng libpng16 openssl libssl libcrypto; do
        [ -f "$PS4_SYSROOT/lib/pkgconfig/$f.pc" ] || continue
        sed -E -e "s|^prefix=.*|prefix=$PS4_SYSROOT|" -e 's|^exec_prefix=/.*|exec_prefix=${prefix}|' \
            "$PS4_SYSROOT/lib/pkgconfig/$f.pc" > "$WISP_SYSROOT/lib/pkgconfig/$f.pc"
    done
    for d in EGL KHR GLES2 GLES3 GL; do
        rm -rf "$WISP_SYSROOT/include/$d"; cp -a "$PSB/src/mesa/include/$d" "$WISP_SYSROOT/include/$d"
    done
}
b_brotli() {
    unpack brotli-1.1.0.tar.gz brotli
    cm brotli "$S/brotli" -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUILD_TOOLS=OFF
}
b_expat() {
    unpack expat-2.7.1.tar.xz expat
    cm expat "$S/expat" -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
        -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_FUZZERS=OFF -DEXPAT_WITH_GETRANDOM=ON
}
b_libffi() {
    unpack libffi-3.4.8.tar.gz libffi
    ac libffi "$S/libffi" --disable-docs --disable-multi-os-directory --disable-exec-static-tramp
}
b_pcre2() {
    unpack pcre2-10.45.tar.bz2 pcre2
    cm pcre2 "$S/pcre2" -DPCRE2_BUILD_PCRE2_8=ON -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF \
        -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_SUPPORT_JIT=OFF -DPCRE2_SUPPORT_UNICODE=ON
}
b_glib() {
    unpack glib-2.86.5.tar.xz glib
    # (v00.39) GMutex, GCond and bit locks on the kernel's umtx: pthread objects made and destroyed in a
    # loop used up the kernel library's internal pool (YouTube in 27 minutes)
    python3 "$WISP/scripts/patch-glib-futex.py" "$S/glib"
    ms glib "$S/glib" -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dman-pages=disabled \
        -Dtests=false -Dinstalled_tests=false -Dintrospection=disabled -Dnls=disabled -Dsysprof=disabled \
        -Dglib_debug=disabled -Ddtrace=disabled -Dsystemtap=disabled -Dlibelf=disabled \
        -Ddocumentation=false -Dbsymbolic_functions=false -Dglib_assert=false
}
b_libpsl() {
    unpack libpsl-0.21.5.tar.gz libpsl
    ms libpsl "$S/libpsl" -Druntime=no -Dbuiltin=true -Dtests=false -Ddocs=false
}
b_nghttp2() {
    unpack nghttp2-1.65.0.tar.xz nghttp2
    cm nghttp2 "$S/nghttp2" -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF -DBUILD_TESTING=OFF
}
b_sqlite() {  # the amalgamation compiled by hand (its autosetup build is not worth teaching cross)
    unpack sqlite-autoconf-3500100.tar.gz sqlite
    rm -rf "$B/sqlite"; mkdir -p "$B/sqlite"
    clang-$CLANG_VER $WISP_CFLAGS -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DHAVE_USLEEP=1 \
        -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE \
        -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_ENABLE_COLUMN_METADATA -DHAVE_FDATASYNC=1 \
        -c "$S/sqlite/sqlite3.c" -o "$B/sqlite/sqlite3.o"
    llvm-ar-$CLANG_VER rcs "$WISP_SYSROOT/lib/libsqlite3.a" "$B/sqlite/sqlite3.o"
    cp "$S/sqlite/sqlite3.h" "$S/sqlite/sqlite3ext.h" "$WISP_SYSROOT/include/"
    pcfile sqlite3 3.50.1 "-lsqlite3"
}
b_soup() {
    unpack libsoup-3.6.6.tar.xz libsoup
    ms soup "$S/libsoup" -Dgssapi=disabled -Dntlm=disabled -Dbrotli=enabled -Dtls_check=false \
        -Dintrospection=disabled -Dvapi=disabled -Ddocs=disabled -Dtests=false -Dautobahn=disabled \
        -Dsysprof=disabled -Dpkcs11_tests=disabled
}
b_glibnet() {  # only the static TLS module (the app calls g_io_openssl_load (NULL)); its shared
               # module, which meson always links too, cannot link for the PS4
    unpack glib-networking-2.80.1.tar.xz glib-networking
    rm -rf "$B/glibnet"
    meson setup "$B/glibnet" "$S/glib-networking" --cross-file "$CROSS" --prefix "$WISP_SYSROOT" --libdir lib \
        -Ddefault_library=static -Dbuildtype=release -Db_ndebug=true --wrap-mode=nodownload \
        -Dgnutls=disabled -Dopenssl=enabled -Dlibproxy=disabled -Dgnome_proxy=disabled -Dinstalled_tests=false
    ninja -C "$B/glibnet" -j"$WISP_JOBS" tls/openssl/libgioopenssl.a
    cp "$B/glibnet/tls/openssl/libgioopenssl.a" "$WISP_SYSROOT/lib/"
    pcfile gioopenssl 2.80.1 "-lgioopenssl" "gio-2.0 openssl"
}
b_libxml2() {
    unpack libxml2-2.14.4.tar.xz libxml2
    cm libxml2 "$S/libxml2" -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ICU=OFF \
        -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF \
        -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_ZLIB=ON -DLIBXML2_WITH_THREADS=ON -DLIBXML2_WITH_ICONV=OFF
}
b_tasn1() {
    unpack libtasn1-4.21.0.tar.gz libtasn1
    ac tasn1 "$S/libtasn1" --disable-doc --disable-valgrind-tests
}
b_gpgerror() {
    unpack libgpg-error-1.61.tar.bz2 libgpg-error
    # The lock object: musl's pthread_mutex_t, which the OpenOrbis headers declare.
    local sc="$S/libgpg-error/src/syscfg"
    local t
    for t in x86_64-pc-freebsd12 x86_64-unknown-freebsd12; do
        cp "$sc/lock-obj-pub.x86_64-unknown-linux-musl.h" "$sc/lock-obj-pub.$t.h"
    done
    ac gpgerror "$S/libgpg-error" --disable-doc --disable-tests --disable-languages --disable-nls \
        --enable-install-gpg-error-config
}
b_gcrypt() {
    unpack libgcrypt-1.12.4.tar.bz2 libgcrypt
    ac gcrypt "$S/libgcrypt" --disable-doc --with-libgpg-error-prefix="$WISP_SYSROOT" \
        --disable-jent-support --disable-asm
}
b_freetype() {
    unpack freetype-2.13.3.tar.xz freetype
    cm freetype "$S/freetype" -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON -DFT_REQUIRE_BROTLI=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BZIP2=ON
}
b_icu() {  # ICU 77.1 (PSChrome's source and host build: the host tools a cross build needs)
    [ -d "$S/icu" ] || cp -a "$HOME/pschrome-build/src/icu" "$S/icu"
    local host="$HOME/pschrome-build/build/icu-host"
    [ -x "$host/bin/icupkg" ] || { echo "no ICU host build at $host" >&2; return 1; }
    # (ICU's own version renaming stays on: --disable-renaming builds plain names, but the installed
    # headers still ask for utext_close_77 and the like, so every user of ICU failed to link)
    ac icu "$S/icu/source" --with-cross-build="$host" --with-data-packaging=archive --disable-tests \
        --disable-samples --disable-extras --disable-tools --disable-icuio --disable-layoutex
}
b_harfbuzz() {
    unpack harfbuzz-11.2.1.tar.xz harfbuzz
    ms harfbuzz "$S/harfbuzz" -Dfreetype=enabled -Dicu=enabled -Dglib=disabled -Dgobject=disabled \
        -Dcairo=disabled -Dchafa=disabled -Dtests=disabled -Ddocs=disabled -Dintrospection=disabled \
        -Dutilities=disabled -Dicu_builtin=false -Dbenchmark=disabled
}
b_fontconfig() {
    unpack fontconfig-2.16.0.tar.xz fontconfig
    ms fontconfig "$S/fontconfig" -Ddoc=disabled -Dtests=disabled -Dtools=disabled -Dcache-build=disabled \
        -Dnls=disabled -Dxml-backend=expat
}
b_jpeg() {
    unpack libjpeg-turbo-3.1.0.tar.gz libjpeg-turbo
    cm jpeg "$S/libjpeg-turbo" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_SIMD=ON \
        -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
}
b_webp() {
    unpack libwebp-1.5.0.tar.gz libwebp
    cm webp "$S/libwebp" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
        -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=ON
}
b_xkbcommon() {
    unpack xkbcommon-1.10.0.tar.gz libxkbcommon
    MS_TARGETS=libxkbcommon.a ms xkbcommon "$S/libxkbcommon" -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
        -Denable-tools=false -Denable-xkbregistry=false -Denable-bash-completion=false
}
b_epoxy() {
    unpack libepoxy-1.5.10.tar.gz libepoxy
    python3 "$WISP/scripts/patch-epoxy.py" "$S/libepoxy"   # GL through wisp_gl_lookup (no dlopen)
    ms epoxy "$S/libepoxy" -Dglx=no -Degl=yes -Dx11=false -Dtests=false -Ddocs=false
}

ALL=(base brotli expat libffi pcre2 glib libpsl nghttp2 sqlite soup glibnet libxml2 tasn1 gpgerror gcrypt
     freetype icu harfbuzz fontconfig jpeg webp xkbcommon epoxy)
want=("$@"); [ ${#want[@]} -gt 0 ] || want=("${ALL[@]}")
for name in "${want[@]}"; do
    if [ $# -eq 0 ] && [ -f "$B/$name.done" ]; then continue; fi
    echo "== $name"
    # Not inside an `if`: bash ignores set -e in a condition, subshells included, and a failed
    # configure was taken for a built library (libgpg-error, 2026-11-17).
    set +e
    ( set -euo pipefail; "b_$name" ) > "$L/deps-$name.log" 2>&1
    rc=$?
    set -e
    if [ $rc -eq 0 ]; then
        touch "$B/$name.done"
    else
        echo "!! $name failed: $L/deps-$name.log"; tail -25 "$L/deps-$name.log"; exit 1
    fi
done
echo "deps: all built in $WISP_SYSROOT"
