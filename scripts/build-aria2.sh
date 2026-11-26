#!/usr/bin/env bash
# Wisp: aria2 1.37.0 (the download engine behind Motrix) as a static C++ library, libaria2, for the
# UI process's download manager: HTTP/HTTPS/FTP, BitTorrent and magnet links (DHT), Metalink,
# segmented multi-connection downloads, pause/resume. Its own prefix, ~/wisp-build/aria2:
# include/aria2/aria2.h, lib/libaria2.a, lib/pkgconfig/libaria2.pc. Nothing in the sysroot changes.
#
#   bash ~/wisp/scripts/build-aria2.sh         # the PS4 library, then a PS4 link test of its API
#   bash ~/wisp/scripts/build-aria2.sh host    # aria2 for Linux in ~/wisp-build/host-aria2 and the
#                                              # same API test run on the PC (downloads a file)
#
# Source: ~/wisp-build/src/aria2-1.37.0.tar.xz (downloaded if missing) -> ~/wisp-build/src/aria2,
# patched (ps4_patch below). Build: ~/wisp-build/build/aria2 (host: aria2-host, aria2-host-openssl).
# Logs: ~/wisp-build/logs/aria2.log (host: aria2-host.log).
#
# Backends: OpenSSL 3 (the kit's) for TLS, the digests, DH bignums and ARC4 (BitTorrent's MSE);
# zlib; expat (Metalink, XML-RPC); kqueue event polling (poll is built too: event-poll=poll). Without
# GnuTLS/nettle/gmp/libgcrypt, libssh2 (sftp://), c-ares (names resolve through getaddrinfo, blocking,
# in the thread that calls aria2::run), sqlite3 (browser cookie databases), WebSocket, NLS. The CA
# bundle defaults to /app0/wisp/cacert.pem (app/package.sh puts it there).
#
# The app links with
#   PKG_CONFIG_LIBDIR="$WB/aria2/lib/pkgconfig:$PKG_CONFIG_LIBDIR" pkg-config --static --libs libaria2
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"

VER=1.37.0
TARBALL=aria2-$VER.tar.xz
SHA256=60a420ad7085eb616cb6e2bdf0a7206d68ff3d37fb5a956dc44242eb2f79b66b
URL=https://github.com/aria2/aria2/releases/download/release-$VER/$TARBALL
S="$WISP_SRC"; SRC="$S/aria2"
B="$WB/build/aria2"; PREFIX="$WB/aria2"
HB="$WB/build/aria2-host"; HPREFIX="$WB/host-aria2"
L="$WISP_LOGS"
JOBS=$(( WISP_JOBS < 6 ? WISP_JOBS : 6 ))   # (other builds may run at the same time)
HOST_TRIPLE=x86_64-pc-freebsd12
CA_BUNDLE=/app0/wisp/cacert.pem
# Host OpenSSL when the PC has no headers for its own: the kit's version, static, providers built in.
OSSL_TARBALL=openssl-3.5.0.tar.gz
OSSL_SHA256=344d0a79f1a9b08029b0744e2cc401a43f9c90acd1044d09a530b4885a8e9fc0
OSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-3.5.0/$OSSL_TARBALL
mkdir -p "$S" "$L"

# configure options both builds share
COMMON=(--enable-libaria2 --disable-shared --enable-static --disable-nls --disable-rpath
    --enable-ssl --enable-bittorrent --enable-metalink --disable-websocket
    --with-openssl --without-gnutls --without-libnettle --without-libgmp --without-libgcrypt
    --without-appletls --without-wintls
    --with-libz --with-libexpat --without-libxml2 --without-sqlite3 --without-libssh2 --without-libcares
    --without-libuv --without-tcmalloc --without-jemalloc)

