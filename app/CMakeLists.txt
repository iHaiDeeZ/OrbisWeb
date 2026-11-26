# Wisp: the PS4 browser's three processes on ps4-kit.
#   eboot.bin     the UI: the browser, WebKit's UI side, the WPE platform (ui/)
#   wisp-web.bin  WebKit's web process: WebCore, JavaScriptCore, Skia (CPU rendering)
#   wisp-web-gpu.bin  the same with Mesa (EGL + GLES on Zink + RADV), for GPU rendering (gpu=1)
#   wisp-net.bin  WebKit's network process: libsoup, glib-networking (OpenSSL)
# Built by app/build.sh (WSL); WebKit and the dependencies come from ~/wisp-build (scripts/).
cmake_minimum_required(VERSION 3.20)
project(wisp C CXX ASM)

set(WB $ENV{HOME}/wisp-build)
# RADV and the kit's presenter from Wisp's Mesa build (the kit's Mesa + Zink + static EGL)
set(PS4_MESA_BUILD ${WB}/build/mesa CACHE PATH "Wisp's Mesa build")
set(PS4_MESA_SRC ${WB}/src/mesa CACHE PATH "Wisp's Mesa source")
include($ENV{HOME}/ps4-kit/ps4-kit.cmake)

# The link script (the kit's copy of OpenOrbis's) KEEPs every .data.rel.ro section. Those hold the
# C++ vtables, and a kept vtable keeps every virtual function of its class: through them, most of
# WebKit stayed in each process whatever --gc-sections found unused (v00.04: the network process
# carried the layout engine, the DOM bindings and the DFG JIT, ~110 MiB). Here they are collected
# like any other data.
file(READ ${PS4_LINK_SCRIPT} _wisp_link_script)
string(FIND "${_wisp_link_script}" "KEEP(*(.data.rel.ro .data.rel.ro.*))" _wisp_relro)
if (_wisp_relro EQUAL -1)
    message(FATAL_ERROR "Unexpected ${PS4_LINK_SCRIPT}: no KEEP of .data.rel.ro to take out")
endif ()
string(REPLACE "KEEP(*(.data.rel.ro .data.rel.ro.*))" "*(.data.rel.ro .data.rel.ro.*)"
    _wisp_link_script "${_wisp_link_script}")
file(WRITE ${PS4_LINK_SCRIPT} "${_wisp_link_script}")

set(WISP_VERSION "00.00" CACHE STRING "Wisp's version (shown, and the package's)")
set(WISP_SYSROOT ${WB}/sysroot)
# the WebKit build to link (scripts/build-webkit.sh; WISP_WEBKIT_DIR picks another build folder).
# webkit-wasm: built for size, with WebAssembly (v00.21 on); webkit-small: the same without it (v00.04-
# v00.20); build/webkit is the first -O3 build, kept as a fallback.
if (DEFINED ENV{WISP_WEBKIT_DIR})
    set(WEBKIT_BUILD $ENV{WISP_WEBKIT_DIR})
else ()
    set(WEBKIT_BUILD ${WB}/build/webkit-wasm)
endif ()
set(WEBKIT_SRC ${WB}/src/WebKit)
set(SRC ${CMAKE_CURRENT_LIST_DIR}/src)

# --- The dependencies (static archives, through the --static pkg-config wrapper) ----------------------
set(WISP_PKGS gio-2.0 gio-unix-2.0 gmodule-2.0 libsoup-3.0 libxml-2.0 sqlite3 harfbuzz harfbuzz-icu icu-uc icu-i18n
    fontconfig freetype2 libpng libjpeg libwebp libwebpdemux libwebpmux xkbcommon epoxy libtasn1 libgcrypt gpg-error)
execute_process(COMMAND ${WB}/host/bin/ps4-pkg-config --libs ${WISP_PKGS}
    OUTPUT_VARIABLE WISP_DEP_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _r)
if (NOT _r EQUAL 0)
    message(FATAL_ERROR "pkg-config could not find Wisp's dependencies (source ~/wisp/scripts/env.sh first)")
