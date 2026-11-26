#!/usr/bin/env bash
# OrbisWeb: RARLAB's UnRAR library (its DLL API, dll.hpp) for Files' RAR archives (v00.27 on): every RAR
# version, passwords (encrypted files and encrypted file lists), multi-part sets. libarchive reads RAR
# too, but decrypts none of it ("Reading encrypted data is not currently supported", "RAR encryption
# support unavailable").
#
#   bash ~/wisp/scripts/build-unrar.sh          # the PS4's library, then the PC's (Files' PC test)
#
# Source: unrarsrc-7.3.2.tar.gz from https://www.rarlab.com/rar/ (downloaded 2026-11-22 with the
# user's go-ahead; RARLAB publishes no signature or sum: the SHA-256 below is the file as it came),
# unpacked fresh into ~/wisp-build/src/unrar-7.3.2 and built unmodified. Its license (license.txt,
# copied to ~/wisp-build/unrar/share and into the package as wisp/licenses/unrar.txt) lets any program
# use it to handle RAR archives free of charge, but not to make RAR archives or re-create RAR's
# compression.
#
# Built as the makefile's "lib" target is (its objects, -DRARDLL: SILENT, no console), but:
#  - without RAR_SMP (the makefile's default): no thread pool; Files unpacks on its job's thread
#  - as the PS4's other C++ (the kit's clang, libc++ and sysroot), C++ exceptions on (UnRAR reports
#    errors by throwing; aria2's library in the same process does as well)
# Out: ~/wisp-build/unrar/{lib/libunrar.a,include/unrar/dll.hpp,share/license.txt} (PS4),
#      ~/wisp-build/host-unrar/... (the PC's, for the PC test with clang++ -stdlib=libc++).
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"

VER=7.3.2
TARBALL=unrarsrc-$VER.tar.gz
SHA256=f61a1160d6e2ebbda2ad353c8e82f62ab4312f97fde23c6c2c982d25e0014f99
URL=https://www.rarlab.com/rar/$TARBALL
S="$WISP_SRC"; SRC="$S/unrar-$VER"
B="$WB/build/unrar"; PREFIX="$WB/unrar"
HB="$WB/build/unrar-host"; HPREFIX="$WB/host-unrar"
LOG="$WISP_LOGS/unrar-build.log"
mkdir -p "$S" "$WISP_LOGS"
exec > >(tee "$LOG") 2>&1

# the makefile's lib objects (OBJECTS + LIB_OBJ)
OBJECTS=(rar strlist strfn pathfn smallfn global file filefn filcreat archive arcread unicode system crypt crc
    rawread encname resource match timefn rdwrfn consio options errhnd rarvm secpassword rijndael getbits sha1
    sha256 blake2s hash extinfo extract volume list find unpack headers threadpool rs16 cmddata ui largepage
    filestr scantree dll qopen)
DEFINES=(-DRARDLL -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE)
WARN=(-Wno-logical-op-parentheses -Wno-switch -Wno-dangling-else -Wno-unused-result -Wno-deprecated-declarations)

if [ ! -f "$S/$TARBALL" ]; then
    echo "unrar: $TARBALL is not in $S (download it from $URL)" >&2
    exit 1
fi
echo "$SHA256  $S/$TARBALL" | sha256sum -c --quiet - || { echo "unrar: $TARBALL is not the file this script knows" >&2; exit 1; }
rm -rf "$SRC"; mkdir -p "$SRC"
tar xzf "$S/$TARBALL" -C "$SRC" --strip-components=1

build() {  # build <name> <out dir> <prefix> <compiler and flags...>
    local name=$1 out=$2 prefix=$3; shift 3
    rm -rf "$out"; mkdir -p "$out" "$prefix/lib" "$prefix/include/unrar" "$prefix/share"
    local o pids=() n=0
    for o in "${OBJECTS[@]}"; do
        "$@" -std=c++17 -O2 "${DEFINES[@]}" "${WARN[@]}" -c "$SRC/$o.cpp" -o "$out/$o.o" &
        pids+=($!)
        n=$((n + 1))
        if [ $((n % WISP_JOBS)) -eq 0 ]; then
            for p in "${pids[@]}"; do wait "$p"; done
            pids=()
        fi
    done
    for p in "${pids[@]}"; do wait "$p"; done
    local objs=()
    for o in "${OBJECTS[@]}"; do
        [ -f "$out/$o.o" ] || { echo "unrar ($name): $o.cpp did not compile" >&2; exit 1; }
        objs+=("$out/$o.o")
    done
    rm -f "$prefix/lib/libunrar.a"
    llvm-ar-$CLANG_VER rcs "$prefix/lib/libunrar.a" "${objs[@]}"
    cp "$SRC/dll.hpp" "$prefix/include/unrar/dll.hpp"
    cp "$SRC/license.txt" "$prefix/share/license.txt"
    echo "unrar ($name): $(ls -la "$prefix/lib/libunrar.a" | awk '{print $5}') bytes, ${#OBJECTS[@]} objects -> $prefix/lib/libunrar.a"
}

# shellcheck disable=SC2086
build PS4 "$B" "$PREFIX" clang++-$CLANG_VER $WISP_CXXFLAGS
# the PC's: libc++ as on the PS4 when its headers are there, else the system's C++ library (the PC
# test links with the same choice)
HOST_CXX=(clang++-$CLANG_VER -stdlib=libc++)
echo '#include <functional>' | "${HOST_CXX[@]}" -x c++ -fsyntax-only - 2>/dev/null || HOST_CXX=(clang++-$CLANG_VER)
build PC "$HB" "$HPREFIX" "${HOST_CXX[@]}" -fPIC -g1
echo "unrar: done"