# --- source ---------------------------------------------------------------------------------------
# The source changes (the PS4 ones only under __PS4__):
#  SocketCore.cc: FreeBSD's numbers for the IPPROTO_IP/IPPROTO_IPV6 options aria2 sets (local peer
#    discovery's multicast, --dscp); OpenOrbis' <netinet/in.h> has Linux's. (aria2 passes no MSG_*
#    flags at all: every send/recv/sendto/recvfrom gets 0, non-blocking mode is fcntl's O_NONBLOCK.)
#  SimpleRandomizer.cc: no std::random_device, whose static initializer opens /dev/urandom before
#    main and keeps it open; the generator is seeded from OpenSSL's RAND_bytes.
#  util.cc: the on-download-* hooks start no program. OpenOrbis' fork() is rfork(0), which returns
#    0 without a new process: aria2 took the app for the child, and its _exit(1) would end the app.
#  DHTMessageFactoryImpl.cc (any platform): get_peers replies without a token are kept for their
#    nodes. The bootstrap nodes send none, so with an empty dht.dat (a new console) the DHT never
#    got past its entry point and trackerless magnet links found no peers (seen on the PC).
#  ConsoleStatCalc.cc (any platform): the terminal width only where TIOCGWINSZ exists (OpenOrbis'
#    headers have only LINUX_TIOCGWINSZ; the console readout is aria2c's, quiet=true in the app).
#  configure (any platform): the fallocate check links its test instead of only compiling it, so
#    the poison library finds Linux's fallocate missing (else HAVE_FALLOCATE: file-allocation=falloc
#    calls it). (m4/fallocate.m4 has the same check; left alone: a newer .m4 sets off automake's
#    rebuild rules.)
ps4_patch() {
    cat <<'ARIA2_PS4_PATCH'
--- a/src/SocketCore.cc
+++ b/src/SocketCore.cc
@@ -66,6 +66,27 @@
 #  include "SSHSession.h"
 #endif // HAVE_LIBSSH2

+#ifdef __PS4__
+// Wisp (PS4): the OpenOrbis <netinet/in.h> gives these IPPROTO_IP and
+// IPPROTO_IPV6 options Linux's numbers, but the PS4's kernel is FreeBSD 9's:
+// there Linux's IP_TOS (1) is IP_OPTIONS, 32-35 are no options, and
+// IPV6_TCLASS is 61, not 67. (Local peer discovery's multicast and --dscp use
+// them.) The SOL_SOCKET options and MSG_* flags come from the kit's
+// libc-overlay <sys/socket.h>, which has FreeBSD's.
+#  undef IP_TOS
+#  define IP_TOS 3
+#  undef IP_MULTICAST_IF
+#  define IP_MULTICAST_IF 9
+#  undef IP_MULTICAST_TTL
+#  define IP_MULTICAST_TTL 10
+#  undef IP_MULTICAST_LOOP
+#  define IP_MULTICAST_LOOP 11
+#  undef IP_ADD_MEMBERSHIP
+#  define IP_ADD_MEMBERSHIP 12
+#  undef IPV6_TCLASS
+#  define IPV6_TCLASS 61
+#endif // __PS4__
+
 namespace aria2 {

 #ifndef __MINGW32__
--- a/src/SimpleRandomizer.cc
+++ b/src/SimpleRandomizer.cc
@@ -70,9 +70,27 @@
   return randomizer_;
 }

+#if defined(__PS4__) && defined(HAVE_OPENSSL)
+// Wisp (PS4): no std::random_device here. libc++'s opens /dev/urandom from
+// this file's static initializer (before main: a failure there ends the app
+// with no log) and keeps that descriptor open for good. The generator's seed
+// comes from OpenSSL's DRBG instead, as getRandomBytes() does already.
+namespace {
+std::mt19937::result_type seedFromOpenSSL()
+{
+  std::mt19937::result_type seed = 0;
+  if (RAND_bytes(reinterpret_cast<unsigned char*>(&seed), sizeof(seed)) != 1) {
+    seed = static_cast<std::mt19937::result_type>(time(nullptr)) ^
+           static_cast<std::mt19937::result_type>(getpid());
+  }
+  return seed;
+}
+} // namespace
+#else  // !(__PS4__ && HAVE_OPENSSL)
 namespace {
 std::random_device rd;
 } // namespace
+#endif // !(__PS4__ && HAVE_OPENSSL)

 #ifdef __MINGW32__
 SimpleRandomizer::SimpleRandomizer()
@@ -81,6 +99,8 @@
                                  CRYPT_VERIFYCONTEXT | CRYPT_SILENT);
   assert(r);
 }