endif ()
execute_process(COMMAND ${WB}/host/bin/ps4-pkg-config --cflags-only-I ${WISP_PKGS}
    OUTPUT_VARIABLE WISP_DEP_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
# ld.lld links directly (ps4.cmake): keep -L, -l and archives (no -pthread; gmodule's
# -Wl,--export-dynamic means nothing in a static PS4 program)
string(REPLACE "-pthread" "" WISP_DEP_LIBS "${WISP_DEP_LIBS}")
string(REPLACE "-Wl,--export-dynamic" "" WISP_DEP_LIBS "${WISP_DEP_LIBS}")
separate_arguments(WISP_DEP_LIBS)
separate_arguments(WISP_DEP_CFLAGS)
# Each -lname as the archive's full path: the toolchain's -L to PacBrew's usr/lib comes first on
# the link line, and its old libfreetype.a (no COLRv1 functions) was taken over ours
set(_libs)
foreach (f ${WISP_DEP_LIBS})
    if (f MATCHES "^-l(.+)$")
        set(_name ${CMAKE_MATCH_1})
        if (EXISTS ${WISP_SYSROOT}/lib/lib${_name}.a)
            list(APPEND _libs ${WISP_SYSROOT}/lib/lib${_name}.a)
        elseif (EXISTS ${PS4_SYSROOT}/lib/lib${_name}.a)
            list(APPEND _libs ${PS4_SYSROOT}/lib/lib${_name}.a)
        else ()
            list(APPEND _libs ${f})
        endif ()
    elseif (NOT f MATCHES "^-L")
        list(APPEND _libs ${f})
    endif ()
endforeach ()
set(WISP_DEP_LIBS ${_libs})
set(WISP_DEP_INCLUDES)
foreach (f ${WISP_DEP_CFLAGS})
    string(REGEX REPLACE "^-I" "" d "${f}")
    list(APPEND WISP_DEP_INCLUDES ${d})
endforeach ()

# WebKit's public headers, where its build tree has them (generated ones first, then the plain ones)
set(WEBKIT_INCLUDES ${WEBKIT_BUILD}/DerivedSources/WebKit ${WEBKIT_BUILD}/DerivedSources/WPEPlatform
    ${WEBKIT_SRC}/Source/WebKit/WPEPlatform ${WEBKIT_SRC}/Source/WebKit/UIProcess/API
    ${WEBKIT_BUILD}/JavaScriptCoreGLib/DerivedSources ${WEBKIT_BUILD}/JavaScriptCoreGLib/Headers)

# WebKit with media (v00.17 on, scripts/build-webkit.sh: <video>, Media Source, Web Audio): GStreamer in
# one static library and the FFmpeg 8 it decodes with (scripts/build-media.sh: ~/wisp-build/media; full
# paths, as the kit's sysroot has FFmpeg 7.1's archives under the same names)
set(WISP_MEDIA ${WB}/media)
file(STRINGS ${WEBKIT_BUILD}/CMakeCache.txt WEBKIT_HAS_MEDIA REGEX "^USE_GSTREAMER:BOOL=ON$")
set(WEBKIT_MEDIA_LIBS)
if (WEBKIT_HAS_MEDIA)
    set(WEBKIT_MEDIA_LIBS ${WISP_MEDIA}/lib/libgstreamer-full-1.0.a ${WISP_MEDIA}/lib/libavfilter.a
        ${WISP_MEDIA}/lib/libavformat.a ${WISP_MEDIA}/lib/libavcodec.a ${WISP_MEDIA}/lib/libswresample.a
        ${WISP_MEDIA}/lib/libswscale.a ${WISP_MEDIA}/lib/libavutil.a ${WISP_MEDIA}/lib/libopus.a
        ${WISP_MEDIA}/lib/libtheoraenc.a ${WISP_MEDIA}/lib/libtheoradec.a ${WISP_MEDIA}/lib/libvorbisenc.a
        ${WISP_MEDIA}/lib/libvorbis.a ${WISP_MEDIA}/lib/libogg.a)
    message(STATUS "Wisp: WebKit with media (GStreamer and FFmpeg from ${WISP_MEDIA})")
else ()
    message(STATUS "Wisp: WebKit without media (${WEBKIT_BUILD})")
endif ()

# The web process's WebKit (v00.27 on): the same sources and options with JavaScriptCore's JIT and DFG
# tiers (scripts/build-webkit.sh WISP_WEBKIT_JIT=1: build/webkit-jit; WISP_WEBKIT_JIT_DIR picks another).
# The UI and network processes keep WEBKIT_BUILD, without them: the JIT adds ~6 MiB of image, and every
# image comes out of the pool the processes share. Without that build the web process links WEBKIT_BUILD
# too, and JavaScript runs on the LLInt interpreter.
if (DEFINED ENV{WISP_WEBKIT_JIT_DIR})
    set(WEBKIT_JIT_BUILD $ENV{WISP_WEBKIT_JIT_DIR})
else ()
    set(WEBKIT_JIT_BUILD ${WB}/build/webkit-jit)
endif ()
set(WEBKIT_WEB_HAS_JIT FALSE)
if (EXISTS ${WEBKIT_JIT_BUILD}/libwisp-webkit.a AND EXISTS ${WEBKIT_JIT_BUILD}/CMakeCache.txt)
    file(STRINGS ${WEBKIT_JIT_BUILD}/CMakeCache.txt _wisp_jit REGEX "^ENABLE_JIT:BOOL=ON$")
    if (_wisp_jit)
        set(WEBKIT_WEB_HAS_JIT TRUE)
        message(STATUS "Wisp: the web process with JavaScriptCore's JIT (${WEBKIT_JIT_BUILD})")
    endif ()
endif ()
if (NOT WEBKIT_WEB_HAS_JIT)
    message(STATUS "Wisp: no JIT build at ${WEBKIT_JIT_BUILD}: the web process runs JavaScript on the LLInt")
endif ()
# WebGL 1 and 2 (v00.30 on): the web process's build has it (ANGLE on Mesa in the GPU web process); the
# UI's WebGL setting follows
set(WEBKIT_WEB_HAS_WEBGL FALSE)
if (WEBKIT_WEB_HAS_JIT)
    file(STRINGS ${WEBKIT_JIT_BUILD}/CMakeCache.txt _wisp_webgl REGEX "^ENABLE_WEBGL:BOOL=ON$")
    if (_wisp_webgl)
        set(WEBKIT_WEB_HAS_WEBGL TRUE)
        message(STATUS "Wisp: the web process with WebGL (ANGLE)")
    endif ()
endif ()
# WebAssembly's JIT (v00.34 on): BBQ and OMG, with the FTL they are built on, in the web process's build;
# the web process's settings wasm_jit and js_ftl then apply (child/child_main.cpp)
set(WEBKIT_WEB_HAS_WASM_JIT FALSE)
if (WEBKIT_WEB_HAS_JIT)
    file(STRINGS ${WEBKIT_JIT_BUILD}/CMakeCache.txt _wisp_bbq REGEX "^ENABLE_WEBASSEMBLY_BBQJIT:BOOL=ON$")
    if (_wisp_bbq)
        set(WEBKIT_WEB_HAS_WASM_JIT TRUE)
        message(STATUS "Wisp: the web process with WebAssembly's JIT (BBQ, OMG)")
    endif ()
endif ()
# WebGPU for pages (v00.40 on): the web process's build has WebCore's WebGPU on Dawn (scripts/build-webkit.sh,
# WISP_WEBKIT_WEBGPU; WebKit patch 0039). The GPU web process links Dawn (below); the CPU one its stubs.
set(WEBKIT_WEB_HAS_WEBGPU FALSE)
if (WEBKIT_WEB_HAS_JIT)
    file(STRINGS ${WEBKIT_JIT_BUILD}/CMakeCache.txt _wisp_webgpu REGEX "^CMAKE_CXX_FLAGS_RELEASE:.*-DHAVE_WEBGPU_IMPLEMENTATION=1")
    if (_wisp_webgpu)
        set(WEBKIT_WEB_HAS_WEBGPU TRUE)
    endif ()
endif ()

# wisp_link_webkit(target [build folder]): WebKit's libraries from that build (WEBKIT_BUILD by default)
function(wisp_link_webkit target)
    set(build ${WEBKIT_BUILD})
    if (ARGC GREATER 1)
        set(build ${ARGV1})
    endif ()
    target_include_directories(${target} SYSTEM PRIVATE ${WEBKIT_INCLUDES} ${WISP_DEP_INCLUDES})
    # what WebKit's installed headers have resolved (its build tree's still test these)
    target_compile_definitions(${target} PRIVATE ENABLE_2022_GLIB_API=1 WTF_PLATFORM_WPE=1 ENABLE_WPE_PLATFORM=1)
    # (ps4.cmake's link rule already puts everything in one --start-group; a process takes from the media
    # libraries only what its part of WebKit refers to)
    target_link_libraries(${target} PRIVATE ${build}/libwisp-webkit.a ${build}/lib/libPAL.a ${build}/lib/libSkia.a
        ${build}/lib/libxdgmime-wisp.a ${WEBKIT_MEDIA_LIBS} ${WISP_DEP_LIBS}
        ${PS4_SYSROOT}/lib/libssl.a ${PS4_SYSROOT}/lib/libcrypto.a ${PS4_SYSROOT}/lib/libz.a)
    # one copy of each function and its data: unused WebCore code goes
    target_link_options(${target} PRIVATE --gc-sections)
endfunction()

# --- Shared by the three ---------------------------------------------------------------------------
add_library(wisp_common OBJECT ${SRC}/common/wisp_log.cpp ${SRC}/common/wisp_settings.cpp
    ${SRC}/common/wisp_memory.cpp ${SRC}/common/wisp_handoff.cpp ${SRC}/common/wisp_libc.cpp
    ${SRC}/common/wisp_icu.cpp)
target_include_directories(wisp_common SYSTEM PRIVATE ${WISP_DEP_INCLUDES} ${PS4_KIT_DIR}/runtime)
target_compile_definitions(wisp_common PRIVATE WISP_VERSION="${WISP_VERSION}")
target_compile_options(wisp_common PRIVATE -std=c++20 -O2)

# --- The UI (eboot.bin) ---------------------------------------------------------------------------
add_executable(wisp ${SRC}/ui/main.cpp ${SRC}/ui/browser.cpp ${SRC}/ui/cursor.cpp ${SRC}/ui/platform.cpp ${SRC}/ui/process_manager.cpp
    ${SRC}/ui/audio_out.cpp
    ${SRC}/ui/ext/extensions.cpp ${SRC}/ui/ext/package.cpp ${SRC}/ui/files/files.cpp ${SRC}/ui/pages.cpp
    ${SRC}/ui/gpu_space.c ${SRC}/ui/shell/screen.cpp ${SRC}/ui/shell/input.cpp ${SRC}/ui/shell/keyboard.cpp
    ${SRC}/ui/shell/hwkeyboard.cpp ${SRC}/ui/shell/ps4_canvas.cpp ${SRC}/ui/shell/fileops.cpp ${SRC}/ui/shell/stb_impl.c
    $<TARGET_OBJECTS:wisp_common>)
target_include_directories(wisp BEFORE PRIVATE ${SRC}/ui/shell ${SRC}/ui/shell/compat ${SRC}/ui/shell/third_party
    ${PS4_SYSROOT}/include)
# pages' sound plays from the UI (v00.35, ui/audio_out.cpp): the PS4's audio out and the user's id
target_link_libraries(wisp PRIVATE SceAudioOut SceUserService)
target_compile_definitions(wisp PRIVATE WISP_VERSION="${WISP_VERSION}" FMT_HEADER_ONLY)
# WebKit with WebAssembly (v00.21 on, scripts/build-webkit.sh): the menu's WebAssembly switch
file(STRINGS ${WEBKIT_BUILD}/CMakeCache.txt WEBKIT_HAS_WASM REGEX "^ENABLE_WEBASSEMBLY:BOOL=ON$")
if (WEBKIT_HAS_WASM)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_WASM=1)
endif ()
# the web process's JIT (v00.27 on): the menu's JavaScript JIT switch
if (WEBKIT_WEB_HAS_JIT)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_JIT=1)
endif ()
# the web process's WebGL (v00.30 on): the menu's WebGL switch
if (WEBKIT_WEB_HAS_WEBGL)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_WEBGL=1)
endif ()
# the web process's WebAssembly JIT (v00.34 on): Settings' WebAssembly JIT and FTL switches
if (WEBKIT_WEB_HAS_WASM_JIT)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_WASM_JIT=1)
endif ()
target_compile_options(wisp PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++20> -O2)
wisp_link_webkit(wisp)
target_link_libraries(wisp PRIVATE SceVideoOut ScePad SceUserService SceMouse SceKeyboard SceIme SceImeDialog
    SceCommonDialog SceNetCtl)
