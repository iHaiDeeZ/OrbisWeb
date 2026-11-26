#!/usr/bin/env bash
# Wisp: libarchive for the Files app (open, extract and create archives the way Explorer and 7-Zip
# do), with the compression libraries it needs, static and -fPIC, in their own prefix
# ~/wisp-build/archive (include, lib, lib/pkgconfig). Nothing in the sysroot, media or aria2 changes.
#
#   bash ~/wisp/scripts/build-archive.sh                 # what isn't built yet (build/archive/<name>.done),
#                                                        # then the PS4 link test
#   bash ~/wisp/scripts/build-archive.sh xz libarchive   # just these (again); linktest = the link test
#   bash ~/wisp/scripts/build-archive.sh fetch           # only download the sources
#   bash ~/wisp/scripts/build-archive.sh host            # the same libraries for Linux in
#                                                        # ~/wisp-build/host-archive, and the API test run on
#                                                        # archives made on the PC (summary at the end)
#
# Versions: libarchive 3.8.11, xz 5.8.4 (liblzma only), zstd 1.5.7, lz4 1.10.0, bzip2 1.0.8 (libraries
# only: no programs, tests or docs). zlib and OpenSSL 3 are the kit's, libxml2 (xar) the sysroot's.
# Sources: ~/wisp-build/src/<tarball> (downloaded when missing, checked against the SHA-256 sums below;
# the GPG signatures of libarchive's (Martin Matuska), xz's (Lasse Collin), zstd's and bzip2's (Mark
# Wielaard) tarballs and lz4's published SHA-256 checked out on 2026-11-20). Build folders:
# ~/wisp-build/build/archive/<name> (host: build/archive-host/<name>). Logs:
# ~/wisp-build/logs/archive-<name>.log. WISP_ARCHIVE_JOBS (default and most 4): other builds run at the
# same time.
#
# The app links (by absolute archive paths, as app/CMakeLists.txt does with the sysroot's: the kit's
# sysroot has a libzstd.a of its own, which -lzstd would find first through the link line's -L order):
#   PKG_CONFIG_LIBDIR="$WB/archive/lib/pkgconfig:$PKG_CONFIG_LIBDIR" ps4-pkg-config --libs libarchive
# and puts -I$WB/archive/include ahead of the sysroots' include folders (the kit's has a zstd.h too).
#
# libarchive's choices for the PS4 (the CMake cache is preset: the OpenOrbis C library's ENOSYS stubs
# link, so the configure checks find them):
#  - no fstatat, unlinkat, linkat, readlinkat, fchdir, utimensat (ENOSYS stubs), futimens, lutimes,
#    futimesat, utime, lchmod (musl's, made on those stubs), statvfs/statfs (they fill in nothing):
#    archive_write_disk uses open/mkdir/lstat/unlink/rename, fchmod and futimes on descriptors,
#    utimes on names (libkernel's), and checks symlinks by whole-name lstat (patch below).
#  - no fork/vfork/posix_spawn: OpenOrbis' fork() is rfork(0), which returns 0 in the app itself (a
#    filter that needs a program: lzop, lrzip, grzip, then fails to start instead of ending the app).
#  - no issetugid: musl's is libc.secure, which is set on the PS4 (no proper auxv), and libarchive then
#    ignored TMPDIR. The 7-Zip, xar and ISO writers keep their data in a temporary file: the app sets
#    TMPDIR to a folder it can write (/tmp is not one).
#  - no ACLs, extended attributes or file flags (chflags), no libb2/nettle/mbedtls/lzo/pcre/libmd, no
#    bsdtar/bsdcpio/bsdcat/bsdunzip, no tests. iconv: the C library's (musl's: UTF-8/16/32, CP437,
#    CP850, CP866, CP125x, ISO-8859-x, KOI8, Shift_JIS, EUC-JP, GBK/GB18030, Big5, EUC-KR).
#  - archive_read_disk's own tree walk (archive_read_disk_open) needs fchdir or fstatat: not on the
#    PS4. The app walks folders itself and fills each entry with archive_read_disk_entry_from_file()
#    (lstat by name), as the API test below does.
#
# Using it in the app (the API test below does all of this):
#  - setlocale(LC_CTYPE, "C.UTF-8") (or uselocale on the archive thread) first: libarchive converts
#    names to the LC_CTYPE charset, and musl's "C" locale is ASCII (a 7z's non-ASCII names came out
#    as no name at all). Zip/RAR names without the UTF-8 flag are taken as they are; 7-Zip reads
#    them in the OEM code page: archive_read_set_options(a, "zip:hdrcharset=CP437,rar:hdrcharset=CP437").
#  - sets of volumes go to archive_read_open_filenames() in order (x.part1.rar ..., x.7z.001 ...,
#    x.z01 ... x.zip); archive_read_support_format_raw() after _all() opens plain .gz/.bz2/.xz/
#    .lzma/.zst/.lz4/.Z/.lz files (one entry: gzip's stored name, else "data"); raw with no filter
#    but "none" means no archive.
#  - extraction from the destination as the working directory needs the kit's path wraps (chdir,
#    open, stat... resolve relative names; the PS4's kernel refuses them), else make every name
#    absolute and leave out ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS. Copy the data even when an
#    entry's size is 0 or unset (raw entries have none).
#  - RAR (4 and 5) and 7z encryption: unsupported (archive_entry_is_encrypted tells); zip's
#    traditional and WinZip AES ones: archive_read_add_passphrase / _set_passphrase_callback.
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"

PREFIX="$WB/archive"
HPREFIX="$WB/host-archive"
S="$WISP_SRC"; B="$WB/build/archive"; HB="$WB/build/archive-host"; L="$WISP_LOGS"
JOBS="${WISP_ARCHIVE_JOBS:-4}"
[ "$JOBS" -le 4 ] 2>/dev/null || JOBS=4
SRC="$S/libarchive"
mkdir -p "$S" "$B" "$L"
# Ours first: the kit's sysroot has a libzstd.pc of its own (another build of zstd).
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PKG_CONFIG_LIBDIR"
# One section per function and per datum, as the media libraries: the app's --gc-sections drops the
# readers and writers it never reaches.
SECTION_FLAGS="-ffunction-sections -fdata-sections"

LA_VER=3.8.11
XZ_VER=5.8.4
ZSTD_VER=1.5.7
LZ4_VER=1.10.0
BZ2_VER=1.0.8
# file|url|sha256
SOURCES=(
    "libarchive-$LA_VER.tar.xz|https://www.libarchive.org/downloads/libarchive-$LA_VER.tar.xz|a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2"
    "xz-$XZ_VER.tar.gz|https://github.com/tukaani-project/xz/releases/download/v$XZ_VER/xz-$XZ_VER.tar.gz|0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9"
    "zstd-$ZSTD_VER.tar.gz|https://github.com/facebook/zstd/releases/download/v$ZSTD_VER/zstd-$ZSTD_VER.tar.gz|eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"
    "lz4-$LZ4_VER.tar.gz|https://github.com/lz4/lz4/releases/download/v$LZ4_VER/lz4-$LZ4_VER.tar.gz|537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b"
    "bzip2-$BZ2_VER.tar.gz|https://sourceware.org/pub/bzip2/bzip2-$BZ2_VER.tar.gz|ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269"
)
ZSTD_OPTIONS=(-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF -DZSTD_BUILD_SHARED=OFF
    -DZSTD_BUILD_STATIC=ON -DZSTD_MULTITHREAD_SUPPORT=ON -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_BUILD_DICTBUILDER=OFF)

# --- libarchive's options (PS4 and host builds alike) ---------------------------------------------
LA_OPTIONS=(-DBUILD_SHARED_LIBS=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF
    -DENABLE_TEST=OFF -DENABLE_INSTALL=ON -DENABLE_WERROR=OFF -DENABLE_COVERAGE=OFF
    -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ICONV=ON
    -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=ON -DENABLE_LZO=OFF
    -DENABLE_OPENSSL=ON -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_CNG=OFF -DENABLE_MD=OFF
    -DENABLE_LIBB2=OFF -DENABLE_LIBXML2=ON -DENABLE_EXPAT=OFF -DENABLE_BSDXML=OFF -DENABLE_WIN32_XMLLITE=OFF
    -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DPOSIX_REGEX_LIB=NONE -DENABLE_LIBGCC=OFF)
# The functions libarchive must not take on the PS4 (header comment). The host build takes the same
# choices, so the PC test runs the PS4's code paths.
LA_FUNCS=(
    # ENOSYS stubs in the OpenOrbis C library
    -DHAVE_FSTATAT=0 -DHAVE_UNLINKAT=0 -DHAVE_LINKAT=0 -DHAVE_READLINKAT=0 -DHAVE_FCHDIR=0
    -DHAVE_UTIMENSAT=0
    # musl's, made on utimensat or fchmodat (ENOSYS stubs)
    -DHAVE_FUTIMENS=0 -DHAVE_LUTIMES=0 -DHAVE_FUTIMESAT=0 -DHAVE_UTIME=0 -DHAVE_LCHMOD=0
    # ask the kernel, then fill in nothing
    -DHAVE_STATVFS=0 -DHAVE_FSTATVFS=0 -DHAVE_STATFS=0 -DHAVE_FSTATFS=0
    # no processes: fork() is rfork(0), which returns 0 in the app itself
    -DHAVE_FORK=0 -DHAVE_VFORK=0 -DHAVE_POSIX_SPAWN=0 -DHAVE_POSIX_SPAWNP=0
    # no file flags
    -DHAVE_CHFLAGS=0 -DHAVE_FCHFLAGS=0 -DHAVE_LCHFLAGS=0
    # musl's issetugid() is libc.secure, set on the PS4: TMPDIR was ignored
    -DHAVE_ISSETUGID=0)