+#elif defined(__PS4__) && defined(HAVE_OPENSSL)
+SimpleRandomizer::SimpleRandomizer() : gen_(seedFromOpenSSL()) {}
 #else  // !__MINGW32__
 SimpleRandomizer::SimpleRandomizer() : gen_(rd()) {}
 #endif // !__MINGW32__
--- a/src/ConsoleStatCalc.cc
+++ b/src/ConsoleStatCalc.cc
@@ -302,7 +302,8 @@

   if (isTTY_) {
 #ifndef __MINGW32__
-#  ifdef HAVE_TERMIOS_H
+    // (Wisp, PS4: the OpenOrbis headers have no TIOCGWINSZ; no terminal there)
+#  if defined(HAVE_TERMIOS_H) && defined(TIOCGWINSZ)
     struct winsize size;
     if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
       cols = std::max(0, (int)size.ws_col - 1);
--- a/configure
+++ b/configure
@@ -27962,13 +27962,13 @@
   return 0;
 }
 _ACEOF
-if ac_fn_cxx_try_compile "$LINENO"
+if ac_fn_cxx_try_link "$LINENO"
 then :
   have_fallocate=yes
 else $as_nop
   have_fallocate=no
 fi
-rm -f core conftest.err conftest.$ac_objext conftest.beam conftest.$ac_ext
+rm -f core conftest.err conftest.$ac_objext conftest.beam conftest$ac_exeext conftest.$ac_ext
 { printf "%s\n" "$as_me:${as_lineno-$LINENO}: result: $have_fallocate" >&5
 printf "%s\n" "$have_fallocate" >&6; }
 if test "x$have_fallocate" = "xyes"; then
--- a/src/util.cc
+++ b/src/util.cc
@@ -2234,7 +2234,17 @@
 {
   const std::string gidStr = GroupId::toHex(gid);
   const std::string numFilesStr = util::uitos(numFiles);
-#ifndef __MINGW32__
+#ifdef __PS4__
+  // Wisp (PS4): no programs to start. The OpenOrbis fork() is rfork(0),
+  // which makes no new process and returns 0, so the "child" below would be
+  // the app itself: its execlp fails and _exit(EXIT_FAILURE) ends the app.
+  // The on-download-* / on-bt-download-complete hooks do nothing here.
+  A2_LOG_WARN(fmt("Not executing user command %s %s %s %s: the PS4 runs no"
+                  " other programs",
+                  command.c_str(), gidStr.c_str(), numFilesStr.c_str(),
+                  firstFilename.c_str()));
+  return;
+#elif !defined(__MINGW32__)
   A2_LOG_INFO(fmt("Executing user command: %s %s %s %s", command.c_str(),
                   gidStr.c_str(), numFilesStr.c_str(), firstFilename.c_str()));
   pid_t cpid = fork();
--- a/src/DHTMessageFactoryImpl.cc
+++ b/src/DHTMessageFactoryImpl.cc
@@ -445,9 +445,18 @@
       }
     }
   }
-  const String* token = getString(rDict, DHTGetPeersReplyMessage::TOKEN);
+  // Wisp: a reply without a token is still taken, for its nodes. DHT
+  // bootstrap nodes (dht.transmissionbt.com and dht.libtorrent.org, tested)
+  // answer get_peers without one, and aria2 threw those answers away: with
+  // an empty dht.dat the lookup never got past the entry point, and magnet
+  // links without trackers found no peers. (The token is only for announcing
+  // to that node; DHTPeerLookupTask::onFinish skips nodes whose token is
+  // empty.)
+  const String* token =
+      downcast<String>(rDict->get(DHTGetPeersReplyMessage::TOKEN));
   return createGetPeersReplyMessage(remoteNode, std::move(nodes),
-                                    std::move(peers), token->s(),
+                                    std::move(peers),
+                                    token ? token->s() : std::string(),
                                     transactionID);
 }