# the download manager (ui/dm): aria2's library (scripts/build-aria2.sh: ~/wisp-build/aria2); without
# it the page is there and the browser's own downloader does every download
target_sources(wisp PRIVATE ${SRC}/ui/dm/dm.cpp)
set(WISP_ARIA2 ${WB}/aria2)
if (EXISTS ${WISP_ARIA2}/lib/libaria2.a)
    target_include_directories(wisp SYSTEM PRIVATE ${WISP_ARIA2}/include)
    # (OpenSSL and zlib come with WebKit's libraries: wisp_link_webkit; Metalink's XML is expat)
    target_link_libraries(wisp PRIVATE ${WISP_ARIA2}/lib/libaria2.a ${WISP_SYSROOT}/lib/libexpat.a)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_ARIA2=1)
    message(STATUS "Wisp: the download manager with aria2 (${WISP_ARIA2})")
else()
    message(STATUS "Wisp: no aria2 at ${WISP_ARIA2}: the download manager's page without its engine")
endif()
# Files' archives (ui/files/archives.cpp): libarchive and its decompressors (scripts/build-archive.sh:
# ~/wisp-build/archive); without them Files opens no archive as a folder
target_sources(wisp PRIVATE ${SRC}/ui/files/archives.cpp)
set(WISP_ARCHIVE ${WB}/archive)
if (EXISTS ${WISP_ARCHIVE}/lib/libarchive.a)
    target_include_directories(wisp SYSTEM PRIVATE ${WISP_ARCHIVE}/include)
    # By full path: the kit's sysroot has a libzstd.a of its own. (zlib, and OpenSSL's libcrypto for
    # ZIP's passwords, come with WebKit's libraries; libxml2, for XAR, with the dependencies.)
    set(_archive_libs ${WISP_ARCHIVE}/lib/libarchive.a)
    foreach (lib liblzma libzstd liblz4 libbz2)
        if (EXISTS ${WISP_ARCHIVE}/lib/${lib}.a)
            list(APPEND _archive_libs ${WISP_ARCHIVE}/lib/${lib}.a)
        endif ()
    endforeach ()
    target_link_libraries(wisp PRIVATE ${_archive_libs} ${WISP_SYSROOT}/lib/libxml2.a)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_LIBARCHIVE=1)
    message(STATUS "Wisp: Files opens archives (libarchive at ${WISP_ARCHIVE})")
    # RAR through RARLAB's UnRAR (scripts/build-unrar.sh: ~/wisp-build/unrar, v00.27 on): encrypted RAR
    # files and lists, which libarchive cannot decrypt; without it, libarchive reads RAR. Only with
    # WISP_WITH_UNRAR=1 in the environment (v1.00): UnRAR's license adds restrictions the GPL forbids, and this
    # program carries GPL code (aria2, the Vita3K canvas), so builds given to others leave it out.
    set(WISP_UNRAR ${WB}/unrar)
    if ("$ENV{WISP_WITH_UNRAR}" STREQUAL "1" AND EXISTS ${WISP_UNRAR}/lib/libunrar.a)
        target_include_directories(wisp SYSTEM PRIVATE ${WISP_UNRAR}/include)
        target_link_libraries(wisp PRIVATE ${WISP_UNRAR}/lib/libunrar.a)
        target_compile_definitions(wisp PRIVATE WISP_HAVE_UNRAR=1)
        message(STATUS "Wisp: RAR archives through UnRAR (${WISP_UNRAR})")
    else ()
        message(STATUS "Wisp: no UnRAR at ${WISP_UNRAR}: libarchive reads RAR (no encrypted ones)")
    endif ()