# --- libarchive's source: the release plus the Wisp patch ----------------------------------------
#  archive_umask.c: the OpenOrbis C library's umask() is an ENOSYS stub returning (mode_t)-1; taken
#    as the umask it cleared every mode bit, file type included, so archive_write_disk created every
#    directory entry as an empty regular file. A result with bits outside 07777 now means 022.
#  archive_write_disk_posix.c (check_symlinks_fsobj): without the *at functions and without fchdir,
#    the SECURE_SYMLINKS / hardlink-target walk chdir()ed into each folder of the path and could not
#    come back (the working directory stayed in the last folder checked, and the entry was created
#    from there; it also opened "." first, a relative name the PS4's kernel refuses). It now lstat()s
#    each leading part of the path by its whole name and never changes the working directory.
#  CMakeLists.txt: no -Wl,--gc-sections in the link flags with the kit's toolchain, which runs ld.lld
#    itself: every configure check that links failed on it (and found nothing, not even stdio.h).
#  archive_read_disk_posix.c: its fchdir stand-in (for systems without one) under its own name: the
#    OpenOrbis <unistd.h> declares fchdir (the C library's ENOSYS stub), and the static one clashed.
#  archive_write_set_format_iso9660.c: uname() is an ENOSYS stub too (Wisp's app has a real one), and
#    the ISO writer copied the unset struct into the volume's system identifier: "Unknown" instead.
la_patch() {
    cat <<'LIBARCHIVE_WISP_PATCH'
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -144,6 +144,10 @@
     SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-dead_strip")
   ELSEIF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
     # SunOS linker doesn't support --gc-sections
+  ELSEIF(DEFINED OPENORBIS)
+    # Wisp (PS4): the OpenOrbis toolchain runs ld.lld itself, which knows
+    # --gc-sections but not the compiler driver's -Wl,: every configure check
+    # that links failed. (The sections come from CMAKE_C_FLAGS_RELEASE.)
   ELSE()
     # Place the functions and data into separate sections, allowing the linker
     # to garbage collect the unused ones.
--- a/libarchive/archive_read_disk_posix.c
+++ b/libarchive/archive_read_disk_posix.c
@@ -115,12 +115,16 @@
 /*
  * Stub function for platforms that do not support fchdir.  This is OK on
  * platforms with newer *at functions as fchdir isn't used there (e.g. WASI).
+ * (Wisp, PS4: under its own name, as the OpenOrbis <unistd.h> declares the
+ * C library's fchdir, which is an ENOSYS stub there.)
  */
-static int fchdir(int fd)
+static int la_fchdir(int fd)
 {
+	(void)fd; /* UNUSED */
 	errno = ENOSYS;
 	return -1;
 }
+#define fchdir(fd) la_fchdir(fd)
 #endif
 
 /*-
--- a/libarchive/archive_umask.c
+++ b/libarchive/archive_umask.c
@@ -96,6 +96,17 @@
 		return (smask);
 #endif
 
-	umask(mask = umask(0));
+	mask = umask(0);
+	/*
+	 * Wisp (PS4): the OpenOrbis C library's umask() is an ENOSYS stub that
+	 * changes nothing and returns (mode_t)-1. Taken as the mask, that
+	 * cleared every mode bit, file type bits included: archive_write_disk
+	 * created directory entries as empty regular files. A real umask has
+	 * no bits outside 07777, so anything else means there is none to
+	 * honour: the usual 022.
+	 */
+	if (mask & ~(mode_t)07777)
+		return ((mode_t)022);
+	umask(mask);
 	return (mask);
 }
--- a/libarchive/archive_write_disk_posix.c
+++ b/libarchive/archive_write_disk_posix.c
@@ -2823,6 +2823,22 @@
  * recent paths.
  */
 /*
+ * Wisp (PS4): without the *at functions and without fchdir(), the chdir()
+ * walk in check_symlinks_fsobj() has no way back to the directory it started
+ * in: it left the process in the last folder it checked, and the entry was
+ * then created relative to that folder. (The OpenOrbis C library has
+ * fstatat, unlinkat and fchdir only as ENOSYS stubs, and the PS4's kernel
+ * turns down the relative "." that walk opens first.) There each leading
+ * part of the path is checked by its whole name instead: lstat() of "a",
+ * "a/b", "a/b/c", ... from the current directory; nothing is opened and the
+ * current directory never changes.
+ */
+#if !(defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)) \
+    && !defined(HAVE_FCHDIR)
+#define CHECK_SYMLINKS_BY_PREFIX 1
+#endif
+
+/*
  * Checks the given path to see if any elements along it are symlinks.  Returns
  * ARCHIVE_OK if there are none, otherwise puts an error in errmsg.
  */
@@ -2847,7 +2863,9 @@
 	char c = '\0';
 	int r;
 	struct stat st;
+#ifndef CHECK_SYMLINKS_BY_PREFIX
 	int chdir_fd;
+#endif
 #if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
 	int fd;
 #endif
@@ -2871,6 +2889,7 @@
 	 *  c holds what used to be in *tail
 	 *  last is 1 if this is the last tail
 	 */
+#ifndef CHECK_SYMLINKS_BY_PREFIX
 	chdir_fd = la_opendirat(AT_FDCWD, ".");
 	__archive_ensure_cloexec_flag(chdir_fd);
 	if (chdir_fd < 0) {
@@ -2878,6 +2897,7 @@
 		    "Could not open ", path);
 		return (ARCHIVE_FATAL);
 	}
+#endif
 	head = path;
 	tail = path;
 	last = 0;
@@ -2947,6 +2967,8 @@
 					close(chdir_fd);
 					chdir_fd = fd;
 				}
+#elif defined(CHECK_SYMLINKS_BY_PREFIX)
+				r = 0;	/* (head stays the whole prefix) */
 #else
 				r = chdir(head);
 #endif
@@ -2957,8 +2979,10 @@
 					res = (ARCHIVE_FATAL);
 					break;
 				}
+#ifndef CHECK_SYMLINKS_BY_PREFIX
 				/* Our view is now from inside this dir: */
 				head = tail + 1;
+#endif
 			}
 		} else if (S_ISLNK(st.st_mode)) {
 			if (last && checking_linkname) {
@@ -3073,6 +3097,8 @@
 						close(chdir_fd);
 						chdir_fd = fd;
 					}
+#elif defined(CHECK_SYMLINKS_BY_PREFIX)
+					r = 0;	/* (head stays the prefix) */
 #else
 					r = chdir(head);
 #endif
@@ -3084,11 +3110,13 @@
 						res = (ARCHIVE_FATAL);
 						break;
 					}
+#ifndef CHECK_SYMLINKS_BY_PREFIX
 					/*
 					 * Our view is now from inside
 					 * this dir:
 					 */
 					head = tail + 1;