ARIA2_PS4_PATCH
}

fetch() {  # fetch <tarball> <sha256> <url> [local copies...]: $S-side tarball, checked
    local file=$1 sum=$2 url=$3 c; shift 3
    if [ ! -f "$file" ]; then
        for c in "$@"; do [ -f "$c" ] && { cp "$c" "$file.part"; break; }; done
        [ -f "$file.part" ] || curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c --quiet - || { echo "checksum mismatch: $file" >&2; return 1; }
}
source_tree() {  # the patched source tree: unpacked again when the patch changes (builds are separate)
    local stamp
    fetch "$S/$TARBALL" "$SHA256" "$URL"
    stamp=$(ps4_patch | sha256sum | cut -d' ' -f1)
    [ "$(cat "$SRC/.wisp-patched" 2>/dev/null)" = "$stamp" ] && return 0
    rm -rf "$SRC" "$S/.tmp.aria2"; mkdir -p "$S/.tmp.aria2"
    tar xf "$S/$TARBALL" -C "$S/.tmp.aria2"
    mv "$S/.tmp.aria2/aria2-$VER" "$SRC"; rmdir "$S/.tmp.aria2"
    ps4_patch | patch -d "$SRC" -p1 --forward --no-backup-if-mismatch
    echo "$stamp" > "$SRC/.wisp-patched"
}

write_pc() {  # write_pc <prefix> <description>: libaria2.pc with the static link's dependencies
    # (aria2's own .pc lists only -laria2)
    mkdir -p "$1/lib/pkgconfig"
    cat > "$1/lib/pkgconfig/libaria2.pc" <<EOF
prefix=$1
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: aria2
Description: $2
URL: https://aria2.github.io/
Version: $VER
Requires.private: libssl libcrypto zlib expat
Libs: -L\${libdir} -laria2
Cflags: -I\${includedir}
EOF
}

install_lib() {  # install_lib <build dir> <prefix>: only the library, its header and .pc
    make -j"$JOBS" -C "$1/src" libaria2.la
    make -C "$1/src" install-libLTLIBRARIES
    make -C "$1/src/includes" install
    rm -f "$2/lib/libaria2.la"
}

check_config() {  # check_config <config.h>: the backends configure must have chosen (or not)
    local h=$1 m bad=0
    for m in ENABLE_SSL HAVE_OPENSSL USE_OPENSSL_MD HAVE_EVP_DIGESTINIT_EX HAVE_EVP_SHA256 HAVE_ZLIB \
             HAVE_LIBEXPAT ENABLE_METALINK ENABLE_XML_RPC ENABLE_BITTORRENT HAVE_POLL HAVE_GETADDRINFO; do
        grep -q "^#define $m 1" "$h" || { echo "config.h: $m missing" >&2; bad=1; }
    done
    for m in HAVE_OLD_OPENSSL HAVE_LIBGNUTLS HAVE_LIBNETTLE HAVE_LIBGMP HAVE_LIBGCRYPT HAVE_LIBXML2 \
             HAVE_SQLITE3 HAVE_LIBSSH2 HAVE_LIBCARES ENABLE_ASYNC_DNS ENABLE_WEBSOCKET ENABLE_NLS; do
        grep -q "^#define $m " "$h" && { echo "config.h: $m unexpected" >&2; bad=1; }
    done
    return $bad
}
check_config_ps4() {  # and what the PS4 must not get: Linux-only calls, ENOSYS stubs
    local h=$1 m bad=0
    grep -q "^#define HAVE_KQUEUE 1" "$h" || { echo "config.h: HAVE_KQUEUE missing" >&2; bad=1; }
    for m in HAVE_EPOLL HAVE_FALLOCATE HAVE_POSIX_FALLOCATE HAVE_SOME_FALLOCATE HAVE_POSIX_FADVISE \
             HAVE_GETIFADDRS; do
        grep -q "^#define $m " "$h" && { echo "config.h: $m unexpected" >&2; bad=1; }
    done
    return $bad
}