else ()
    message(STATUS "Wisp: no libarchive at ${WISP_ARCHIVE}: Files opens no archive")
endif ()
# Wisp Player (ui/player): FFmpeg (scripts/build-media.sh: ~/wisp-build/media) and the PS4's audio out;
# without FFmpeg, video and music files open in a tab as before
target_sources(wisp PRIVATE ${SRC}/ui/player/player.cpp)
if (EXISTS ${WISP_MEDIA}/lib/libavcodec.a)
    target_include_directories(wisp SYSTEM PRIVATE ${WISP_MEDIA}/include)
    target_link_libraries(wisp PRIVATE ${WISP_MEDIA}/lib/libavformat.a ${WISP_MEDIA}/lib/libavcodec.a
        ${WISP_MEDIA}/lib/libswscale.a ${WISP_MEDIA}/lib/libswresample.a ${WISP_MEDIA}/lib/libavutil.a SceAudioOut)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_FFMPEG=1)
    message(STATUS "Wisp: the player with FFmpeg (${WISP_MEDIA})")
else()
    message(STATUS "Wisp: no FFmpeg at ${WISP_MEDIA}: no player")
endif()
ps4_kit_app(wisp)

# --- WebKit's processes ---------------------------------------------------------------------------
# Mesa for the web process: EGL (surfaceless) + GLES + Zink, with RADV (ps4_kit_link_mesa: whole
# archives; Mesa's dispatch refers to entry points through weak symbols, which pull no members).
set(WISP_GL_LIBS src/egl/libEGL.a src/gallium/targets/dri/libgallium-26.0.8.a src/gallium/drivers/zink/libzink.a
    src/gallium/auxiliary/libgallium.a src/gallium/auxiliary/pipe-loader/libpipe_loader_static.a
    src/gallium/winsys/sw/null/libws_null.a src/gallium/winsys/sw/wrapper/libwsw.a src/gallium/winsys/sw/dri/libswdri.a
    src/gallium/auxiliary/libgalliumvl.a src/mesa/libmesa.a src/mesa/libmesa_sse41.a
    src/mesa/glapi/glapi/libglapi_bridge.a src/mesa/glapi/shared-glapi/libglapi.a src/compiler/glsl/libglsl.a
    src/compiler/glsl/glcpp/libglcpp.a src/loader/libloader.a)
