# Wisp's CMake toolchain: the kit's, plus our dependency sysroot (~/wisp-build/sysroot) and the
# configure-time poison library ahead of libc (scripts/env.sh). The _INIT variables, not -D flags:
# a -DCMAKE_C_FLAGS on the command line replaces the toolchain's target flags instead of adding to them.
include($ENV{HOME}/ps4-kit/toolchain/ps4-kit-toolchain.cmake)

if (DEFINED ENV{WISP_SYSROOT})
    set(WISP_SYSROOT $ENV{WISP_SYSROOT})
else ()
    set(WISP_SYSROOT $ENV{HOME}/wisp-build/sysroot)
endif ()

# C as generic Unix (scripts/env.sh): clang's FreeBSD target defines __FreeBSD__, musl's headers are not FreeBSD's.
string(APPEND CMAKE_C_FLAGS_INIT " -U__FreeBSD__ -nostdlibinc -isystem ${WISP_SYSROOT}/include -isystem ${PS4_SYSROOT}/include -g1")
string(APPEND CMAKE_CXX_FLAGS_INIT " -nostdlibinc -isystem ${WISP_SYSROOT}/include -isystem ${PS4_SYSROOT}/include -g1")
string(APPEND CMAKE_ASM_FLAGS_INIT " -nostdlibinc -isystem ${WISP_SYSROOT}/include")
if (NOT WISP_NO_POISON)
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${WISP_SYSROOT}/lib/libwisp_nolinux.a")
endif ()
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -L${WISP_SYSROOT}/lib -L${PS4_SYSROOT}/lib -lSceNet")

list(PREPEND CMAKE_FIND_ROOT_PATH ${WISP_SYSROOT})
# GStreamer and FFmpeg 8 (scripts/build-media.sh: WebKit's media, Wisp Player): a find root too, after the
# sysroots (the kit's mode finds libraries and headers only under a root)
if (EXISTS $ENV{HOME}/wisp-build/media/lib)
    list(APPEND CMAKE_FIND_ROOT_PATH $ENV{HOME}/wisp-build/media)
endif ()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WISP_SYSROOT WISP_NO_POISON)