api_test_source() {  # the API test both builds compile (the PS4 one is only linked)
    cat <<'ARIA2_API_TEST'
// Wisp: libaria2 API test (scripts/build-aria2.sh). For the PS4 it is only linked; on the PC
//   aria2-api-test <uri> <dir> --wait [option=value ...]
// downloads uri into dir (the options are added to the session's) and prints what the API reports.
#include <aria2/aria2.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char* eventName(aria2::DownloadEvent event)
{
    switch (event) {
    case aria2::EVENT_ON_DOWNLOAD_START: return "start";
    case aria2::EVENT_ON_DOWNLOAD_PAUSE: return "pause";
    case aria2::EVENT_ON_DOWNLOAD_STOP: return "stop";
    case aria2::EVENT_ON_DOWNLOAD_COMPLETE: return "complete";
    case aria2::EVENT_ON_DOWNLOAD_ERROR: return "error";
    case aria2::EVENT_ON_BT_DOWNLOAD_COMPLETE: return "bt-complete";
    }
    return "?";
}

const char* statusName(aria2::DownloadStatus status)
{
    switch (status) {
    case aria2::DOWNLOAD_ACTIVE: return "active";
    case aria2::DOWNLOAD_WAITING: return "waiting";
    case aria2::DOWNLOAD_PAUSED: return "paused";
    case aria2::DOWNLOAD_COMPLETE: return "complete";
    case aria2::DOWNLOAD_ERROR: return "error";
    case aria2::DOWNLOAD_REMOVED: return "removed";
    }
    return "?";
}

int onEvent(aria2::Session*, aria2::DownloadEvent event, aria2::A2Gid gid, void*)
{
    std::printf("event %s, gid %s\n", eventName(event), aria2::gidToHex(gid).c_str());
    return 0;
}

void report(aria2::Session* session, aria2::A2Gid gid)
{
    aria2::DownloadHandle* dh = aria2::getDownloadHandle(session, gid);
    if (!dh) {
        std::printf("no download handle for gid %s\n", aria2::gidToHex(gid).c_str());
        return;
    }
    std::printf("status %s, %lld of %lld bytes, %d B/s, %d connections, error code %d\n",
                statusName(dh->getStatus()), (long long)dh->getCompletedLength(),
                (long long)dh->getTotalLength(), dh->getDownloadSpeed(), dh->getConnections(),
                dh->getErrorCode());
    for (const aria2::FileData& file : dh->getFiles())
        std::printf("  file %d: %s, %lld bytes\n", file.index, file.path.c_str(), (long long)file.length);
    aria2::deleteDownloadHandle(dh);
}

} // namespace

