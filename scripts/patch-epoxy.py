#!/usr/bin/env python3
"""Wisp: libepoxy for the PS4 - no shared GL libraries to dlopen.

Mesa (EGL + GLES on Zink) is linked statically into the web process only. Every name epoxy
would dlsym from libEGL / libGLESv2 is looked up through wisp_gl_lookup(), which the web process
defines (Mesa's eglGetProcAddress: EGL_KHR_get_all_proc_addresses) and the other processes leave
to the weak default here (NULL: no GL there).

    python3 patch-epoxy.py <libepoxy source dir>      (idempotent)
"""
import sys
from pathlib import Path

src = Path(sys.argv[1]) / "src" / "dispatch_common.c"
s = src.read_text()
if "wisp_gl_lookup" in s:
    sys.exit(0)

hook = '''
#if defined(__ORBIS__)
/* PS4 (Wisp): no libEGL/libGLESv2 to dlopen. The web process links Mesa statically and defines
 * wisp_gl_lookup() (its eglGetProcAddress); elsewhere there is no GL and lookups give NULL. */
__attribute__((weak)) void *wisp_gl_lookup(const char *name) { (void)name; return NULL; }
#define WISP_GL_HANDLE ((void *)1)
#endif

static bool
get_dlopen_handle(void **handle, const char *lib_name, bool exit_on_fail, bool load)
{'''
old = '''
static bool
get_dlopen_handle(void **handle, const char *lib_name, bool exit_on_fail, bool load)
{'''
assert old in s
s = s.replace(old, hook, 1)

old = '''#ifdef _WIN32
    *handle = LoadLibraryA(lib_name);
#else
    pthread_mutex_lock(&api.mutex);'''
new = '''#if defined(__ORBIS__)
    /* EGL and GLES come from the statically linked Mesa (web process); desktop GL and GLX never */
    if (strstr(lib_name, "EGL") || strstr(lib_name, "GLES"))
        *handle = wisp_gl_lookup("eglGetProcAddress") ? WISP_GL_HANDLE : NULL;
    if (!*handle && exit_on_fail) {
        fprintf(stderr, "Couldn't open %s: no GL in this process (PS4)\\n", lib_name);
        abort();
    }
#elif defined(_WIN32)
    *handle = LoadLibraryA(lib_name);
#else
    pthread_mutex_lock(&api.mutex);'''
assert old in s
s = s.replace(old, new, 1)

old = '''#ifdef _WIN32
    result = GetProcAddress(*handle, name);
#else
    result = dlsym(*handle, name);'''
new = '''#if defined(__ORBIS__)
    (void)handle;
    result = wisp_gl_lookup(name);
    if (!result)
        error = "not in the PS4 build of Mesa";
#elif defined(_WIN32)
    result = GetProcAddress(*handle, name);
#else
    result = dlsym(*handle, name);'''
assert old in s
s = s.replace(old, new, 1)

if "#include <string.h>" not in s:
    s = s.replace("#include <stdlib.h>", "#include <stdlib.h>\n#include <string.h>", 1)
src.write_text(s)
print("epoxy: patched for the PS4 (wisp_gl_lookup)")
