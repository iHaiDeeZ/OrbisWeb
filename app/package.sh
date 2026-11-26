#!/usr/bin/env bash
# Wisp: stage and package a built Wisp (app/build.sh calls this).
#   bash ~/wisp/app/package.sh XX.YY
#
# /app0/eboot.bin              the UI process
# /app0/wisp-web.bin           WebKit's web process (a local process of the app; CPU rendering)
# /app0/wisp-web-gpu.bin       the web process with Mesa, for GPU rendering (gpu=1)
# /app0/wisp-net.bin           WebKit's network process (a local process of the app)
# /app0/wisp/icudt77l.dat      ICU's data (ICU_DATA)
# /app0/wisp/fonts.conf, fonts/ fontconfig's setup and the packaged fonts (Liberation, Roboto, and
#                              fonts/noto: Noto for every other script, CJK and emoji)
# /app0/wisp/cacert.pem        the CA bundle TLS checks against (SSL_CERT_FILE)
# /app0/wisp/licenses/unrar.txt UnRAR's license (Files reads RAR archives with it)
# /app0/fonts/                 the toolbar's font (Roboto: the canvas's fallback when the PS4's
#                              /preinst fonts are not readable)
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
VERSION="${1:?version XX.YY}"
HERE="$WISP/app"
B="$WB/build/app"
STAGE="$B/stage"
OUT="$WISP/out"

rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys" "$STAGE/wisp/fonts" "$STAGE/fonts" "$OUT" "$WB/oelf"
cp "$B/wisp_eboot/eboot.bin" "$STAGE/eboot.bin"
cp "$B/wisp_web_eboot/eboot.bin" "$STAGE/wisp-web.bin"
cp "$B/wisp_web_gpu_eboot/eboot.bin" "$STAGE/wisp-web-gpu.bin"
cp "$B/wisp_net_eboot/eboot.bin" "$STAGE/wisp-net.bin"
cp "$HERE/sce_sys/icon0.png" "$STAGE/sce_sys/"
[ -f "$HERE/sce_sys/pic1.png" ] && cp "$HERE/sce_sys/pic1.png" "$STAGE/sce_sys/"
icudat="$(find "$WISP_SYSROOT/share/icu" -name 'icudt*l.dat' | head -1)"
[ -n "$icudat" ] || icudat="$(find "$WB/build/deps/icu" -name 'icudt*l.dat' | head -1)"
[ -n "$icudat" ] || { echo "no ICU data file (icudt*l.dat)" >&2; exit 1; }
cp "$icudat" "$STAGE/wisp/"
cp "$HERE"/data/fonts/* "$STAGE/wisp/fonts/"
# Noto for every other script (fetched once into the build tree: scripts/get-noto-fonts.sh)
NOTO="$WB/fonts/noto"
[ -f "$NOTO/NotoNaskhArabic-Regular.ttf" ] && [ -f "$NOTO/NotoSansCJK-Regular.ttc" ] \
    || { echo "no Noto fonts in $NOTO: bash ~/wisp/scripts/get-noto-fonts.sh" >&2; exit 1; }
mkdir -p "$STAGE/wisp/fonts/noto"
find "$NOTO" -maxdepth 1 -type f \( -name '*.ttf' -o -name '*.ttc' -o -name '*.otf' -o -name 'OFL.txt' \) \
    -exec cp {} "$STAGE/wisp/fonts/noto/" \;
cp "$HERE/data/fonts/Roboto-Regular.ttf" "$HERE/data/fonts/Roboto-Bold.ttf" "$HERE/data/fonts/OFL-Roboto.txt" "$STAGE/fonts/"
cp "$HERE/data/fonts.conf" "$STAGE/wisp/fonts.conf"
cp "$HERE/data/ca-certificates.crt" "$STAGE/wisp/cacert.pem"
# extensions' API (data/ext/api.js) and the built-in ad blocker's lists (scripts/abp2webkit.py)
mkdir -p "$STAGE/wisp/ext" "$STAGE/wisp/adblock"
cp "$HERE/data/ext/api.js" "$STAGE/wisp/ext/"
cp "$HERE"/data/adblock/*.json "$STAGE/wisp/adblock/"
mkdir -p "$STAGE/wisp/no-gio-modules"
touch "$STAGE/wisp/no-gio-modules/.keep"
# UnRAR's license (Files' RAR archives, scripts/build-unrar.sh), when it is built in (WISP_WITH_UNRAR=1)
if [ "${WISP_WITH_UNRAR:-0}" = 1 ] && [ -f "$WB/unrar/share/license.txt" ]; then
    mkdir -p "$STAGE/wisp/licenses"
    cp "$WB/unrar/share/license.txt" "$STAGE/wisp/licenses/unrar.txt"
fi

PKG="$(NO_RIGHT_SPRX=1 bash "$HOME/ps4-kit/scripts/make-pkg.sh" "$STAGE" WISP00001 "OrbisWeb" "$VERSION" WISPBROWSER0000 "$B/pkg" | tail -1)"
NAME="OrbisWeb-v$VERSION.pkg"
cp "$PKG" "$OUT/$NAME"
# symbols of every packaged build (crash logs are symbolized against the exact binaries)
for t in wisp wisp_web wisp_web_gpu wisp_net; do
    cp "$B/$t" "$WB/oelf/$t-v$VERSION.elf"
done
ls -la "$OUT/$NAME"