int main(int argc, char** argv)
{
    const char* uri = argc > 1 ? argv[1] : "https://ftp.gnu.org/gnu/hello/hello-2.12.3.tar.gz";
    const char* dir = argc > 2 ? argv[2] : "/data/wisp/downloads";
    const bool wait = argc > 3 && std::strcmp(argv[3], "--wait") == 0;

    if (aria2::libraryInit() != 0) {
        std::puts("libraryInit failed");
        return 1;
    }
    aria2::KeyVals options = {
        {"dir", dir},
        {"file-allocation", "none"},
        {"max-connection-per-server", "8"},
        {"split", "8"},
        {"enable-dht", "true"},
        {"seed-time", "0"},
    };
    for (int i = 4; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        if (eq)
            options.emplace_back(std::string(argv[i], eq - argv[i]), std::string(eq + 1));
    }
    aria2::SessionConfig config;
    config.useSignalHandler = false; // the app's signal handlers stay the app's
    config.downloadEventCallback = onEvent;
    aria2::Session* session = aria2::sessionNew(options, config);
    if (!session) {
        std::puts("sessionNew failed");
        aria2::libraryDeinit();
        return 1;
    }

    aria2::A2Gid gid = 0;
    int rv = aria2::addUri(session, &gid, {uri}, aria2::KeyVals());
    std::printf("addUri %s: %d, gid %s\n", uri, rv, aria2::gidToHex(gid).c_str());
    rv = aria2::run(session, aria2::RUN_ONCE);
    std::printf("run(RUN_ONCE): %d\n", rv);
    if (wait) {
        const auto start = std::chrono::steady_clock::now();
        auto shown = start;
        while (rv == 1 && std::chrono::steady_clock::now() - start < std::chrono::minutes(2)) {
            rv = aria2::run(session, aria2::RUN_ONCE);
            const auto now = std::chrono::steady_clock::now();
            if (rv == 1 && now - shown >= std::chrono::seconds(1)) {
                report(session, gid);
                shown = now;
            }
        }
        std::printf("run(RUN_ONCE) loop: %d\n", rv);
        if (rv == 1) {
            // the clean stop: shutdown, then run until it returns 0 (.aria2 files, dht.dat saved)
            aria2::shutdown(session, false);
            const auto stop = std::chrono::steady_clock::now();
            while (rv == 1 && std::chrono::steady_clock::now() - stop < std::chrono::seconds(30))
                rv = aria2::run(session, aria2::RUN_ONCE);
            std::printf("shutdown, run(RUN_ONCE) loop: %d\n", rv);
        }
    }
    report(session, gid);
    aria2::GlobalStat stat = aria2::getGlobalStat(session);
    std::printf("global: %d active, %d waiting, %d stopped\n", stat.numActive, stat.numWaiting,
                stat.numStopped);
    const int final = aria2::sessionFinal(session);
    std::printf("sessionFinal: %d\n", final);
    aria2::libraryDeinit();
    return final;
}
ARIA2_API_TEST
}