list(TRANSFORM WISP_GL_LIBS PREPEND "${PS4_MESA_BUILD}/")

# wisp_web: the web process for CPU rendering, without Mesa (~23 MiB less of the flexible memory pool
# the app's processes share); wisp_web_gpu: the same with Mesa, started when GPU rendering is on
foreach (child web web_gpu net)
    add_executable(wisp_${child} ${SRC}/child/child_main.cpp $<TARGET_OBJECTS:wisp_common>)
    target_include_directories(wisp_${child} PRIVATE ${PS4_KIT_DIR}/runtime)
    # (C++20 for the C++ sources only: the web processes have a C one, child/gst_ps4_audiosink.c)
    target_compile_options(wisp_${child} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=c++20> -O2)
    if (WEBKIT_WEB_HAS_JIT AND NOT child STREQUAL "net")
        wisp_link_webkit(wisp_${child} ${WEBKIT_JIT_BUILD})
        target_compile_definitions(wisp_${child} PRIVATE WISP_HAVE_JIT=1)
        if (WEBKIT_WEB_HAS_WASM_JIT)
            target_compile_definitions(wisp_${child} PRIVATE WISP_HAVE_WASM_JIT=1)
        endif ()
    else ()
        wisp_link_webkit(wisp_${child})
    endif ()
    ps4_kit_app(wisp_${child})
