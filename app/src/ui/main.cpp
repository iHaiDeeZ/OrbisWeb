// Wisp: the UI process (eboot.bin). The browser runs here; WebKit's web and network processes are
// local processes the UI starts (ui/process_manager.cpp, child/child_main.cpp).
#include "../common/wisp.h"
#include "browser.h"

#include <clocale>
#include <cstdlib>
#include <jsc/jsc.h>
#include <ps4_kit.h>
#include <ps4_kit_diag.h>
#include <sys/stat.h>

// the kit's crash log and boot trace of this process (each process has its own folder)
PS4_DIAG_DATA_DIR("/data/Wisp/kit-ui");

// The app's flexible memory: the pool the app's processes share, where every program image, frame
// buffer and the kernel's own objects live (running it dry freezes the app). The kernel's default is
// 448 MiB of it (v00.18: UI 109 + network 92 + web 127 MiB of images left ~100 MiB), the rest of the
// app's memory direct (the heaps). The kernel reads the size asked for from the eboot's process
// parameters (_sceKernelMemParam's flexible memory size; the kit's crt has none: ps4-kit
// runtime/ps4_crt1.S). v00.19 asked for 1 GiB: on .43 the app ended before its first line (no log,
// no crash log), so the request is off: WISP_FLEXIBLE_MIB 0 keeps the kernel's 448 MiB (the kit's
// all-null parameters). The children's parameters stay the kit's (the pool is the app's).
#define WISP_FLEXIBLE_MIB 0
#if WISP_FLEXIBLE_MIB > 0
extern "C" {
uint64_t wisp_flexible_memory_size = static_cast<uint64_t>(WISP_FLEXIBLE_MIB) << 20;
struct WispKernelMemParam {
    uint64_t size;
    void *extended_page_table;
    uint64_t *flexible_memory_size;
    void *extended_memory_1;
    void *extended_gpu_page_table;
    void *extended_memory_2;
};
static_assert(sizeof(WispKernelMemParam) == 0x30, "SceKernelMemParam");
// (not const: a const at namespace scope has internal linkage, extern "C" or not, and the crt's weak
// all-null one would stay the process parameters' choice; v00.19's first build)
__attribute__((used, section(".data.rel.ro._sceKernelMemParam"))) WispKernelMemParam _sceKernelMemParam = {
    sizeof(WispKernelMemParam), nullptr, &wisp_flexible_memory_size, nullptr, nullptr, nullptr };
}
#endif

extern "C" {
int sceSysmoduleLoadModuleInternal(uint32_t id);

// The heap: direct memory first; the UI's own needs are small (GLib, WebKit's UI side, the toolbar)
// (The shell modules' trace lines go to the kit's boot trace, /data/Wisp/kit-ui/boot-trace.log.)
void ps4_kit_configure_heap(ps4_kit_heap_options *options)
{
    options->log_tag = "ui";
    options->source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    // 512 MiB of the app's ~4.5 GiB of direct memory (the player's pictures, aria2's buffers, archives
    // being unpacked; it used ~100 MiB before them). 768 up to v00.35: every heap takes its direct memory
    // whole at the start, and pages need what is left (a game's WebAssembly memory, the GPU's)
    options->direct_max = static_cast<size_t>(512) << 20;
    // thread stacks from the first thread on (this runs at the heap's start, before any): every stack
    // comes out of the flexible memory pool the app's processes share, and the app's main process
    // gives a thread 2 MiB when nothing asks otherwise (v00.12: 13 threads, 26 MiB)
    ps4_kit_set_min_thread_stack(static_cast<size_t>(256) << 10);
    ps4_kit_set_default_thread_stack(static_cast<size_t>(256) << 10);
    // and since v00.17 out of this heap, not that pool (settings: stacks_in_heap=0, read in main)
    ps4_kit_set_thread_stacks_from_heap(1);
}
}

