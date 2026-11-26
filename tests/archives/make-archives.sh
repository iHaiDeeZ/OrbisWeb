#!/usr/bin/env bash
# OrbisWeb: the encrypted archives Files' PC test reads (tests/archives/run.sh), made on Windows (Git Bash)
# with the installed 7-Zip and WinRAR, password "secret":
#   bash tests/archives/make-archives.sh [folder]     (default: $TEMP/orbisweb-archive-test, ~70 MB)
# ZIP: ZipCrypto, AES-256, AES-256 + LZMA. RAR 5 and 4: files encrypted, list encrypted; a set of
# parts (name.partN.rar), RAR 4's old part names (.rar, .r00, ...); a solid archive with copies of
# identical files (RAR 5 file references); a plain RAR; a 64 MiB stored file (Cancel). 7-Zip: AES, files
# and list (still not supported: libarchive's).
set -euo pipefail
OUT="${1:-${TEMP:-/tmp}/orbisweb-archive-test}"
Z="/c/Program Files/7-Zip/7z.exe"
R="/c/Program Files/WinRAR/Rar.exe"
[ -x "$Z" ] || { echo "no 7-Zip at $Z" >&2; exit 1; }
[ -x "$R" ] || { echo "no WinRAR at $R" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/src/dir/sub"
cd "$OUT/src"
echo hello > dir/a.txt
head -c 300000 /dev/urandom > dir/sub/b.bin
for i in $(seq 2000); do echo "second file"; done > dir/sub/c.txt
"$Z" a -bso0 -bd -tzip -mem=ZipCrypto -psecret ../zipcrypto.zip dir
"$Z" a -bso0 -bd -tzip -mem=AES256 -psecret ../aes256.zip dir
"$Z" a -bso0 -bd -tzip -mm=LZMA -mem=AES256 -psecret ../aes256-lzma.zip dir
"$Z" a -bso0 -bd -t7z -psecret ../enc.7z dir
"$Z" a -bso0 -bd -t7z -mhe=on -psecret ../enc-headers.7z dir
"$R" a -idq -psecret ../rar5.rar dir
"$R" a -idq -hpsecret ../rar5-headers.rar dir
"$R" a -idq -ma4 -psecret ../rar4.rar dir
"$R" a -idq -ma4 -hpsecret ../rar4-headers.rar dir
"$R" a -idq -psecret -v100k ../vol5.rar dir
"$R" a -idq -ma4 -vn -v100k -psecret ../old4.rar dir
cp -r dir dir2
"$R" a -idq -s -oi:1 -psecret ../solid-copies.rar dir dir2
rm -rf dir2
"$R" a -idq ../plain.rar dir
mkdir -p ../bigsrc
head -c $((64 * 1024 * 1024)) /dev/urandom > ../bigsrc/big.bin
(cd ../bigsrc && "$R" a -idq -m0 -psecret ../big.rar big.bin)
rm -rf ../bigsrc
ls -la "$OUT"