endforeach ()
target_compile_definitions(wisp_web PRIVATE WISP_VERSION="${WISP_VERSION}" WISP_CHILD_WEB)
target_compile_definitions(wisp_web_gpu PRIVATE WISP_VERSION="${WISP_VERSION}" WISP_CHILD_WEB WISP_WEB_GPU)
# WebKit's resource bundles register themselves from constructors, and nothing refers to their
# objects, so a static link left them out: the web process had no form-control images (a text area's
# resize corner, the missing-image icon; "g_bytes_get_data: assertion 'bytes != NULL'", v00.07).
# WebKitResources is pulled in by its accessor; PDF.js's bundles (3.6 MiB of the image) stay out, and the
# media controls' come in with media (below).
foreach (child web web_gpu)
    target_link_options(wisp_${child} PRIVATE --undefined=WebKitResourcesGResourceBundle_get_resource)
endforeach ()
# (v00.41) The kernel-object census (child/kobj_census.cpp): the web processes' pthread, semaphore and kernel
# object calls measured against the PS4's internal memory, each charged to its caller (settings: kobj_census)
set(WISP_KOBJ_WRAPS pthread_mutex_init pthread_mutex_destroy pthread_mutex_lock pthread_mutex_trylock
    pthread_cond_init pthread_cond_destroy pthread_cond_wait pthread_cond_timedwait pthread_cond_signal
    pthread_cond_broadcast pthread_rwlock_init pthread_rwlock_destroy pthread_rwlock_rdlock pthread_rwlock_wrlock
    pthread_attr_init pthread_attr_destroy pthread_mutexattr_init pthread_mutexattr_destroy pthread_condattr_init
    pthread_condattr_destroy sem_init sem_destroy pthread_key_create pthread_key_delete sceKernelCreateEqueue
    sceKernelDeleteEqueue sceKernelCreateSema sceKernelDeleteSema sceKernelCreateEventFlag sceKernelDeleteEventFlag
    sceKernelAllocateDirectMemory sceKernelReleaseDirectMemory sceKernelMapDirectMemory sceKernelMapNamedDirectMemory
    sceKernelReserveVirtualRange)