+#endif
 				} else {
 					tail[0] = c;
 					fsobj_error(a_eno, a_estr, ELOOP,
--- a/libarchive/archive_write_set_format_iso9660.c
+++ b/libarchive/archive_write_set_format_iso9660.c
@@ -2172,7 +2172,10 @@
 #if defined(HAVE_SYS_UTSNAME_H)
 	struct utsname u;
 
-	uname(&u);
+	/* (Wisp, PS4: uname() is an ENOSYS stub in the OpenOrbis C library,
+	 * and the unchecked call copied uninitialized stack into the ISO) */
+	if (uname(&u) != 0)
+		strcpy(u.sysname, "Unknown");
 	strncpy(system_id, u.sysname, size-1);
 	system_id[size-1] = '\0';
 #elif defined(_WIN32) && !defined(__CYGWIN__)
LIBARCHIVE_WISP_PATCH
}

# --- helpers --------------------------------------------------------------------------------------
fetch() {  # every tarball in SOURCES, downloaded when missing and checked
    local e name url sum
    for e in "${SOURCES[@]}"; do
        IFS='|' read -r name url sum <<<"$e"
        if [ ! -f "$S/$name" ]; then
            echo "-- download $url"
            curl -fL --retry 3 -sS -o "$S/.$name.part" "$url"
            mv "$S/.$name.part" "$S/$name"
        fi
        echo "$sum  $S/$name" | sha256sum -c --quiet - || { echo "bad checksum: $S/$name" >&2; return 1; }
    done
}
unpack() {  # unpack <tarball> <dir>: the source tree $S/<dir>, unpacked once
    [ -d "$S/$2" ] || { rm -rf "$S/.tmp.$2"; mkdir -p "$S/.tmp.$2"; tar xf "$S/$1" -C "$S/.tmp.$2";
        mv "$S/.tmp.$2/"* "$S/$2"; rmdir "$S/.tmp.$2"; }
}
source_tree() {  # libarchive's patched tree: unpacked again when the patch changes
    local stamp
    stamp=$( (echo "$LA_VER"; la_patch) | sha256sum | cut -d' ' -f1)
    [ "$(cat "$SRC/.wisp-patched" 2>/dev/null)" = "$stamp" ] && return 0
    rm -rf "$SRC" "$S/.tmp.libarchive"; mkdir -p "$S/.tmp.libarchive"
    tar xf "$S/libarchive-$LA_VER.tar.xz" -C "$S/.tmp.libarchive"
    mv "$S/.tmp.libarchive/libarchive-$LA_VER" "$SRC"; rmdir "$S/.tmp.libarchive"
    la_patch | patch -d "$SRC" -p1 --forward --no-backup-if-mismatch
    echo "$stamp" > "$SRC/.wisp-patched"
}
cm() {  # cm <name> <srcdir> [cmake options]: cmake cross build + install into $PREFIX
    local name=$1 src=$2; shift 2
    rm -rf "$B/$name"
    wisp_cmake -S "$src" -B "$B/$name" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX;$WISP_SYSROOT;$PS4_SYSROOT" \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG $SECTION_FLAGS" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG $SECTION_FLAGS" \
        "$@"
    ninja -C "$B/$name" -j"$JOBS"
    ninja -C "$B/$name" install
}
pcfile() {  # pcfile <prefix> <name> <version> <libs> <description>: a .pc for a library built by hand
    mkdir -p "$1/lib/pkgconfig"
    cat > "$1/lib/pkgconfig/$2.pc" <<EOF
prefix=$1
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: $2
Description: $5
Version: $3
Libs: -L\${libdir} $4
Cflags: -I\${includedir}
EOF
}
la_pcfile() {  # la_pcfile <prefix> <description>: libarchive.pc with its static link's dependencies
    # (CMake's lists them as absolute paths in Libs.private; this one names their .pc files)
    mkdir -p "$1/lib/pkgconfig"
    cat > "$1/lib/pkgconfig/libarchive.pc" <<EOF
prefix=$1
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libarchive
Description: $2
URL: https://libarchive.org
Version: $LA_VER
Requires.private: liblzma libzstd liblz4 bzip2 libxml-2.0 libcrypto zlib
Libs: -L\${libdir} -larchive
Cflags: -I\${includedir}
Cflags.private: -DLIBARCHIVE_STATIC
EOF
}
no_weak_refs() {  # no_weak_refs <archive>: no weak undefined symbols (an app would import them, and
                  # create-fself refuses imports no module has)
    local w
    w=$(llvm-nm-$CLANG_VER -u "$1" 2>/dev/null | awk '$1 == "w" || $1 == "v" { print $2 }' | sort -u | tr '\n' ' ')
    [ -z "$w" ] || { echo "$1: weak undefined symbols: $w" >&2; return 1; }
}
nopthread_pc() {  # the PS4's pthreads are libkernel's: no -pthread/-lpthread in our .pc files
    sed -i -E -e 's/(^|[[:space:]])-pthread\b//g' -e 's/(^|[[:space:]])-lpthread\b//g' "$@"
}
check_config() {  # check_config <config.h> [host]: what libarchive must have chosen (or not)
    local h=$1 m bad=0 linux=HAVE_LINUX_FIEMAP_H
    [ "${2:-}" = host ] && linux=""   # (Linux's FIEMAP sparse-file probe: the PC's only)
    for m in HAVE_ZLIB_H HAVE_LIBZ HAVE_BZLIB_H HAVE_LIBBZ2 HAVE_LZMA_H HAVE_LIBLZMA HAVE_ZSTD_H HAVE_LIBZSTD \
             HAVE_ZSTD_compressStream HAVE_LZ4_H HAVE_LIBLZ4 HAVE_LIBXML2 HAVE_LIBXML_XMLREADER_H \
             HAVE_LIBXML_XMLWRITER_H HAVE_OPENSSL_EVP_H HAVE_LIBCRYPTO HAVE_PKCS5_PBKDF2_HMAC_SHA1 \
             ARCHIVE_CRYPTO_SHA256_OPENSSL HAVE_ICONV HAVE_ICONV_H HAVE_LSTAT HAVE_FCHMOD HAVE_FUTIMES \
             HAVE_UTIMES HAVE_MKSTEMP; do
        grep -q "^#define $m 1" "$h" || { echo "config.h: $m missing" >&2; bad=1; }
    done
    for m in HAVE_FSTATAT HAVE_UNLINKAT HAVE_LINKAT HAVE_READLINKAT HAVE_FCHDIR HAVE_UTIMENSAT HAVE_FUTIMENS \
             HAVE_LUTIMES HAVE_FUTIMESAT HAVE_UTIME HAVE_LCHMOD HAVE_STATVFS HAVE_FSTATVFS HAVE_STATFS \
             HAVE_FSTATFS HAVE_FORK HAVE_VFORK HAVE_POSIX_SPAWN HAVE_POSIX_SPAWNP HAVE_CHFLAGS HAVE_FCHFLAGS \
             HAVE_LCHFLAGS HAVE_ISSETUGID HAVE_LIBB2 HAVE_LZO2 HAVE_LIBMBEDCRYPTO HAVE_LIBNETTLE HAVE_LIBMD \
             HAVE_PCREPOSIX_H HAVE_PCRE2POSIX_H $linux HAVE_LOCALE_CHARSET \
             ARCHIVE_ACL_DARWIN ARCHIVE_ACL_FREEBSD ARCHIVE_ACL_LIBACL ARCHIVE_ACL_LIBRICHACL \
             ARCHIVE_ACL_SUNOS ARCHIVE_XATTR_AIX ARCHIVE_XATTR_DARWIN ARCHIVE_XATTR_FREEBSD \
             ARCHIVE_XATTR_LINUX; do
        grep -q "^#define $m " "$h" && { echo "config.h: $m unexpected" >&2; bad=1; }
    done
    return $bad
}

# --- the PS4 libraries, in dependency order -------------------------------------------------------
b_bzip2() {  # seven C files and a header (bzip2's own build is a Makefile for the host)
    unpack bzip2-$BZ2_VER.tar.gz bzip2
    local o="$B/bzip2" f
    rm -rf "$o"; mkdir -p "$o" "$PREFIX/lib" "$PREFIX/include"
    for f in blocksort huffman crctable randtable compress decompress bzlib; do
        clang-$CLANG_VER $WISP_CFLAGS $SECTION_FLAGS -D_FILE_OFFSET_BITS=64 -Wall -c "$S/bzip2/$f.c" -o "$o/$f.o"
    done
    rm -f "$PREFIX/lib/libbz2.a"
    llvm-ar-$CLANG_VER rcs "$PREFIX/lib/libbz2.a" "$o"/*.o
    cp "$S/bzip2/bzlib.h" "$PREFIX/include/"
    pcfile "$PREFIX" bzip2 $BZ2_VER "-lbz2" "bzip2 compression library (Wisp, PS4: static)"
    no_weak_refs "$PREFIX/lib/libbz2.a"
}
b_xz() {  # liblzma only (posix threads: libarchive's xz/zip/7z writers take a threads option)
    unpack xz-$XZ_VER.tar.gz xz
    cm xz "$S/xz" -DBUILD_TESTING=OFF -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_DOXYGEN=OFF -DXZ_TOOL_XZ=OFF \
        -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF \
        -DXZ_THREADS=posix -DXZ_SANDBOX=no
    nopthread_pc "$PREFIX/lib/pkgconfig/liblzma.pc"
    no_weak_refs "$PREFIX/lib/liblzma.a"
}
b_zstd() {  # the library only, with multithreaded compression; without the pre-v0.8 legacy formats and
            # the dictionary builder (ZDICT_*: libarchive doesn't train dictionaries, and its cover.c
            # wants qsort_r with _GNU_SOURCE, which the OpenOrbis C library lacks). ZSTD_TRACE=0: its
            # tracing hooks are weak references, which an app left undefined: weak imports, which
            # create-fself refuses ("missing module for symbol").
    unpack zstd-$ZSTD_VER.tar.gz zstd
    cm zstd "$S/zstd/build/cmake" "${ZSTD_OPTIONS[@]}" \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG $SECTION_FLAGS -DZSTD_TRACE=0"
    nopthread_pc "$PREFIX/lib/pkgconfig/libzstd.pc"
    no_weak_refs "$PREFIX/lib/libzstd.a"
}
b_lz4() {
    unpack lz4-$LZ4_VER.tar.gz lz4
    cm lz4 "$S/lz4/build/cmake" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DBUILD_STATIC_LIBS=ON
    no_weak_refs "$PREFIX/lib/liblz4.a"
}
b_libarchive() {
    source_tree
    # find_package in module mode, with the paths given here: the OpenOrbis toolchain prefers config
    # packages, and the kit's OpenSSLConfig.cmake gives OPENSSL_LIBRARIES as imported targets, which
    # libarchive's crypto checks (old-style try_compile) cannot link: no hash support was found
    printf 'set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)\n' > "$B/libarchive-module-mode.cmake"
    cm libarchive "$SRC" "${LA_OPTIONS[@]}" "${LA_FUNCS[@]}" \
        -DCMAKE_PROJECT_INCLUDE="$B/libarchive-module-mode.cmake" \
        -DZLIB_INCLUDE_DIR="$PS4_SYSROOT/include" -DZLIB_LIBRARY="$PS4_SYSROOT/lib/libz.a" \
        -DBZIP2_INCLUDE_DIR="$PREFIX/include" -DBZIP2_LIBRARY_RELEASE="$PREFIX/lib/libbz2.a" \
        -DLIBLZMA_INCLUDE_DIR="$PREFIX/include" -DLIBLZMA_LIBRARY="$PREFIX/lib/liblzma.a" \
        -DLIBLZMA_LIBRARY_RELEASE="$PREFIX/lib/liblzma.a" \
        -DZSTD_INCLUDE_DIR="$PREFIX/include" -DZSTD_LIBRARY="$PREFIX/lib/libzstd.a" \
        -DLZ4_INCLUDE_DIR="$PREFIX/include" -DLZ4_LIBRARY="$PREFIX/lib/liblz4.a" \
        -DOPENSSL_ROOT_DIR="$PS4_SYSROOT" -DOPENSSL_USE_STATIC_LIBS=TRUE \
        -DOPENSSL_INCLUDE_DIR="$PS4_SYSROOT/include" -DOPENSSL_CRYPTO_LIBRARY="$PS4_SYSROOT/lib/libcrypto.a" \
        -DOPENSSL_SSL_LIBRARY="$PS4_SYSROOT/lib/libssl.a" \
        -DLIBXML2_INCLUDE_DIR="$WISP_SYSROOT/include/libxml2" -DLIBXML2_LIBRARY="$WISP_SYSROOT/lib/libxml2.a"
    check_config "$B/libarchive/config.h"
    no_weak_refs "$PREFIX/lib/libarchive.a"
    la_pcfile "$PREFIX" "Multi-format archive and compression library (Wisp, PS4: static; zlib, bzip2, liblzma, zstd, lz4, OpenSSL, libxml2, iconv)"
    # (no -I of the dependencies' folders ahead of ours: the kit's zstd.h must not be the one taken)
    if grep -q -- "-I$PS4_SYSROOT/include" "$B/libarchive/build.ninja"; then
        echo "!! the kit's include folder is an -I in libarchive's build (its zstd.h could win)" >&2
        return 1
    fi
    ls -l "$PREFIX/lib/"*.a
}

# --- the API test (linked for the PS4; built and run on the PC by `host`) --------------------------
api_test_source() {
    cat <<'ARCHIVE_API_TEST'
/*
 * Wisp: libarchive API test (scripts/build-archive.sh). For the PS4 it is only linked; on the PC:
 *   archive-api-test list [-p PASS] ARCHIVE [VOLUME...]
 *   archive-api-test extract [-p PASS] [-a] DIR ARCHIVE [VOLUME...]
 *   archive-api-test create [-p PASS] OUT FORMAT[:FILTER] OPTIONS PATH...
 * list and extract open ARCHIVE and the further volumes of a set, in order (x.part1.rar x.part2.rar
 * ..., x.7z.001 x.7z.002 ...), as one stream with archive_read_open_filenames(), with every format and
 * filter, and raw last (plain .gz/.bz2/.xz/.lzma/.zst/.lz4/.Z/.lz files hold no archive). extract writes
 * under DIR with archive_write_disk (TIME, SECURE_NODOTDOT, SECURE_SYMLINKS, SECURE_NOABSOLUTEPATHS)
 * from DIR as the working directory; with -a every name is made absolute here instead (the PS4's
 * kernel takes only absolute names: what an app without the kit's path wraps does) and absolute names
 * from the archive are refused here. create writes FORMAT (zip, 7zip, pax, ustar, gnutar, newc, odc,
 * iso9660, xar, raw) through FILTER (gzip, bzip2, xz, lzma, zstd, lz4, compress) from the PATHs on
 * disk, folders walked here (archive_read_disk_open's walk needs fchdir/fstatat: not on the PS4),
 * each entry from archive_read_disk_entry_from_file(); OPTIONS ("-" for none) go to
 * archive_write_set_options ("zip:encryption=aes256", "7zip:compression=ppmd", "xz:threads=2"), PASS is
 * the encryption passphrase. ARCHIVE_TEST_LOCALE (default C.UTF-8) is the LC_CTYPE locale.
 */
#include <archive.h>
#include <archive_entry.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static int problems;

/* r below ARCHIVE_OK: print the archive's message (a warning is not counted as a problem) */
static int check(struct archive *a, int r, const char *what)
{
    if (r < ARCHIVE_OK) {
        const char *kind = r == ARCHIVE_WARN ? "warning" : r == ARCHIVE_RETRY ? "retry"
            : r == ARCHIVE_FAILED ? "failed" : "FATAL";
        printf("  %s: %s: %s\n", what, kind, archive_error_string(a) ? archive_error_string(a) : "(no message)");
        if (r < ARCHIVE_WARN)
            problems++;
    }
    return r;
}

static struct archive *open_input(const char *pass, char **names, int count)
{
    struct archive *a = archive_read_new();
    const char **list = calloc((size_t)count + 1, sizeof(*list));
    int i, r;

    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);
    archive_read_support_format_raw(a); /* bids least: only when no archive format took the data */
    if (pass)
        archive_read_add_passphrase(a, pass);
    for (i = 0; i < count; i++)
        list[i] = names[i];
    r = archive_read_open_filenames(a, list, 64 * 1024);
    free(list);
    if (check(a, r, "open") < ARCHIVE_WARN) {
        archive_read_free(a);
        return NULL;
    }
    return a;
}

/* 0 when the input is no archive: raw (the format that takes any data) with no compression filter
 * (archive_filter_count counts "none" too) */
static int describe_input(struct archive *a)
{
    int i;
    printf("  format: %s, filters:", archive_format_name(a));
    for (i = 0; i < archive_filter_count(a); i++)
        printf(" %s", archive_filter_name(a, i));
    printf("\n");
    if (archive_format(a) == ARCHIVE_FORMAT_RAW && archive_filter_count(a) < 2) {
        printf("  not an archive (no format or compression recognized)\n");
        problems++;
        return 0;
    }
    return 1;
}

static void describe_entry(struct archive_entry *e, int64_t bytes, int show_bytes)
{
    const char *utf8 = archive_entry_pathname_utf8(e), *mbs = archive_entry_pathname(e);
    char type = '?';
    switch (archive_entry_filetype(e)) {
    case AE_IFREG: type = '-'; break;
    case AE_IFDIR: type = 'd'; break;
    case AE_IFLNK: type = 'l'; break;
    case AE_IFIFO: type = 'p'; break;
    case AE_IFCHR: type = 'c'; break;
    case AE_IFBLK: type = 'b'; break;
    }
    if (show_bytes || archive_entry_size_is_set(e))
        printf("  %c %04o %10jd", type, (unsigned)(archive_entry_perm(e) & 07777),
            (intmax_t)(show_bytes ? bytes : archive_entry_size(e)));
    else /* (raw entries: the size is known only once read) */
        printf("  %c %04o %10s", type, (unsigned)(archive_entry_perm(e) & 07777), "?");
    printf(" %s", utf8 ? utf8 : mbs ? mbs : "(no name)");
    if (mbs && (!utf8 || strcmp(mbs, utf8) != 0))
        printf("  [archive_entry_pathname: %s]", mbs);
    if (archive_entry_hardlink(e))
        printf("  (hardlink to %s)", archive_entry_hardlink(e));
    if (archive_entry_symlink(e))
        printf("  -> %s", archive_entry_symlink(e));
    if (archive_entry_is_encrypted(e))
        printf("  [encrypted]");
    printf("\n");
}

static int do_list(const char *pass, char **names, int count)
{
    struct archive *a = open_input(pass, names, count);
    struct archive_entry *e;
    int r, entries = 0;

    if (!a)
        return 1;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        const void *buffer;
        size_t size;
        la_int64_t offset, bytes = 0;

        check(a, r, "header");
        if (entries++ == 0 && !describe_input(a))
            break;
        /* the data is read through: checksums, decryption, the codec */
        while ((r = archive_read_data_block(a, &buffer, &size, &offset)) == ARCHIVE_OK)
            bytes += (la_int64_t)size;
        describe_entry(e, bytes, 1);
        if (r != ARCHIVE_EOF && check(a, r, "data") == ARCHIVE_FATAL)
            break;
    }
    if (r != ARCHIVE_EOF && r != ARCHIVE_OK && r != ARCHIVE_WARN)
        check(a, r, "next header");
    printf("  %d entries; encrypted entries: %d\n", entries, archive_read_has_encrypted_entries(a));
    archive_read_free(a);
    return problems ? 1 : 0;
}