# --- PS4 ------------------------------------------------------------------------------------------
b_ps4() {
    source_tree
    rm -rf "$B" "$PREFIX"; mkdir -p "$B"; cd "$B"
    # the ac helper's environment (scripts/build-deps.sh): the ps4-cc/ps4-c++ wrappers link configure's
    # test programs with the poison library, so Linux-only calls (epoll, fallocate, posix_fadvise,
    # getifaddrs, ...) are found missing. CFLAGS/CXXFLAGS: the wrappers' -O2 -g1, not autoconf's
    # "-g -O2" (full debug info made libaria2.a 199 MB).
    env -u CPPFLAGS -u LDFLAGS -u LIBS CFLAGS="-O2 -g1" CXXFLAGS="-O2 -g1" \
        CC=ps4-cc CXX=ps4-c++ AR=llvm-ar-$CLANG_VER RANLIB=llvm-ranlib-$CLANG_VER NM=llvm-nm-$CLANG_VER \
        STRIP=llvm-strip-$CLANG_VER OBJDUMP=llvm-objdump-$CLANG_VER CC_FOR_BUILD=gcc CPP_FOR_BUILD="gcc -E" \
        PKG_CONFIG=ps4-pkg-config \
        "$SRC/configure" --host=$HOST_TRIPLE --build=x86_64-pc-linux-gnu --prefix="$PREFIX" \
        --libdir="$PREFIX/lib" "${COMMON[@]}" --disable-epoll --with-ca-bundle="$CA_BUNDLE"
    check_config "$B/config.h"
    check_config_ps4 "$B/config.h"
    install_lib "$B" "$PREFIX"
    write_pc "$PREFIX" "High speed download utility library (Wisp, PS4: static; OpenSSL, zlib, expat)"

    # Link test: the API test with the app's link line (scripts/env.sh), no kit runtime or wraps.
    local t="$B/api-test" libs
    mkdir -p "$t"; api_test_source > "$t/aria2-api-test.cc"
    libs=$(PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PKG_CONFIG_LIBDIR" ps4-pkg-config --libs libaria2)
    echo "link libs: $libs"
    clang++-$CLANG_VER $WISP_CXXFLAGS $(PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PKG_CONFIG_LIBDIR" \
        ps4-pkg-config --cflags libaria2) -c "$t/aria2-api-test.cc" -o "$t/aria2-api-test.o"
    clang++-$CLANG_VER $WISP_CXXFLAGS "$t/aria2-api-test.o" $WISP_LINK_PRE $libs $WISP_LINK_POST_CXX \
        -Wl,-Map,"$t/aria2-api-test.map" -o "$t/aria2-api-test.elf"
    echo "PS4 link test: $t/aria2-api-test.elf"
    ls -l "$PREFIX/lib/libaria2.a" "$PREFIX/include/aria2/aria2.h" "$PREFIX/lib/pkgconfig/libaria2.pc"
}

# --- host (Linux, for API tests on the PC) ----------------------------------------------------------
host_openssl() {  # OpenSSL 3.5.0 in $HPREFIX/openssl, static, legacy provider built in as the kit's
    local tb="$S/aria2-host/$OSSL_TARBALL" ob="$WB/build/aria2-host-openssl"
    mkdir -p "$S/aria2-host"
    fetch "$tb" "$OSSL_SHA256" "$OSSL_URL" "$HOME/cemups4-build/src/deps/$OSSL_TARBALL" \
        "$HOME/pschrome-build/src/deps/$OSSL_TARBALL"
    [ -f "$HPREFIX/openssl/lib/libcrypto.a" ] && return 0
    rm -rf "$ob"; mkdir -p "$ob"
    tar xf "$tb" -C "$ob" --strip-components=1
    cd "$ob"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS CC=gcc \
        ./Configure linux-x86_64 no-shared no-module no-tests no-docs --prefix="$HPREFIX/openssl" \
        --libdir=lib --openssldir=/usr/lib/ssl
    make -j"$JOBS" build_libs
    make install_dev >/dev/null
}
b_host() {
    source_tree
    local pcpath=""
    if ! env -u PKG_CONFIG_LIBDIR pkg-config --exists 'openssl >= 3'; then
        host_openssl
        pcpath="$HPREFIX/openssl/lib/pkgconfig"
    fi
    rm -rf "$HB" "$HPREFIX/include" "$HPREFIX/lib/libaria2.a"; mkdir -p "$HB"; cd "$HB"
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS -u PKG_CONFIG_LIBDIR -u PKG_CONFIG_SYSROOT_DIR \
        PKG_CONFIG_PATH="$pcpath" PKG_CONFIG="pkg-config --static" CC=gcc CXX=g++ \
        "$SRC/configure" --prefix="$HPREFIX" "${COMMON[@]}" \
        --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt
    check_config "$HB/config.h"
    install_lib "$HB" "$HPREFIX"
    write_pc "$HPREFIX" "High speed download utility library (Wisp's PC API tests)"

    local t="$HB/api-test" pc="$HPREFIX/lib/pkgconfig${pcpath:+:$pcpath}"
    mkdir -p "$t"; api_test_source > "$t/aria2-api-test.cc"
    g++ -O1 -g "$t/aria2-api-test.cc" -o "$t/aria2-api-test" \
        $(env -u PKG_CONFIG_LIBDIR PKG_CONFIG_PATH="$pc" pkg-config --static --cflags --libs libaria2)
    rm -rf "$t/downloads"; mkdir -p "$t/downloads"
    echo "== $t/aria2-api-test https://ftp.gnu.org/gnu/hello/hello-2.12.3.tar.gz $t/downloads --wait"
    "$t/aria2-api-test" https://ftp.gnu.org/gnu/hello/hello-2.12.3.tar.gz "$t/downloads" --wait ||
        echo "!! the API test returned $? (network?)"
    ls -l "$t/downloads"
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
        echo "!! $1 failed: $log"; tail -30 "$log"; exit 1
    fi
}
case "${1:-ps4}" in
    ps4) run_logged "$L/aria2.log" b_ps4; echo "aria2 $VER for the PS4: $PREFIX" ;;
    host) run_logged "$L/aria2-host.log" b_host; tail -25 "$L/aria2-host.log" ;;
    *) echo "usage: $0 [ps4|host]" >&2; exit 2 ;;
esac