foreach (child web web_gpu)
    target_sources(wisp_${child} PRIVATE ${SRC}/child/kobj_census.cpp)
    foreach (fn ${WISP_KOBJ_WRAPS})
        target_link_options(wisp_${child} PRIVATE --wrap=${fn})
    endforeach ()
endforeach ()
# With media: the UI and network processes play nothing, but WebKit's code they keep reaches gst_init,
# which registers every GStreamer plugin and with them FFmpeg's decoders (~14 MiB more of program image
# each, out of the shared pool): gst_init is a stub there (common/no_media.cpp)
if (WEBKIT_HAS_MEDIA)
    foreach (target wisp wisp_net)
        target_sources(${target} PRIVATE ${SRC}/common/no_media.cpp)
        target_link_options(${target} PRIVATE --wrap=gst_init_check --wrap=gst_init)
    endforeach ()
endif ()
# With media: GStreamer's start and the PS4's audio sink (child/gst_ps4_audiosink.c), and the media
# controls' bundle (the buttons of <video controls>)
if (WEBKIT_HAS_MEDIA)
    foreach (child web web_gpu)
        target_sources(wisp_${child} PRIVATE ${SRC}/child/gst_ps4_audiosink.c)
        target_include_directories(wisp_${child} SYSTEM PRIVATE ${WISP_MEDIA}/include/gstreamer-1.0)
        target_compile_definitions(wisp_${child} PRIVATE WISP_HAVE_GSTREAMER=1 GST_STATIC_COMPILATION)
        target_link_libraries(wisp_${child} PRIVATE SceAudioOut SceUserService)
        target_link_options(wisp_${child} PRIVATE --undefined=ModernMediaControlsGResourceBundle_get_resource)
    endforeach ()