static int copy_data(struct archive *in, struct archive *out)
{
    const void *buffer;
    size_t size;
    la_int64_t offset;
    int r;

    for (;;) {
        r = archive_read_data_block(in, &buffer, &size, &offset);
        if (r == ARCHIVE_EOF)
            return ARCHIVE_OK;
        if (check(in, r, "read data") < ARCHIVE_WARN)
            return r;
        r = (int)archive_write_data_block(out, buffer, size, offset);
        if (check(out, r, "write data") < ARCHIVE_WARN)
            return r;
    }
}

/* -a: name -> base/name; NULL when the name is absolute (refused, as SECURE_NOABSOLUTEPATHS does) */
static const char *under(const char *base, const char *name, char *out, size_t size)
{
    if (name[0] == '/')
        return NULL;
    snprintf(out, size, "%s/%s", base, name);
    return out;
}

static int do_extract(const char *pass, int absolute, const char *dir, char **names, int count)
{
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS;
    struct archive *a, *disk;
    struct archive_entry *e;
    char path[PATH_MAX], link[PATH_MAX];
    int r, entries = 0;

    if (absolute) {
        if (dir[0] != '/') {
            printf("  -a needs an absolute folder\n");
            return 1;
        }
    } else {
        flags |= ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS;
        if (chdir(dir) != 0) {
            printf("  chdir %s: %s\n", dir, strerror(errno));
            return 1;
        }
    }
    a = open_input(pass, names, count);
    if (!a)
        return 1;
    disk = archive_write_disk_new();
    archive_write_disk_set_options(disk, flags);
    /* (no archive_write_disk_set_standard_lookup: owners are not restored, and its getpwnam reads
     * /etc/passwd) */
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        check(a, r, "header");
        if (entries++ == 0 && !describe_input(a))
            break;
        describe_entry(e, 0, 0);
        if (absolute) {
            const char *name = archive_entry_pathname(e), *target = archive_entry_hardlink(e);
            if (!name || !under(dir, name, path, sizeof(path)) ||
                (target && !under(dir, target, link, sizeof(link)))) {
                printf("  refused: absolute name\n");
                problems++;
                continue;
            }
            archive_entry_copy_pathname(e, path);
            if (target)
                archive_entry_copy_hardlink(e, link);
        }
        r = check(disk, archive_write_header(disk, e), "write header");
        if (r >= ARCHIVE_WARN) /* (whatever the size says: a raw entry has none) */
            r = copy_data(a, disk);
        if (r == ARCHIVE_FATAL)
            break;
        check(disk, archive_write_finish_entry(disk), "finish entry");
    }
    if (r != ARCHIVE_EOF && r != ARCHIVE_OK && r != ARCHIVE_WARN && r != ARCHIVE_FAILED)
        check(a, r, "next header");
    check(disk, archive_write_close(disk), "close"); /* folders' times and modes are set here */
    archive_write_free(disk);
    archive_read_free(a);
    printf("  %d entries\n", entries);
    return problems ? 1 : 0;
}

