#!/bin/bash
# OrbisWeb: encrypted archives through Files' own code (archives.cpp + fileops.cpp, test_enc.cpp) on the
# PC, with Wisp's host builds of libarchive (~/wisp-build/host-archive: the PS4's options, OpenSSL's
# libcrypto) and UnRAR (~/wisp-build/host-unrar, scripts/build-unrar.sh). WSL:
#   bash tests/archives/run.sh <folder made by make-archives.sh> [rar]
# "rar": with UnRAR (RAR decrypts); without it, libarchive alone (encrypted RAR expected to fail).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DATA="${1:?the folder make-archives.sh made (a /mnt/c/... path)}"
MODE="${2:-}"
APP="$HERE/../../app/src"
H=~/wisp-build/host-archive
OSSL=~/wisp-build/host-aria2/openssl
W=~/orbisweb-archive-test
rm -rf "$W"; mkdir -p "$W"
cp -r "$DATA"/*.zip "$DATA"/*.7z "$DATA"/*.rar "$DATA"/*.r[0-9][0-9] "$DATA/src" "$W/"
# libc++ as on the PS4 when its headers are there, else the system's (UnRAR's PC build made the same choice)
CXX="clang++-21 -stdlib=libc++"
echo '#include <functional>' | $CXX -x c++ -fsyntax-only - 2>/dev/null || CXX=clang++-21
EXTRA_LIB=()
DEFS=()
if [ "$MODE" = rar ]; then EXTRA_LIB=(~/wisp-build/host-unrar/lib/libunrar.a); DEFS=(-DWISP_HAVE_UNRAR=1 -isystem ~/wisp-build/host-unrar/include); fi
$CXX -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -DWISP_HAVE_LIBARCHIVE=1 "${DEFS[@]}" -isystem "$H/include" \
    -I"$APP/ui/files" -I"$APP/ui/shell" -I"$APP/common" \
    "$APP/ui/files/archives.cpp" "$APP/ui/shell/fileops.cpp" "$HERE/test_enc.cpp" \
    "$H/lib/libarchive.a" "$H/lib/liblzma.a" "$H/lib/libzstd.a" "$H/lib/liblz4.a" "$H/lib/libbz2.a" \
    "${EXTRA_LIB[@]}" -lxml2 "$OSSL/lib/libcrypto.a" -lz -lpthread -ldl -o ~/orbisweb-archive-test-bin
~/orbisweb-archive-test-bin "$W" "$MODE"