endif ()
target_compile_definitions(wisp_net PRIVATE WISP_VERSION="${WISP_VERSION}" WISP_CHILD_NETWORK)
target_link_libraries(wisp_net PRIVATE ${WISP_SYSROOT}/lib/libgioopenssl.a)
# RADV and the kit's presenter as ps4_kit_link_mesa links them, but libvulkan_util as a plain archive:
# libzink.a carries 15 of its objects, and two whole copies collide (Etterna's link does the same)
set(WISP_RADV_LIBS src/amd/vulkan/libvulkan_radeon.a src/amd/common/libamd_common.a src/amd/compiler/libaco.a
    src/amd/addrlib/libaddrlib.a src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
    src/util/libxmlconfig.a src/util/libmesa_util.a src/util/libmesa_util_simd.a src/util/libmesa_util_clflush.a
    src/util/libmesa_util_clflushopt.a src/util/libparson.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a)
list(TRANSFORM WISP_RADV_LIBS PREPEND "${PS4_MESA_BUILD}/")
target_sources(wisp_web_gpu PRIVATE ${PS4_KIT_DIR}/gpu/ps4_vk_wsi.cpp)
target_include_directories(wisp_web_gpu PRIVATE ${PS4_KIT_DIR}/gpu ${PS4_MESA_SRC}/include)
target_link_libraries(wisp_web_gpu PRIVATE --whole-archive ${WISP_RADV_LIBS} ${WISP_GL_LIBS} --no-whole-archive
    ${PS4_MESA_BUILD}/src/vulkan/util/libvulkan_util.a ${PS4_MESA_BUILD}/src/vulkan/runtime/libvulkan_lite_runtime.a
    -lSceVideoOut -lSceSystemService)
# Dawn (WebGPU, v00.31 on: scripts/build-dawn.sh, docs/WEBGPU.md), in the GPU web process only; so far its
# self-test (menu: WebGPU test). Its Vulkan is the kit's RADV above (ps4kit_vk_get_instance_proc_addr).
set(WISP_DAWN_BUILD ${WB}/build/dawn CACHE PATH "Wisp's Dawn build")
set(WISP_DAWN_LIB ${WISP_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a)
if (EXISTS ${WISP_DAWN_LIB})
    target_sources(wisp_web_gpu PRIVATE ${SRC}/child/webgpu_selftest.cpp)
    target_include_directories(wisp_web_gpu SYSTEM PRIVATE ${WISP_DAWN_BUILD}/gen/include ${WB}/src/dawn/include)
    target_link_libraries(wisp_web_gpu PRIVATE ${WISP_DAWN_LIB})
    target_compile_definitions(wisp_web_gpu PRIVATE WISP_HAVE_DAWN=1)
    target_compile_definitions(wisp PRIVATE WISP_HAVE_DAWN=1)
    message(STATUS "Wisp: the GPU web process with Dawn (the WebGPU self-test)")
    # WebGPU for pages (v00.40): the GPU web process says whether its pages may have it (child_main.cpp's
    # wisp_webgpu_usable, the setting webgpu), the UI turns WebKit's feature on; the CPU web process has
    # WebKit's WebGPU code too but no GPU: do-nothing wgpu functions (child/webgpu_stubs.c, generated by
    # scripts/gen-webgpu-stubs.py), and WebKit's weak wisp_webgpu_usable says no
    if (WEBKIT_WEB_HAS_WEBGPU)
        target_sources(wisp_web PRIVATE ${SRC}/child/webgpu_stubs.c)
        target_include_directories(wisp_web SYSTEM PRIVATE ${WISP_DAWN_BUILD}/gen/include ${WB}/src/dawn/include)
        target_compile_definitions(wisp_web_gpu PRIVATE WISP_HAVE_WEBGPU=1)
        target_compile_definitions(wisp PRIVATE WISP_HAVE_WEBGPU=1)
        message(STATUS "Wisp: WebGPU for pages (WebCore's WebGPU on Dawn in the GPU web process)")
    endif ()
elseif (WEBKIT_WEB_HAS_WEBGPU)
    message(FATAL_ERROR "The web process's WebKit has WebGPU, but there is no Dawn at ${WISP_DAWN_LIB} (scripts/build-dawn.sh)")
endif ()