static int add_path(struct archive *out, struct archive *disk, const char *path, const char *name)
{
    struct archive_entry *e = archive_entry_new();
    int r, isdir;

    archive_entry_copy_sourcepath(e, path); /* where it is read from */
    archive_entry_copy_pathname(e, name);   /* its name in the archive */
    r = check(disk, archive_read_disk_entry_from_file(disk, e, -1, NULL), path); /* lstat(path) */
    if (r < ARCHIVE_WARN) {
        archive_entry_free(e);
        return r;
    }
    isdir = archive_entry_filetype(e) == AE_IFDIR;
    r = check(out, archive_write_header(out, e), name);
    if (r >= ARCHIVE_WARN && archive_entry_filetype(e) == AE_IFREG && archive_entry_size(e) > 0) {
        size_t size = 256 * 1024;
        char *buffer = malloc(size);
        int fd = open(path, O_RDONLY);
        ssize_t n;
        if (fd < 0) {
            printf("  open %s: %s\n", path, strerror(errno));
            problems++;
        } else {
            while ((n = read(fd, buffer, size)) > 0)
                if (check(out, (int)archive_write_data(out, buffer, (size_t)n) < 0 ? ARCHIVE_FATAL : ARCHIVE_OK,
                        name) != ARCHIVE_OK)
                    break;
            close(fd);
        }
        free(buffer);
    }
    archive_entry_free(e);
    if (r == ARCHIVE_FATAL)
        return r;
    if (isdir) {
        DIR *d = opendir(path);
        struct dirent *de;
        char child[PATH_MAX], childname[PATH_MAX];
        if (!d) {
            printf("  opendir %s: %s\n", path, strerror(errno));
            problems++;
            return ARCHIVE_FAILED;
        }
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            snprintf(childname, sizeof(childname), "%s/%s", name, de->d_name);
            if (add_path(out, disk, child, childname) == ARCHIVE_FATAL)
                break;
        }
        closedir(d);
    }
    return ARCHIVE_OK;
}

static int do_create(const char *pass, const char *file, const char *spec, const char *options,
    char **paths, int count)
{
    struct archive *out = archive_write_new(), *disk = archive_read_disk_new();
    char format[64];
    const char *filter = strchr(spec, ':');
    int i;

    snprintf(format, sizeof(format), "%.*s", filter ? (int)(filter - spec) : (int)strlen(spec), spec);
    if (check(out, archive_write_set_format_by_name(out, format), "format") < ARCHIVE_WARN ||
        (filter && check(out, archive_write_add_filter_by_name(out, filter + 1), "filter") < ARCHIVE_WARN) ||
        (options && strcmp(options, "-") && check(out, archive_write_set_options(out, options), "options") < ARCHIVE_WARN) ||
        (pass && check(out, archive_write_set_passphrase(out, pass), "passphrase") < ARCHIVE_WARN) ||
        check(out, archive_write_open_filename(out, file), "open") < ARCHIVE_WARN) {
        archive_write_free(out);
        archive_read_free(disk);
        return 1;
    }
    archive_read_disk_set_symlink_physical(disk); /* (the PS4's file systems have no links anyway) */
    for (i = 0; i < count; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base && base[1] ? base + 1 : paths[i];
        if (add_path(out, disk, paths[i], base) == ARCHIVE_FATAL)
            break;
    }
    check(out, archive_write_close(out), "close");
    printf("  wrote %s: format %s, filters:", file, archive_format_name(out));
    for (i = 0; i < archive_filter_count(out); i++)
        printf(" %s", archive_filter_name(out, i));
    printf(", %jd bytes\n", (intmax_t)archive_filter_bytes(out, -1));
    archive_write_free(out);
    archive_read_free(disk);
    return problems ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *locale = getenv("ARCHIVE_TEST_LOCALE");
    const char *pass = NULL;
    int absolute = 0, i = 2;

    /* libarchive converts names to the LC_CTYPE charset: with musl's "C" locale (ASCII) every
     * non-ASCII name would fail to convert */
    if (!setlocale(LC_CTYPE, locale ? locale : "C.UTF-8"))
        printf("setlocale(%s) failed\n", locale ? locale : "C.UTF-8");
    printf("%s; LC_CTYPE %s (%s)\n", archive_version_details(), setlocale(LC_CTYPE, NULL), nl_langinfo(CODESET));
    if (argc < 3)
        goto usage;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            pass = argv[++i];
        else if (!strcmp(argv[i], "-a"))
            absolute = 1;
        else
            goto usage;
    }
    if (!strcmp(argv[1], "list") && i < argc)
        return do_list(pass, argv + i, argc - i);
    if (!strcmp(argv[1], "extract") && i + 1 < argc)
        return do_extract(pass, absolute, argv[i], argv + i + 1, argc - i - 1);
    if (!strcmp(argv[1], "create") && i + 3 < argc)
        return do_create(pass, argv[i], argv[i + 1], argv[i + 2], argv + i + 3, argc - i - 3);
usage:
    printf("usage: %s list|extract|create ... (see the source)\n", argv[0]);
    return 2;
}
ARCHIVE_API_TEST
}

b_linktest() {  # the API test linked for the PS4 with the app's link line (scripts/env.sh)
    local t="$B/linktest" f lib name
    local -a libs=()
    rm -rf "$t"; mkdir -p "$t"
    api_test_source > "$t/archive-api-test.c"
    clang-$CLANG_VER $WISP_CFLAGS -Wall -I"$PREFIX/include" -c "$t/archive-api-test.c" -o "$t/archive-api-test.o"
    # pkg-config's -l names as absolute archives: ours, then the sysroots' (as app/CMakeLists.txt)
    for f in $(ps4-pkg-config --libs libarchive); do
        case "$f" in
            -L*) continue ;;
            -l*) name=${f#-l}; lib=""
                 for d in "$PREFIX/lib" "$WISP_SYSROOT/lib" "$PS4_SYSROOT/lib"; do
                     [ -f "$d/lib$name.a" ] && { lib="$d/lib$name.a"; break; }
                 done
                 libs+=("${lib:-$f}") ;;
            *) libs+=("$f") ;;
        esac
    done
    echo "pkg-config --static --libs libarchive: $(ps4-pkg-config --libs libarchive)"
    echo "link: ${libs[*]}"
    clang-$CLANG_VER $WISP_CFLAGS "$t/archive-api-test.o" $WISP_LINK_PRE "${libs[@]}" $WISP_LINK_POST \
        -Wl,-Map,"$t/archive-api-test.map" -Wl,--why-extract="$t/why-extract.txt" -o "$t/archive-api-test.elf"
    echo "PS4 link test: $t/archive-api-test.elf"
    # no weak imports (create-fself refuses a symbol no module has: zstd's trace hooks were some)
    llvm-readelf-$CLANG_VER --dyn-syms "$t/archive-api-test.elf" > "$t/dynsyms.txt"
    if awk '$5 == "WEAK" && $7 == "UND" { print $8; bad = 1 } END { exit !bad }' "$t/dynsyms.txt"; then
        echo "weak imports in the link test (above)" >&2; return 1
    fi
    echo "imports: $(awk '$7 == "UND" && $8 != "" { n++ } END { print n + 0 }' "$t/dynsyms.txt") symbols, none weak"
    # the API the Files app needs (nm into a file: grep -q would end the pipe early, and pipefail
    # would take nm's SIGPIPE for a missing symbol)
    llvm-nm-$CLANG_VER --defined-only "$PREFIX/lib/libarchive.a" > "$t/libarchive.nm" 2>/dev/null
    for f in archive_read_new archive_read_support_format_rar5 archive_read_support_format_7zip \
             archive_write_set_format_zip archive_read_add_passphrase archive_write_set_passphrase \
             archive_read_open_filenames archive_write_disk_new archive_read_disk_entry_from_file; do
        grep -q " T $f\$" "$t/libarchive.nm" || { echo "libarchive.a: $f missing" >&2; return 1; }
    done
    echo "libarchive.a defines the API checked (archive_read_new ... archive_read_disk_entry_from_file)"
    # what the test took from the C library that is only an ENOSYS stub there (movl $78 to errno),
    # and which object asked for it (lld's --why-extract)
    echo "-- C library members linked that store ENOSYS (member: wanted by object (symbol)):"
    mkdir -p "$t/libc"
    grep -o 'libc\.a([^)]*)' "$t/archive-api-test.map" | sort -u | sed -E 's/libc\.a\((.*)\)/\1/' > "$t/libc-members.txt"
    while read -r f; do
        (cd "$t/libc" && llvm-ar-$CLANG_VER x "$OPENORBIS/lib/libc.a" "$f")
        llvm-objdump-$CLANG_VER -d --no-show-raw-insn "$t/libc/$f" > "$t/libc/$f.s"
        if grep -qE 'movl[[:space:]]+\$0x4e, ?\(%r' "$t/libc/$f.s"; then
            echo "   $f: $(awk -F'\t' -v m="libc.a($f)" 'index($2, m) { n = $1; sub(/.*\//, "", n); printf "%s (%s) ", n, $3 }' \
                "$t/why-extract.txt")"
        fi
    done < "$t/libc-members.txt"
    rm -rf "$t/libc"
    echo "($(wc -l < "$t/libc-members.txt") C library members linked in all)"
    ls -l "$t/archive-api-test.elf"
}