int main(int, char **)
{
    // thread stacks come out of the flexible memory pool the web process needs (child/child_main.cpp):
    // 256 KiB a thread at least here, not the kit's 2 MiB (that is for GPU shader compilers; the UI has
    // no GPU; Sony's WebKit runs its threads on the system's 64 KiB)
    ps4_kit_set_min_thread_stack(static_cast<size_t>(256) << 10);
    if (!wisp_setting_int("stacks_in_heap", 1))
        ps4_kit_set_thread_stacks_from_heap(0);
    wisp_log_open(WispProcess::UI);
    // characters as UTF-8 (the C locale's are ASCII): file names in archives (libarchive converts names
    // through the locale's character set; non-ASCII ones failed)
    if (!setlocale(LC_CTYPE, "C.UTF-8"))
        wisp_log("locale: C.UTF-8 not taken: archives' non-ASCII names may come out wrong");
    // every system module before its first call (a call into an unloaded one ends the app silently)
    static const uint32_t modules[] = {
        0x80000010,  // SystemService
        0x80000011,  // UserService
        0x80000022,  // VideoOut
        0x80000024,  // Pad
        0x8000001C,  // Net
        0x80000009,  // NetCtl
        0x80000001,  // AudioOut (Wisp Player)
    };
    for (uint32_t id : modules) {
        const int result = sceSysmoduleLoadModuleInternal(id);
        if (result)
            wisp_log("module %#x: %#x", id, result);
    }
    char heap[160];
    ps4rt_heap_report(heap, sizeof heap);
    wisp_log("heap: %s (%s); code: %s", heap, ps4rt_heap_source(), ps4_kit_exec_source());
    {
        // what the pool came to (only this process holds any of it yet)
        WispMemoryCensus census;
        wisp_memory_census(census);
        wisp_log("memory: the app's flexible memory pool: %zu MiB in all (asked for %d MiB, 0: the kernel's default); "
            "%zu MiB of direct memory", static_cast<size_t>((wisp_flexible_free() + census.flexible) >> 20),
            WISP_FLEXIBLE_MIB, wisp_direct_memory_size() >> 20);
    }

    // WebKit's UI side (GLib, the network session's paths); the children set the same for themselves
    mkdir(WISP_DATA, 0777);
    mkdir(WISP_DATA "/tmp", 0777);
    mkdir(WISP_DATA "/run", 0777);
    mkdir(WISP_DATA "/cache", 0777);
    setenv("HOME", WISP_PROFILE, 1);
    setenv("XDG_DATA_HOME", WISP_PROFILE "/data", 1);
    setenv("XDG_CACHE_HOME", WISP_PROFILE "/cache", 1);
    setenv("XDG_CONFIG_HOME", WISP_PROFILE "/config", 1);
    setenv("XDG_RUNTIME_DIR", WISP_DATA "/run", 1);
    setenv("TMPDIR", WISP_DATA "/tmp", 1);
    setenv("GIO_USE_VFS", "local", 1);
    setenv("GIO_MODULE_DIR", "/app0/wisp/no-gio-modules", 1);
    setenv("GSETTINGS_BACKEND", "memory", 1);
    setenv("ICU_DATA", "/app0/wisp", 1);
    setenv("FONTCONFIG_FILE", "/app0/wisp/fonts.conf", 1);
    setenv("SSL_CERT_FILE", "/app0/wisp/cacert.pem", 1);
    // JavaScriptCore's thread-suspension signal off the kit's SIGUSR1 (thread dumps), and no
    // concurrent collector (as in the children: child/child_main.cpp)
    setenv("JSC_SIGNAL_FOR_GC", "29", 1);
    setenv("JSC_useConcurrentGC", wisp_setting_int("concurrent_gc", 0) ? "true" : "false", 1);
    // no WebAssembly here (pages run in the web process; child/child_main.cpp), and no fault handler
    setenv("JSC_useWasm", "false", 1);
    setenv("JSC_useWasmFaultSignalHandler", "false", 1);
    // (v00.38) COEP credentialless isolates, here as in the children (WebKit patch 0037; settings:
    // coep_credentialless=0)
    setenv("WISP_COEP_CREDENTIALLESS", wisp_setting_int("coep_credentialless", 1) ? "1" : "0", 1);
    // WebKit's pages in direct memory, not the shared flexible pool (as in the children; settings:
    // webkit_pages_direct=0)
    if (!wisp_setting_int("webkit_pages_direct", 1))
        setenv("WISP_WEBKIT_PAGES", "flexible", 1);
    // the remote Web Inspector (Settings: inspector=1): a WebKitGTK MiniBrowser on the PC opens
    // inspector://<console>:9222; any browser opens http://<console>:9223
    if (wisp_setting_int("inspector", 0)) {
        setenv("WEBKIT_INSPECTOR_SERVER", "0.0.0.0:9222", 1);
        setenv("WEBKIT_INSPECTOR_HTTP_SERVER", "0.0.0.0:9223", 1);
        wisp_log("web inspector on ports 9222 (inspector://) and 9223 (http://)");
    }
    wisp_memory_set_budget(static_cast<size_t>(512) << 20);
    wisp_log_capture_glib();
    wisp_memory_watch_start();
    // (v00.44) the kit's hot-spot profiler, as in the web process (settings: profiler, off by default from v1.00):
    // kit-ui/profile.log
    if (wisp_setting_int("profiler", 0))
        ps4_hot_profiler_set_enabled(1);
    wisp_icu_setup();
    // pages' sound plays from here (v00.35, ui/audio_out.cpp; settings: audio_out=web brings back the web
    // process's own port), set up before the first web process starts
    wisp_audio_out_start();
    // JavaScriptCore runs here too (extension messages' values): interpreter only, no executable
    // memory reserved
    jsc_options_set_boolean(JSC_OPTIONS_USE_JIT, FALSE);
    wisp::browser_run();
    return 0;
}
