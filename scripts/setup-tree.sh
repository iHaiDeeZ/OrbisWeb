#!/usr/bin/env bash
# Wisp: the WSL build tree, once (and again whenever env.sh's flags change).
#   bash "/mnt/e/PS4 Projects/Wisp/scripts/setup-tree.sh"
# Makes ~/wisp (link to the project), ~/wisp-build/{src,build,sysroot,logs,host/bin}, the meson
# cross file, the autotools compiler wrappers and the configure-time poison library.
set -euo pipefail
PROJ="/mnt/e/PS4 Projects/Wisp"
[ -L "$HOME/wisp" ] || ln -sfn "$PROJ" "$HOME/wisp"
source "$HOME/wisp/scripts/env.sh"
mkdir -p "$WB"/{src,build,logs,host/bin} "$WISP_SYSROOT"/{include,lib/pkgconfig}

# --- meson cross file -----------------------------------------------------------------------------
q() { local out="" a; for a in "$@"; do out+="'$a', "; done; echo "[${out%, }]"; }
read -r -a PRE <<< "$WISP_LINK_PRE"
read -r -a POST <<< "$WISP_LINK_POST"
read -r -a POSTXX <<< "$WISP_LINK_POST_CXX"
cat > "$WB/wisp-cross.ini" <<EOF
[binaries]
c = 'clang-$CLANG_VER'
cpp = 'clang++-$CLANG_VER'
ar = 'llvm-ar-$CLANG_VER'
strip = 'llvm-strip-$CLANG_VER'
ranlib = 'llvm-ranlib-$CLANG_VER'
nm = 'llvm-nm-$CLANG_VER'
objcopy = 'llvm-objcopy-$CLANG_VER'
pkg-config = '$WB/host/bin/ps4-pkg-config'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PKG_CONFIG_LIBDIR'

[built-in options]
c_args = $(q "${WISP_CFLAGS_LIST[@]}" -U__FreeBSD__)
cpp_args = $(q -nostdinc++ -isystem "$PS4_SYSROOT/include/c++/v1" "${WISP_CFLAGS_LIST[@]}")
c_link_args = $(q "${PRE[@]}" "${POST[@]}")
cpp_link_args = $(q "${PRE[@]}" "${POSTXX[@]}")

[host_machine]
system = 'freebsd'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

# --- pkg-config, always --static: every library here is an archive, so the private dependencies
# (libbrotlicommon behind libbrotlidec, libffi/pcre2 behind GLib, ...) belong on every link line.
printf '#!/bin/sh\nexec pkg-config --static "$@"\n' > "$WB/host/bin/ps4-pkg-config"
chmod +x "$WB/host/bin/ps4-pkg-config"

# --- autotools compiler wrappers ------------------------------------------------------------------
for kind in cc c++; do
    if [ $kind = cc ]; then comp="clang-$CLANG_VER"; flags="$WISP_CFLAGS"; post="$WISP_LINK_POST";
    else comp="clang++-$CLANG_VER"; flags="$WISP_CXXFLAGS"; post="$WISP_LINK_POST_CXX"; fi
    cat > "$WB/host/bin/ps4-$kind" <<EOF
#!/bin/bash
# Wisp: $comp for the PS4 (autotools' CC/CXX). Links test programs the way the kit links apps,
# with the poison library ahead of libc (scripts/env.sh).
link=1
for a in "\$@"; do case "\$a" in -c|-S|-E|-M|-MM) link=0 ;; esac; done
if [ \$link = 1 ]; then
    exec $comp $flags "\$@" $WISP_LINK_PRE $post
else
    exec $comp $flags "\$@"
fi
EOF
    chmod +x "$WB/host/bin/ps4-$kind"
done

# --- the poison library -----------------------------------------------------------------------------
# One member per function: a link check pulls only the member it asks for, which needs a symbol
# nothing defines. getrandom/getentropy are not here: the kit's runtime has PS4 versions.
NOLINUX=(eventfd eventfd_read eventfd_write pipe2 accept4 getifaddrs freeifaddrs copy_file_range
    fallocate posix_fallocate posix_fadvise splice tee vmsplice inotify_init inotify_init1
    inotify_add_watch inotify_rm_watch epoll_create epoll_create1 epoll_ctl epoll_wait epoll_pwait
    memfd_create dup3 ppoll getauxval clock_nanosleep posix_spawn posix_spawnp sched_getcpu prctl
    signalfd timerfd_create timerfd_settime timerfd_gettime sendmmsg recvmmsg syncfs readahead
    sync_file_range mremap remap_file_pages name_to_handle_at open_by_handle_at fanotify_init
    fanotify_mark unshare setns clone getdents64 statx renameat2 close_range sendfile
    pthread_setname_np pthread_getname_np getresuid getresgid setresuid
    setresgid prlimit getcpu pthread_getattr_np sched_setaffinity sched_getaffinity)
# (Not poisoned: musl's getpw*/getgr* only read /etc files and fail cleanly; __sched_cpucount is
# arithmetic. GLib calls both whatever its checks say.)
tmp="$(mktemp -d)"
for f in "${NOLINUX[@]}"; do
    printf 'extern void __wisp_linux_only_%s(void);\nvoid %s(void) { __wisp_linux_only_%s(); }\n' "$f" "$f" "$f" > "$tmp/$f.c"
    clang-$CLANG_VER --target=$PS4_TARGET -fPIC -O0 -c "$tmp/$f.c" -o "$tmp/nolinux_$f.o"
done
rm -f "$WISP_NOLINUX"
llvm-ar-$CLANG_VER rcs "$WISP_NOLINUX" "$tmp"/nolinux_*.o
rm -rf "$tmp"
echo "wisp tree: $WB (cross file, ps4-cc/ps4-c++, $(basename "$WISP_NOLINUX") with ${#NOLINUX[@]} functions)"