# --- host (Linux, for the API test on the PC) -------------------------------------------------------
hcm() {  # hcm <name> <srcdir> [cmake options]: host cmake build + install into $HPREFIX
    local name=$1 src=$2; shift 2
    rm -rf "$HB/$name"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
        CC=gcc CXX=g++ cmake -S "$src" -B "$HB/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$HPREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$HPREFIX" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
    ninja -C "$HB/$name" -j"$JOBS"
    ninja -C "$HB/$name" install
}
host_openssl() {  # OpenSSL for the host build: the system's, aria2's host build, or 3.5.0 built here
    OSSL=""
    if env -u PKG_CONFIG_LIBDIR pkg-config --exists 'openssl >= 3' && [ -f /usr/include/openssl/evp.h ]; then
        OSSL=system
    elif [ -f "$WB/host-aria2/openssl/lib/libcrypto.a" ]; then
        OSSL="$WB/host-aria2/openssl"
    else
        local tb="$S/openssl-3.5.0.tar.gz" ob="$HB/openssl"
        [ -f "$tb" ] || cp "$S/aria2-host/openssl-3.5.0.tar.gz" "$tb" 2>/dev/null ||
            curl -fL --retry 3 -sS -o "$tb" https://github.com/openssl/openssl/releases/download/openssl-3.5.0/openssl-3.5.0.tar.gz
        echo "344d0a79f1a9b08029b0744e2cc401a43f9c90acd1044d09a530b4885a8e9fc0  $tb" | sha256sum -c --quiet -
        rm -rf "$ob"; mkdir -p "$ob"; tar xf "$tb" -C "$ob" --strip-components=1
        (cd "$ob" && env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS CC=gcc \
            ./Configure linux-x86_64 no-shared no-module no-tests no-docs --prefix="$HPREFIX/openssl" \
            --libdir=lib --openssldir=/usr/lib/ssl && make -j"$JOBS" build_libs && make install_dev >/dev/null)
        OSSL="$HPREFIX/openssl"
    fi
}
b_host() {
    fetch
    source_tree
    rm -rf "$HPREFIX/include" "$HPREFIX/lib/lib"{archive,bz2,lzma,zstd,lz4}.a "$HPREFIX/lib/pkgconfig"
    mkdir -p "$HB" "$HPREFIX/lib" "$HPREFIX/include"
    # bzip2
    unpack bzip2-$BZ2_VER.tar.gz bzip2
    rm -rf "$HB/bzip2"; mkdir -p "$HB/bzip2"
    local f
    for f in blocksort huffman crctable randtable compress decompress bzlib; do
        gcc -O2 -fPIC -D_FILE_OFFSET_BITS=64 -c "$S/bzip2/$f.c" -o "$HB/bzip2/$f.o"
    done
    ar rcs "$HPREFIX/lib/libbz2.a" "$HB/bzip2"/*.o
    cp "$S/bzip2/bzlib.h" "$HPREFIX/include/"
    pcfile "$HPREFIX" bzip2 $BZ2_VER "-lbz2" "bzip2 compression library (Wisp's PC tests)"
    # xz, zstd, lz4 as for the PS4
    unpack xz-$XZ_VER.tar.gz xz
    hcm xz "$S/xz" -DBUILD_TESTING=OFF -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_DOXYGEN=OFF -DXZ_TOOL_XZ=OFF \
        -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_THREADS=posix
    unpack zstd-$ZSTD_VER.tar.gz zstd
    hcm zstd "$S/zstd/build/cmake" "${ZSTD_OPTIONS[@]}"
    unpack lz4-$LZ4_VER.tar.gz lz4
    hcm lz4 "$S/lz4/build/cmake" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DBUILD_STATIC_LIBS=ON
    # libarchive: the same options, function choices and patch as the PS4's; the system's zlib and
    # libxml2
    host_openssl
    local -a ossl=()
    [ "$OSSL" = system ] || ossl=(-DOPENSSL_ROOT_DIR="$OSSL" -DOPENSSL_USE_STATIC_LIBS=TRUE
        -DOPENSSL_INCLUDE_DIR="$OSSL/include" -DOPENSSL_CRYPTO_LIBRARY="$OSSL/lib/libcrypto.a"
        -DOPENSSL_SSL_LIBRARY="$OSSL/lib/libssl.a")
    hcm libarchive "$SRC" "${LA_OPTIONS[@]}" "${LA_FUNCS[@]}" "${ossl[@]}" \
        -DBZIP2_INCLUDE_DIR="$HPREFIX/include" -DBZIP2_LIBRARY_RELEASE="$HPREFIX/lib/libbz2.a" \
        -DLIBLZMA_INCLUDE_DIR="$HPREFIX/include" -DLIBLZMA_LIBRARY="$HPREFIX/lib/liblzma.a" \
        -DLIBLZMA_LIBRARY_RELEASE="$HPREFIX/lib/liblzma.a" \
        -DZSTD_INCLUDE_DIR="$HPREFIX/include" -DZSTD_LIBRARY="$HPREFIX/lib/libzstd.a" \
        -DLZ4_INCLUDE_DIR="$HPREFIX/include" -DLZ4_LIBRARY="$HPREFIX/lib/liblz4.a"
    check_config "$HB/libarchive/config.h" host
    # the API test, linked like the PS4's (static libarchive and codecs; the system's zlib, libxml2)
    local t="$HB/linktest" crypto
    rm -rf "$t"; mkdir -p "$t"
    api_test_source > "$t/archive-api-test.c"
    if [ "$OSSL" = system ]; then crypto=-lcrypto; else crypto="$OSSL/lib/libcrypto.a"; fi
    gcc -O1 -g -Wall -I"$HPREFIX/include" "$t/archive-api-test.c" -o "$t/archive-api-test" \
        "$HPREFIX/lib/libarchive.a" "$HPREFIX/lib/liblzma.a" "$HPREFIX/lib/libzstd.a" "$HPREFIX/lib/liblz4.a" \
        "$HPREFIX/lib/libbz2.a" -lxml2 $crypto -lz -lpthread -ldl
    host_tests "$t/archive-api-test"
}

# The PC test: archives made with the PC's own tools (zip, 7z, tar, bsdtar, xz, gzip, bzip2, ar) and
# libarchive's own test samples (rar, rar5, multi-volume sets, cab, lha, iso, encrypted ones), listed
# and extracted by the API test; archives the API test writes, checked with 7z, unzip and bsdtar.
host_tests() {
    local api=$1 T="$HB/t" n src
    [ -d "$T" ] && chmod -R u+rwx "$T"
    rm -rf "$T"; mkdir -p "$T/samples" "$T/out"
    export LC_ALL=C.UTF-8
    : > "$T/summary.txt"
    result() { printf '%-58s %s\n' "$1" "$2" | tee -a "$T/summary.txt"; }
    fresh_x() {  # an empty extraction folder (an ISO's folders come out read-only: Rock Ridge "useful")
        [ -d "$T/x" ] && chmod -R u+rwx "$T/x"
        rm -rf "$T/x"; mkdir -p "$T/x"
    }
    api() { echo "\$ archive-api-test $*"; "$api" "$@"; local rc=$?; echo "(exit $rc)"; return $rc; }
    tree() { (cd "$1" && find . -mindepth 1 -printf '%y %TY-%Tm-%Td %TH:%TM:%.2TS %s %p\n' | sort); }
    same() {  # same <test> <extracted folder>: files, contents, types, sizes, times as the source's
        if diff -r "$T/src" "$2" >"$T/diff.txt" 2>&1 && diff <(tree "$T/src") <(tree "$2") >>"$T/diff.txt"; then
            result "$1" "OK (same files, contents, times)"
        else
            result "$1" "DIFFERS"; head -n 20 "$T/diff.txt"
        fi
    }
    extract_same() {  # extract_same <test> [api options] <archive...>: extract, then compare
        local name=$1; shift
        fresh_x
        if api extract "$@" "$T/x" "${archives[@]}"; then same "$name" "$T/x"; else result "$name" "FAILED (exit $?)"; fi
    }
    listed() {  # listed <test> <expect: ok|fail> [api options] <archive...>
        local name=$1 want=$2 rc=0; shift 2
        api list "$@" > "$T/list.txt" 2>&1 || rc=$?
        cat "$T/list.txt"
        if [ $rc -eq 0 ] && [ "$want" = ok ]; then
            result "$name" "OK ($(grep -E '^  [0-9]+ entries' "$T/list.txt" | sed 's/^ *//'))"
        elif [ $rc -ne 0 ] && [ "$want" = fail ]; then
            result "$name" "refused as expected: $(grep -E ': (failed|FATAL|warning):|not an archive' "$T/list.txt" | head -n 1 | sed -E 's/^ *//' | cut -c1-90)"
        else
            result "$name" "UNEXPECTED (exit $rc)"
        fi
    }
    uudec() {  # uudec <sample> <out>: one of libarchive's uuencoded test archives
        python3 -I - "$SRC/libarchive/test/$1.uu" "$2" <<'PY'
import binascii, sys
out, inside = bytearray(), False
for line in open(sys.argv[1], 'rb'):
    if not inside:
        inside = line.startswith(b'begin ')
        continue
    if line.strip() == b'end':
        break
    if line.strip():
        n = (line[0] - 32) & 63
        out += binascii.a2b_uu(line[:1 + (n + 2) // 3 * 4] + b'\n')[:n]
open(sys.argv[2], 'wb').write(out)
PY
    }

    # The source tree: text, random binary, empty file, deep folders, a UTF-8 name, modes, fixed times
    src="$T/src"
    mkdir -p "$src/bin" "$src/deep/a/b/c" "$src/empty dir"
    printf 'Wisp archive test\n%.0s' $(seq 1 2000) > "$src/readme.txt"
    head -c 300000 /dev/urandom > "$src/bin/random.bin"
    : > "$src/empty.txt"
    echo "deep" > "$src/deep/a/b/c/file.txt"
    echo "unicode" > "$src/ünïcødé ✓ файл 文件.txt"
    chmod 0640 "$src/readme.txt"; chmod 0750 "$src/bin"
    find "$src" -mindepth 1 -exec touch -h -d '2001-02-03 04:05:06' {} +
    archives=()

    mk() {  # mk <file> <command...>: the PC's tool makes $T/<file> from inside the source folder
        rm -f "$T/$1"
        if (cd "$src" && "${@:2}") > "$T/mk.log" 2>&1 && [ -s "$T/$1" ]; then return 0; fi
        result "$1 (by $2)" "SKIPPED: the PC's tool did not make it ($(tail -n 1 "$T/mk.log" | cut -c1-50))"
        return 1
    }
    try() {  # try <test> <file> <command...>: make it, extract it, compare
        local name=$1 file=$2; shift 2
        mk "$file" "$@" || return 0
        archives=("$T/$file"); extract_same "$name"
    }

    echo "== read: archives made on the PC"
    try "zip (Info-ZIP, deflate)" info.zip zip -qr "$T/info.zip" .
    if mk aes.zip 7z a -bd -tzip -mem=AES256 -pSecret.123 "$T/aes.zip" .; then
        archives=("$T/aes.zip"); extract_same "zip AES-256 (7z), right passphrase" -p Secret.123
        listed "zip AES-256 (7z), no passphrase" fail "$T/aes.zip"
        listed "zip AES-256 (7z), wrong passphrase" fail -p wrong "$T/aes.zip"
    fi
    if mk zipcrypto.zip zip -qr -P Secret.123 "$T/zipcrypto.zip" .; then
        archives=("$T/zipcrypto.zip"); extract_same "zip traditional PKWARE encryption (zip -P)" -p Secret.123
    fi
    archives=("$T/info.zip"); extract_same "zip, names made absolute here (-a)" -a
    for m in LZMA2 LZMA PPMd BZip2 Deflate; do try "7z $m" "$m.7z" 7z a -bd -t7z -m0=$m "$T/$m.7z" .; done
    try "7z BCJ + LZMA2" bcj.7z 7z a -bd -t7z -mf=BCJ -m0=LZMA2 "$T/bcj.7z" .
    try "7z solid, -mx=9" solid.7z 7z a -bd -t7z -mx=9 -ms=on "$T/solid.7z" .
    mk enc.7z 7z a -bd -t7z -pSecret.123 "$T/enc.7z" . &&
        listed "7z AES-256 encrypted data (with the passphrase)" fail -p Secret.123 "$T/enc.7z"
    if mk split.7z.001 7z a -bd -t7z -v100k "$T/split.7z" .; then
        archives=("$T"/split.7z.0*); extract_same "7z split into ${#archives[@]} volumes (.7z.001...)"
    fi
    if mk span.zip zip -qr -s 100k "$T/span.zip" .; then
        archives=("$T"/span.z0* "$T/span.zip"); extract_same "zip split in ${#archives[@]} (zip -s: .z01 ... .zip)"
        archives=("$T/span.zip"); listed "zip split, the last part (.zip) alone" fail "$T/span.zip"
    fi
    try "tar.gz (GNU tar)" t.tar.gz tar czf "$T/t.tar.gz" .
    try ".tgz (GNU tar)" t.tgz tar czf "$T/t.tgz" .
    try "tar.bz2 (GNU tar)" t.tar.bz2 tar cjf "$T/t.tar.bz2" .
    try "tar.xz (GNU tar)" t.tar.xz tar cJf "$T/t.tar.xz" .
    try "tar.lzma (tar | xz --format=lzma)" t.tar.lzma bash -c 'tar cf - . | xz --format=lzma > "$0"' "$T/t.tar.lzma"
    try "tar.zst (bsdtar)" t.tar.zst bsdtar --zstd -cf "$T/t.tar.zst" .
    try "tar.lz4 (bsdtar)" t.tar.lz4 bsdtar --lz4 -cf "$T/t.tar.lz4" .
    try "tar.Z (bsdtar)" t.tar.Z bsdtar -Z -cf "$T/t.tar.Z" .
    try "tar.lz (bsdtar --lzip)" t.tar.lz bsdtar --lzip -cf "$T/t.tar.lz" .
    try "ustar (bsdtar)" t.ustar.tar bsdtar --format=ustar -cf "$T/t.ustar.tar" .
    try "pax (bsdtar)" t.pax.tar bsdtar --format=pax -cf "$T/t.pax.tar" .
    try "GNU tar format (GNU tar)" t.gnu.tar tar --format=gnu -cf "$T/t.gnu.tar" .
    try "cpio newc (bsdtar)" t.cpio bsdtar --format=newc -cf "$T/t.cpio" .
    try "iso9660 + Rock Ridge + Joliet (bsdtar)" t.iso bsdtar --format=iso9660 -cf "$T/t.iso" .
    try "xar (bsdtar)" t.xar bsdtar --format=xar -cf "$T/t.xar" .
    mk t.a ar rc "$T/t.a" readme.txt empty.txt bin/random.bin && listed "ar (binutils)" ok "$T/t.a"
    for f in gzip bzip2 xz; do "$f" -c "$src/readme.txt" > "$T/readme.txt.$f"; done
    xz --format=lzma -c "$src/readme.txt" > "$T/readme.txt.lzma"
    # (zstd and lz4 have no command-line tools here: the API test writes those below)
    for f in gzip bzip2 xz lzma; do
        fresh_x
        # one entry: the name gzip stored, else "data"
        if api extract "$T/x" "$T/readme.txt.$f" && [ "$(ls "$T/x" | wc -l)" = 1 ] &&
           cmp -s "$T/x/$(ls "$T/x")" "$src/readme.txt"; then
            result "plain .$f file (raw format: one entry, \"$(ls "$T/x")\")" "OK (same contents)"
        else result "plain .$f file (raw)" "FAILED"; fi
    done
    mk random.bin cp bin/random.bin "$T/random.bin" &&
        listed "a file that is no archive (raw, no filter)" fail "$T/random.bin"
    # the names as the C locale sees them (why the app sets a UTF-8 LC_CTYPE)
    echo "-- the 7z archive's names in the C locale:"
    ARCHIVE_TEST_LOCALE=C "$api" list "$T/LZMA2.7z" 2>&1 | grep -E 'nïc|\[archive_entry_pathname|warning' | head -n 4 || true

    echo "== read: libarchive's test samples"
    for n in test_read_format_rar.rar test_read_format_rar5_compressed.rar test_read_format_rar5_unicode.rar \
             test_read_format_rar5_solid.rar test_read_format_rar_unicode.rar test_read_format_7zip_ppmd.7z \
             test_read_format_7zip_bcj2_lzma2_1.7z test_read_format_7zip_delta_lzma2.7z \
             test_read_format_7zip_lzma2_arm64.7z test_read_format_zip_zip64a.zip test_read_format_cab_1.cab \
             test_read_format_cab_lzx_e8.cab test_read_format_lha_lh6.lzh test_read_format_lha_lh7.lzh \
             test_read_format_lha_header2.lzh test_read_format_iso_joliet_rockridge.iso.Z \
             test_read_format_iso_zisofs.iso.Z test_read_format_warc.warc test_read_format_ar.ar \
             test_read_format_cpio_bin_le.cpio test_compat_lzip_3.lz test_compat_lzma_1.tlz \
             test_compat_zstd_1.tar.zst test_compat_lz4_B7BD.tar.lz4 test_archive_string_conversion.txt.Z \
             test_compat_tar_hardlink_1.tar; do
        uudec "$n" "$T/samples/$n"; listed "$n" ok "$T/samples/$n"
    done
    uudec test_read_format_zip_winzip_aes256.zip "$T/samples/aes256.zip"
    listed "WinZip AES-256 sample, passphrase \"password\"" ok -p password "$T/samples/aes256.zip"
    uudec test_read_format_zip_traditional_encryption_data.zip "$T/samples/zipcrypto.zip"
    listed "traditional encryption sample, passphrase \"12345678\"" ok -p 12345678 "$T/samples/zipcrypto.zip"
    for n in 0001 0002 0003 0004; do uudec test_read_format_rar_multivolume.part$n.rar "$T/samples/mv.part$n.rar"; done
    listed "RAR4 multi-volume (.part0001-4.rar)" ok "$T"/samples/mv.part000?.rar
    for n in 01 02 03 04 05 06 07 08; do uudec test_read_format_rar5_multiarchive.part$n.rar "$T/samples/mv5.part$n.rar"; done
    listed "RAR5 multi-volume (.part01-08.rar)" ok "$T"/samples/mv5.part0?.rar
    for n in 1 2; do uudec test_read_format_cab_multi.p$n.cab "$T/samples/multi.p$n.cab"; done
    listed "CAB set over two cabinets (not supported)" fail "$T"/samples/multi.p?.cab
    for n in test_read_format_rar4_encrypted.rar test_read_format_rar5_encrypted.rar \
             test_read_format_7zip_encryption.7z test_read_filter_lzop.tar.lzo test_read_filter_lrzip.tar.lrz \
             test_read_filter_grzip.tar.grz; do
        uudec "$n" "$T/samples/$n"; listed "$n" fail -p password "$T/samples/$n"
    done

    echo "== write: archives made by the API test, checked with the PC's tools"
    rm -rf "$T/out"; mkdir -p "$T/out"
    w() {  # w <test> <file> <format[:filter]> <options> [-p pass] -- checked by the command after it
        local name=$1 file=$2 spec=$3 opts=$4; shift 4
        local -a p=(); [ "${1:-}" = -p ] && { p=(-p "$2"); shift 2; }
        if ! api create "${p[@]}" "$T/out/$file" "$spec" "$opts" "$src/readme.txt" "$src/empty.txt" "$src/bin" \
                "$src/deep" "$src/ünïcødé ✓ файл 文件.txt"; then
            result "write $name" "FAILED"; return
        fi
        if "$@" > "$T/check.txt" 2>&1; then result "write $name" "OK (checked: $1 $2)"
        else result "write $name" "CHECK FAILED ($*)"; cat "$T/check.txt"; fi
    }
    w "zip deflate" deflate.zip zip "zip:compression=deflate" unzip -tq "$T/out/deflate.zip"
    w "zip AES-256" aes.zip zip "zip:encryption=aes256" -p Secret.123 7z t -pSecret.123 "$T/out/aes.zip"
    w "zip AES-128" aes128.zip zip "zip:encryption=aes128" -p Secret.123 7z t -pSecret.123 "$T/out/aes128.zip"
    w "zip traditional encryption" zipcrypto.zip zip "zip:encryption=zipcrypt" -p Secret.123 \
        unzip -tq -P Secret.123 "$T/out/zipcrypto.zip"
    w "zip store" store.zip zip "zip:compression=store" unzip -tq "$T/out/store.zip"
    w "7z LZMA2" lzma2.7z 7zip "7zip:compression=lzma2" 7z t "$T/out/lzma2.7z"
    w "7z PPMd" ppmd.7z 7zip "7zip:compression=ppmd" 7z t "$T/out/ppmd.7z"
    w "7z deflate" deflate.7z 7zip "7zip:compression=deflate" 7z t "$T/out/deflate.7z"
    w "7z bzip2" bzip2.7z 7zip "7zip:compression=bzip2" 7z t "$T/out/bzip2.7z"
    w "tar.gz (pax)" t.tar.gz pax:gzip - tar tzf "$T/out/t.tar.gz"
    w "tar.bz2 (ustar)" t.tar.bz2 ustar:bzip2 - tar tjf "$T/out/t.tar.bz2"
    w "tar.xz (pax, 2 threads)" t.tar.xz pax:xz "xz:threads=2" tar tJf "$T/out/t.tar.xz"
    w "tar.zst (pax)" t.tar.zst pax:zstd - bsdtar tf "$T/out/t.tar.zst"
    w "tar.lz4 (gnutar)" t.tar.lz4 gnutar:lz4 - bsdtar tf "$T/out/t.tar.lz4"
    w "cpio newc" t.cpio newc - bsdtar tf "$T/out/t.cpio"
    w "iso9660" t.iso iso9660 - 7z t "$T/out/t.iso"
    w "xar" t.xar xar - bsdtar tf "$T/out/t.xar"
    # the non-ASCII name as 7-Zip sees it in our zip (UTF-8 flag) and 7z
    for f in deflate.zip lzma2.7z; do
        7z l "$T/out/$f" > "$T/7zl.txt" 2>&1 || true
        if grep -q "ünïcødé ✓ файл 文件.txt" "$T/7zl.txt"; then result "our $f: the UTF-8 name, as 7-Zip lists it" "OK"
        else result "our $f: the UTF-8 name, as 7-Zip lists it" "DIFFERS"; cat "$T/7zl.txt"; fi
    done
    # plain compressed files (the raw format: one entry, no archive)
    for f in zstd lz4 xz gzip bzip2; do
        if api create "$T/out/readme.txt.$f" "raw:$f" - "$src/readme.txt" > "$T/w.log" 2>&1; then
            fresh_x
            if api extract "$T/x" "$T/out/readme.txt.$f" > "$T/x.log" 2>&1 && [ "$(ls "$T/x" | wc -l)" = 1 ] &&
               cmp -s "$T/x/$(ls "$T/x")" "$src/readme.txt"; then
                result "write a plain .$f file (raw format), read back" "OK"
            else result "write a plain .$f file (raw format), read back" "FAILED"; cat "$T/x.log"; fi
        else result "write a plain .$f file (raw format)" "FAILED"; cat "$T/w.log"; fi
    done
    # what we wrote, read back by us
    for f in aes.zip zipcrypto.zip lzma2.7z ppmd.7z t.tar.xz t.tar.zst t.iso t.xar; do
        fresh_x
        if api extract -p Secret.123 "$T/x" "$T/out/$f" > "$T/x.log" 2>&1 &&
           cmp -s "$T/x/bin/random.bin" "$src/bin/random.bin" &&
           [ -f "$T/x/ünïcødé ✓ файл 文件.txt" ] && [ -d "$T/x/deep/a/b/c" ]; then
            result "our $f read back" "OK"
        else result "our $f read back" "FAILED"; cat "$T/x.log"; fi
    done

    echo "== extraction safety"
    rm -f /tmp/wisp-archive-abs.txt /tmp/wisp-archive-link.txt "$T/escape.txt"
    python3 -I - "$T/evil.tar" <<'PY'
import io, sys, tarfile
with tarfile.open(sys.argv[1], 'w', format=tarfile.GNU_FORMAT) as t:
    def add(name, data=b'x', **kw):
        i = tarfile.TarInfo(name); i.size = len(data)
        for k, v in kw.items(): setattr(i, k, v)
        t.addfile(i, io.BytesIO(data) if i.type == tarfile.REGTYPE else None)
    add('ok.txt')
    add('../escape.txt')
    add('/tmp/wisp-archive-abs.txt')
    add('dirlink', type=tarfile.SYMTYPE, linkname='/tmp')
    add('dirlink/wisp-archive-link.txt')
    add('sub/inner.txt')
PY
    fresh_x
    api extract "$T/x" "$T/evil.tar" || true
    if [ -f "$T/x/ok.txt" ] && [ -f "$T/x/sub/inner.txt" ] && [ ! -e "$T/escape.txt" ] &&
       [ ! -e /tmp/wisp-archive-abs.txt ] && [ ! -e /tmp/wisp-archive-link.txt ]; then
        result "../, absolute and through-a-symlink names refused" "OK (the others extracted)"
    else result "../, absolute and through-a-symlink names refused" "FAILED"; fi
    # The umask fix: libarchive with a umask() that fails as the OpenOrbis C library's does
    printf '#include <errno.h>\n#include <sys/types.h>\nmode_t umask(mode_t m) { (void)m; errno = 78; return (mode_t)-1; }\n' \
        > "$T/umask-stub.c"
    gcc -shared -fPIC -o "$T/umask-stub.so" "$T/umask-stub.c"
    fresh_x
    if LD_PRELOAD="$T/umask-stub.so" "$api" extract "$T/x" "$T/info.zip" > "$T/x.log" 2>&1 && [ -d "$T/x/deep/a/b/c" ] &&
       cmp -s "$T/x/bin/random.bin" "$src/bin/random.bin"; then
        result "with umask() failing as on the PS4" "OK (folders are folders: $(stat -c '%A' "$T/x/deep"))"
    else result "with umask() failing as on the PS4" "FAILED"; cat "$T/x.log"; fi
    echo "== summary"
    cat "$T/summary.txt"
}

# --- main -----------------------------------------------------------------------------------------
run_logged() {  # run_logged <log> <function>: its output into the log, the log's tail on failure
    local log=$1 rc; shift
    echo "== $1 (log: $log)"
    # (not inside an `if`: bash ignores set -e in a condition, subshells included)
    set +e
    ( set -euo pipefail; "$@" ) > "$log" 2>&1
    rc=$?
    set -e
    if [ $rc -ne 0 ]; then
        echo "!! $1 failed: $log"; tail -n 30 "$log"; exit 1
    fi
}
ALL=(bzip2 xz zstd lz4 libarchive linktest)
case "${1:-}" in
    fetch) run_logged "$L/archive-fetch.log" fetch; exit 0 ;;
    host) run_logged "$L/archive-host.log" b_host
          sed -n '/^== summary/,$p' "$L/archive-host.log"; exit 0 ;;
esac
run_logged "$L/archive-fetch.log" fetch
want=("$@"); [ ${#want[@]} -gt 0 ] || want=("${ALL[@]}")
for name in "${want[@]}"; do
    if [ $# -eq 0 ] && [ "$name" != linktest ] && [ -f "$B/$name.done" ]; then continue; fi
    run_logged "$L/archive-$name.log" "b_$name"
    [ "$name" = linktest ] || touch "$B/$name.done"
done
grep -E '^(link|pkg-config|PS4 link test|-- C library|   )' "$L/archive-linktest.log" 2>/dev/null || true
echo "archive libraries for the PS4: $PREFIX"
