// Wisp: main() of WebKit's web process (wisp-web.bin, WISP_CHILD_WEB; wisp-web-gpu.bin with
// WISP_WEB_GPU too) and network process (wisp-net.bin, WISP_CHILD_NETWORK). The UI starts them as
// local processes of the app (common/wisp_handoff.cpp); here they take WebKit's IPC socket from the
// handoff and run WebKit's own process main, as its WPEWebProcess / WPENetworkProcess executables would.
#include "../common/wisp.h"
#include "../common/wisp_audio.h"

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <gio/gio.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#if defined(WISP_CHILD_WEB)
#include <fontconfig/fontconfig.h>
#endif
#if defined(WISP_CHILD_WEB) && defined(WISP_HAVE_GSTREAMER)
#include <gst/gst.h>
#endif
#include <ps4_kit.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps4_kit_diag.h>

#if defined(WISP_WEB_GPU)
// (a folder of its own: the CPU web process's start would push the GPU one's traces out)
PS4_DIAG_DATA_DIR("/data/Wisp/kit-web-gpu");
#elif defined(WISP_CHILD_WEB)
PS4_DIAG_DATA_DIR("/data/Wisp/kit-web");   // the kit's crash log and boot trace of this process
#else
PS4_DIAG_DATA_DIR("/data/Wisp/kit-net");
#endif

namespace WebKit {
int WebProcessMain(int argc, char **argv);
int NetworkProcessMain(int argc, char **argv);
}

extern "C" {
int sceSysmoduleLoadModuleInternal(uint32_t id);
}

#if defined(WISP_CHILD_WEB) && !defined(WISP_WEB_GPU)
// CPU rendering (wisp-web.bin): no Mesa in this process. Mesa was ~23 MiB of the image, and every
// image comes out of the flexible memory pool the app's processes share. libepoxy still asks for EGL
// as WebKit looks for a surfaceless display: every name gets a function that fails (on x86-64 one
// returning 0 serves any signature: EGL_FALSE, EGL_NO_DISPLAY, NULL), eglGetError says
// EGL_NOT_INITIALIZED and eglQueryString an empty list, so WebKit finds no display and draws pages
// on the CPU, as it did when Mesa found no GPU.
namespace {
long gl_none() { return 0; }
int egl_not_initialized() { return 0x3001; }
const char *egl_no_strings(void *, int) { return ""; }
}
extern "C" void *wisp_gl_lookup(const char *name)
{
    if (!strcmp(name, "eglGetError"))
        return reinterpret_cast<void *>(egl_not_initialized);
    if (!strcmp(name, "eglQueryString"))
        return reinterpret_cast<void *>(egl_no_strings);
    return reinterpret_cast<void *>(gl_none);
}
#elif defined(WISP_CHILD_WEB)
extern "C" {
// Mesa (EGL + GLES on Zink on the kit's RADV), linked into this process only (wisp-web-gpu.bin)
void *eglGetProcAddress(const char *name);
// libepoxy's lookups (scripts/patch-epoxy.py): everything through EGL's
void *wisp_gl_lookup(const char *name) { return eglGetProcAddress(name); }
// ANGLE's native EGL for WebGL (v00.30, WebKit patch 0029: no dlopen of libEGL); the CPU web process
// keeps ANGLE's weak default, which finds nothing: no WebGL there
void *wisp_angle_egl_lookup(const char *name) { return eglGetProcAddress(name); }
// Zink's Vulkan: the kit's RADV (the OpenCE Halo port's Zink loader asks the app)
void *ps4kit_vk_get_instance_proc_addr(void);
void *zink_ps4_vk_get_instance_proc_addr(void) { return ps4kit_vk_get_instance_proc_addr(); }
// Gallium's DRI target refers to the KMS render-only driver, which needs a DRM device; the PS4 has none
struct pipe_screen;
struct pipe_screen *kmsro_drm_screen_create(int, const void *) { return nullptr; }
// C++ code reads Mesa's thread-local GL context (a C __thread variable, nothing to initialize) through
// a weakly referenced TLS init function; undefined, it would become an import (as in Etterna)
void _ZTH23_mesa_glapi_tls_Context(void) { }
}
#else
// the TLS backend (glib-networking's OpenSSL module, linked statically)
extern "C" void g_io_openssl_load(GIOModule *module);
#endif

// The heap: direct memory first; the web process gets room for pages, the network process less.
extern "C" void ps4_kit_configure_heap(ps4_kit_heap_options *options)
{
#if defined(WISP_CHILD_WEB)
    options->log_tag = "web";
    // 1.5 GiB of the app's ~4.5 GiB of direct memory (pages; v00.15's used up to ~270 MiB)
    options->direct_max = static_cast<size_t>(1536) << 20;
#else
    options->log_tag = "net";
    // 128 MiB (256 up to v00.35; it uses 5-20): the rest of direct memory is pages' (see the UI's)
    options->direct_max = static_cast<size_t>(128) << 20;
    // (v00.43) an overflow heap of 128 MiB at most (the kit's default is 512): in v00.42 one request
    // the main heap could not take, as Hot Pursuit 2 loaded, mapped 512 MiB of direct memory that stayed
    // for the rest of the run while the games after it were refused theirs. The kit's line now says
    // which request it was.
    options->overflow_direct_max = static_cast<size_t>(128) << 20;
#endif
    options->source_order = PS4_KIT_HEAP_DIRECT_FIRST;
    // thread stacks from the first thread on, before main (threads started that early got the kit's
    // 2 MiB: v00.12's census showed 2 MiB GLib threads in the network process)
    ps4_kit_set_min_thread_stack(static_cast<size_t>(256) << 10);
    ps4_kit_set_default_thread_stack(static_cast<size_t>(256) << 10);
    // and since v00.17 out of this heap (direct memory), not the flexible memory pool the processes
    // share (v00.15's web process: 62 threads, 25 MiB of it); settings: stacks_in_heap=0, read in main
    ps4_kit_set_thread_stacks_from_heap(1);
}

#if defined(WISP_CHILD_WEB)
// (v00.35) the UI's ring this process's sound goes to (common/wisp_audio.h; child/gst_ps4_audiosink.c writes it)
extern "C" {
WispAudioRing *wisp_audio_ring = nullptr;
}

// A web process's sound plays from the UI when START carried a ring (settings: audio_out, the UI's)
static void map_audio_ring()
{
    const int fd = wisp_child_audio_fd();
    if (fd < 0) {
        wisp_log("audio: this process plays its sound on an audio out port of its own (no ring from the UI)");
        return;
    }
    void *map = mmap(nullptr, WISP_AUDIO_RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    auto *ring = map == MAP_FAILED ? nullptr : static_cast<WispAudioRing *>(map);
    if (!ring || ring->magic != WISP_AUDIO_RING_MAGIC || ring->blocks != WISP_AUDIO_RING_BLOCKS) {
        wisp_log("audio: the UI's sound ring could not be used (%s): this process plays its sound itself",
            ring ? "not a ring" : strerror(errno));
        if (ring)
            munmap(map, WISP_AUDIO_RING_BYTES);
        return;
    }
    wisp_audio_ring = ring;
    wisp_log("audio: this process's sound goes to the UI, which plays it (a ring of %u blocks)", ring->blocks);
}

// JavaScriptCore's JIT (v00.27 on: this process links WebKit's JIT build, app/CMakeLists.txt): the
// Baseline and DFG tiers, and the regular expression and CSS selector JITs with them. Until v00.26 every
// script ran on the LLInt interpreter, and Cloudflare's check took minutes (v00.24: 136 s, the main
// thread stopped 9 s at a time). Its code goes in executable memory the kit finds on this console
// (ps4_exec_probe: system flexible memory on the consoles tested, outside the pool the app's processes
// share; the boot trace says what it tried), which WebKit maps whole when the first page's scripts start
// (OSAllocatorPS4): 256 MiB (settings: jit_memory_mib; 64 until v00.44, 128 in v00.45, when Mortal Kombat's
// compiled code reached 96 MiB of it and every compile after failed), 24 MiB when the only kind that runs code here is the shared pool's. jit=0 (menu:
// JavaScript JIT) or a console that runs no generated code: the LLInt.
static std::string g_javascript = "the LLInt interpreter (built without JIT tiers)";

static void jit_environment()
{
#if defined(WISP_HAVE_JIT)
    if (!wisp_setting_int("jit", 1)) {
        setenv("JSC_useJIT", "false", 1);
        g_javascript = "the LLInt interpreter (JavaScript JIT off in the menu)";
        return;
    }
    const int source = ps4_exec_probe();
    if (source < 0) {
        setenv("JSC_useJIT", "false", 1);
        g_javascript = "the LLInt interpreter (this console runs no generated code: kit boot trace)";
        return;
    }
    // the kit's sources 1-3 (mmap, mmap + mprotect, named flexible memory) are the shared pool's
    const bool shared_pool = source >= 1 && source <= 3;
    int mib = std::clamp(wisp_setting_int("jit_memory_mib", shared_pool ? 24 : 256), 8, 512);
    // (v00.46) More than 128 MiB only if system flexible memory has it now: a reservation JavaScriptCore can't make
    // turns its JIT off for every page. The kit's mmap takes anonymous mappings of 1 MiB or more from there; this one
    // goes again at once.
    if (!shared_pool && mib > 128) {
        const size_t size = static_cast<size_t>(mib) << 20;
        void *probe = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (probe == MAP_FAILED) {
            wisp_log("jit: %d MiB of system flexible memory for compiled code is not there: 128 MiB", mib);
            mib = 128;
        } else
            munmap(probe, size);
    }
    char bytes[32];
    snprintf(bytes, sizeof bytes, "%llu", static_cast<unsigned long long>(mib) << 20);
    setenv("JSC_jitMemoryReservationSize", bytes, 1);
    // JIT code polls for VM traps (WebKit patch 0027 makes it the default here too): signal-based ones
    // would take SIGSEGV and SIGBUS from the kit's crash log
    setenv("JSC_usePollingTraps", "true", 1);
    // WebAssembly's JIT (v00.34, a build with it: scripts/build-webkit.sh): BBQ compiles a function once
    // IPInt has run it a while, on compiler threads; OMG (B3) compiles the hottest ones again, optimized.
    // (v00.42) OMG and the FTL (JavaScript's B3 tier) are on by default: v00.41's emulators ran on BBQ and
    // were unplayable (Star Fox's N64 core, Crash Bash's Babylon.js). B3 takes hundreds of MiB and seconds of
    // a Jaguar core for a game's largest functions, so those stay in the tiers below: half of OMG's default
    // candidate cost, a third of the FTL's. Settings: wasm_jit=bbq or off, js_ftl=0
    std::string wasm = "IPInt";
    bool ftl = false;
#if defined(WISP_HAVE_WASM_JIT)
    // (v00.46) OMG takes functions up to wasm_omg_max bytes, 256 KB: Mortal Kombat's largest is 156 KB, Star Fox 64's
    // 221 KB (v00.45's "wasm JIT:" lines; JavaScriptCore's own default is 100 KB, v00.42-v00.44 had 50). B3 here
    // allocates registers greedily (no graph colouring), and the compilers run below the pages' priority.
    {
        char cost[24];
        snprintf(cost, sizeof cost, "%d", std::clamp(wisp_setting_int("wasm_omg_max", 256000), 10000, 4000000));
        setenv("JSC_maximumOMGCandidateCost", cost, 1);
    }
    // (v00.45) WebKit's compiler threads (six for WebAssembly, the JavaScript JIT's) a step below the default
    // priority: the PS4 gives threads of one priority no turns, so at the default a compile burst held every core
    // and a game's thread waited behind it; below it they use the cores nothing else wants (kit:
    // ps4_kit_set_thread_priority_by_name, applied as a thread names itself; WebKit patch 0050 names them).
    // Settings: compiler_priority (0: the default priority)
    if (const int priority = wisp_setting_int("compiler_priority", 720); priority > 0) {
        ps4_kit_set_thread_priority_by_name("Wasm Worklist Helper Thread", priority);
        ps4_kit_set_thread_priority_by_name("JIT Worklist Helper Thread", priority);
    }
    setenv("JSC_maximumFTLCandidateBytecodeCost", "20000", 1);
    const std::string wasm_jit = wisp_setting("wasm_jit", "omg");
    if (wasm_jit == "off") {
        setenv("JSC_useBBQJIT", "false", 1);
        setenv("JSC_useOMGJIT", "false", 1);
        wasm = "IPInt (WebAssembly JIT off in the settings)";
    } else if (wasm_jit == "omg") {
        wasm = "IPInt, then BBQ, then OMG";
    } else {
        setenv("JSC_useOMGJIT", "false", 1);
        wasm = "IPInt, then BBQ";
    }
    ftl = wisp_setting_int("js_ftl", 1) != 0;
    if (!ftl)
        setenv("JSC_useFTLJIT", "false", 1);
#endif
    g_javascript = std::string("its JIT (Baseline and DFG tiers") + (ftl ? ", FTL" : "") +
        "; WebAssembly by " + wasm + "), code in " + std::to_string(mib) + " MiB of " + ps4_kit_exec_source();
#else
    setenv("JSC_useJIT", "false", 1);
#endif
}

// JavaScriptCore's concurrent garbage collector (v00.33): it marks beside the page and stops the
// JavaScript thread only for moments, where the plain collector stops it for the whole collection (the
// hitches of a page with a large heap). It stops a thread with a signal (JSC_SIGNAL_FOR_GC): the thread's
// handler posts a semaphore and waits in sigsuspend until the same signal comes again (WTF's
// ThreadingPOSIX.cpp; the semaphores and signals are the PS4 kernel library's). The same steps on a
// thread of our own first, with deadlines (nothing here waits for good): concurrent_gc=auto, the default,
// turns the collector on when they work; 1 on unchecked, 0 off. run/concurrent-gc.txt tells the UI, whose
// fallback turns the collector off for the rest of a run when the web process keeps ending.
namespace gc_check {

constexpr int SIGNAL = 29;   // SIGINFO (JSC_SIGNAL_FOR_GC; the kit's thread dumps use SIGUSR1)
sem_t posted;
std::atomic<int> stage { 0 };
std::atomic<unsigned long> spins { 0 };
std::atomic<bool> finished { false };

void handler(int, siginfo_t *, void *)
{
    if (stage.load() != 1)
        return;   // the second signal: sigsuspend below returns after it
    stage.store(2);
    sem_post(&posted);
    sigset_t allowed;
    sigfillset(&allowed);
    sigdelset(&allowed, SIGNAL);
    sigsuspend(&allowed);
    stage.store(4);
    sem_post(&posted);
}

void *spinner(void *)
{
    while (!finished.load(std::memory_order_relaxed))
        spins.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// the semaphore posted within `ms` (polled)
bool posted_within(int ms)
{
    const double until = wisp_seconds() + ms / 1000.0;
    do {
        if (!sem_trywait(&posted))
            return true;
        usleep(100);
    } while (wisp_seconds() < until);
    return false;
}

// the spinning thread moved within `ms`
bool spinning_within(int ms)
{
    const unsigned long before = spins.load();
    const double until = wisp_seconds() + ms / 1000.0;
    do {
        usleep(200);
        if (spins.load() != before)
            return true;
    } while (wisp_seconds() < until);
    return false;
}

// nullptr when the steps work, else what went wrong
const char *run()
{
    if (sem_init(&posted, 0, 0))
        return "no semaphore";
    struct sigaction action {}, previous {};
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGNAL);
    action.sa_sigaction = handler;
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    if (sigaction(SIGNAL, &action, &previous))
        return "no signal handler";
    pthread_t thread;
    if (pthread_create(&thread, nullptr, spinner, nullptr)) {
        sigaction(SIGNAL, &previous, nullptr);
        return "no thread";
    }
    const char *failure = nullptr;
    if (!spinning_within(500))
        failure = "the test thread did not run";
    else {
        stage.store(1);
        if (pthread_kill(thread, SIGNAL) || !posted_within(500) || stage.load() != 2)
            failure = "a thread did not stop on the signal";
        else {
            const unsigned long held = spins.load();
            usleep(3000);
            if (spins.load() != held)
                failure = "a stopped thread kept running";
            stage.store(3);
            if (pthread_kill(thread, SIGNAL) || !posted_within(500) || stage.load() != 4)
                failure = "a stopped thread did not resume on the signal";
            else if (!failure && !spinning_within(500))
                failure = "a resumed thread did not run again";
        }
    }
    finished.store(true);
    if (failure) {
        // (the thread may be held in the handler: it stays as it is, and so does the handler)
        pthread_detach(thread);
        return failure;
    }
    pthread_join(thread, nullptr);
    sigaction(SIGNAL, &previous, nullptr);
    sem_destroy(&posted);
    return nullptr;
}

} // namespace gc_check

static void concurrent_gc_environment()
{
    const std::string wanted = wisp_setting("concurrent_gc", "auto");
    bool on = false;
    if (wanted == "0")
        wisp_log("gc: the plain collector (settings: concurrent_gc=0)");
    else if (wanted == "1") {
        on = true;
        wisp_log("gc: JavaScriptCore's concurrent collector (settings: concurrent_gc=1, not checked)");
    } else {
        const double start = wisp_seconds();
        const char *failure = gc_check::run();
        on = !failure;
        if (on)
            wisp_log("gc: JavaScriptCore's concurrent collector (a thread stops and resumes on signal %d here: checked in %.1f ms)",
                gc_check::SIGNAL, (wisp_seconds() - start) * 1000);
        else
            wisp_log("gc: the plain collector: %s (signal %d, %.0f ms), which the concurrent one needs", failure, gc_check::SIGNAL,
                (wisp_seconds() - start) * 1000);
    }
    setenv("JSC_useConcurrentGC", on ? "true" : "false", 1);
    if (FILE *f = fopen(WISP_DATA "/run/concurrent-gc.txt", "w")) {
        fputs(on ? "1\n" : "0\n", f);
        fclose(f);
    }
}
#endif

static void set_environment()
{
    // A local process inherits no environment.
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
    setenv("LANG", "en_US.UTF-8", 1);
    // WebKit's IPC lines in this process's log (WebKit patch: messages that wait for room on a busy
    // socket, a message lost); settings: ipc_log=0 turns them off
    setenv("WISP_IPC_LOG", wisp_setting_int("ipc_log", 1) ? "1" : "0", 1);
    // (v00.38) COEP credentialless makes a page cross-origin isolated, as require-corp (WebKit patch 0037;
    // both processes parse the header); a page's failed loads in web.log (patch 0038). Settings:
    // coep_credentialless=0, load_log=0
    setenv("WISP_COEP_CREDENTIALLESS", wisp_setting_int("coep_credentialless", 1) ? "1" : "0", 1);
    setenv("WISP_LOAD_LOG", wisp_setting_int("load_log", 1) ? "1" : "0", 1);
    // WebKit's pages (JavaScriptCore's structure heap and the like) in direct memory since v00.17, not
    // the flexible memory pool the processes share (WebKit patch: WTF's OSAllocatorPS4.cpp); settings:
    // webkit_pages_direct=0 brings flexible memory back
    if (!wisp_setting_int("webkit_pages_direct", 1))
        setenv("WISP_WEBKIT_PAGES", "flexible", 1);
    // JavaScriptCore suspends threads with a signal (SIGUSR1 by default, which the kit's thread dumps
    // use: SIGINFO here); its concurrent collector does, in the web process when the console passes the
    // check above (concurrent_gc_environment); the network process runs no JavaScript
    setenv("JSC_SIGNAL_FOR_GC", "29", 1);
#if defined(WISP_CHILD_WEB)
    concurrent_gc_environment();
    jit_environment();
#else
    setenv("JSC_useConcurrentGC", "false", 1);
    setenv("JSC_useJIT", "false", 1);  // (the network process's WebKit has no JIT tiers anyway)
#endif
    // WebAssembly (a WebKit built with it, v00.21 on): JavaScriptCore's in-place interpreter runs it. Each
    // memory is bounds-checked in place: no fault handler (the kit's crash log owns SIGSEGV and SIGBUS)
    // and no "fast memory" (4 GiB and more of address space reserved per memory). Settings: wasm=0
    // (menu: WebAssembly) takes it out of the pages.
    setenv("JSC_useWasmFaultSignalHandler", "false", 1);
    setenv("JSC_useWasmFastMemory", "false", 1);
    if (!wisp_setting_int("wasm", 1))
        setenv("JSC_useWasm", "false", 1);
    // WebAssembly threads (v00.33): SharedArrayBuffer in every page, and with it shared WebAssembly
    // memories, bounds-checked (WebKit patch 0033; there is no fault handler here). Chrome gives
    // SharedArrayBuffer to cross-origin isolated pages only because each of those gets a process of its
    // own; here one process serves every page, so the headers would guard nothing, and a page that checks
    // for SharedArrayBuffer alone gets its threads too. Settings: wasm_threads=0 (menu: WebAssembly threads)
    if (wisp_setting_int("wasm_threads", 1))
        setenv("JSC_useSharedArrayBuffer", "true", 1);
    // a page leaving this process (its tab closed or asleep): the caches let go of what it used and a full garbage
    // collection follows (WebKit patch 0025); memory_release=0 leaves that to WebKit's memory monitor alone
    setenv("WISP_MEMORY_RELEASE", wisp_setting_int("memory_release", 1) ? "1" : "0", 1);
#if defined(WISP_WEB_GPU)
    // Mesa: EGL's surfaceless platform on Zink. Zink runs without a DRM device through Kopper (Wisp's
    // Mesa patch: surfaceless EGL tries it when no DRM device answers), so Kopper must stay on; EGL
    // says in web.log how its start went.
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
    setenv("MESA_SHADER_CACHE_DIR", WISP_DATA "/cache/mesa", 1);
    setenv("EGL_LOG_LEVEL", wisp_setting_int("gpu_log", 1) ? "debug" : "warning", 1);
    // Wisp's Zink guards against freed buffers (v00.24) say in web.log when they catch one; same switch
    setenv("WISP_ZINK_LOG", wisp_setting_int("gpu_log", 1) ? "1" : "0", 1);
    // no GL thread per context (Mesa's glthread: a thread each, and WebKit makes about ten contexts);
    // Gallium's threaded context keeps its one
    setenv("mesa_glthread", "false", 1);
    // CPU + GPU (rendering=hybrid, the default): Skia draws the page's tiles on the CPU, on painting
    // threads, and the GPU composites, scrolls and animates them (WebKit's CPU tile rendering, as WPE
    // runs on boards with weak GPUs); GPU: Skia draws on GL too
    setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", wisp_rendering() == WispRendering::Gpu ? "0" : "1", 1);
    // WebGPU for pages (v00.40): Dawn's errors a page does not catch, and a device it lost, in web.log
    // ("WebGPU (Dawn): ..."; WebKit patch 0039); settings: webgpu_log=0
    setenv("WISP_WEBGPU_LOG", wisp_setting_int("webgpu_log", 1) ? "1" : "0", 1);
#endif
#if defined(WISP_CHILD_WEB) && defined(WISP_HAVE_GSTREAMER)
    // GStreamer (<video>, <audio>, Media Source, Web Audio): every plugin is in the program (no registry
    // to scan, no helper process); its warnings in logs/gstreamer.log (settings: gst_log=0 turns them
    // off, gst_log=4 says much more)
    setenv("GST_REGISTRY_FORK", "no", 1);
    setenv("GST_REGISTRY_UPDATE", "no", 1);
    // the audio sink's lines (child/gst_ps4_audiosink.c, v00.34: each player's start, the port's state and,
    // every 10 s while it plays, its pace and loudness and each player's); settings: audio_log=0
    setenv("WISP_AUDIO_LOG", wisp_setting_int("audio_log", 1) ? "1" : "0", 1);
    setenv("GST_PLUGIN_SYSTEM_PATH_1_0", "", 1);
    setenv("GST_PLUGIN_PATH_1_0", "", 1);
    if (const int level = std::clamp(wisp_setting_int("gst_log", 2), 0, 9)) {
        char value[8];
        snprintf(value, sizeof value, "%d", level);
        setenv("GST_DEBUG", value, 1);
        setenv("GST_DEBUG_FILE", WISP_LOGS "/gstreamer.log", 1);
        setenv("GST_DEBUG_NO_COLOR", "1", 1);
    }
    // a progressive video's download buffer (WebKit's downloadbuffer) on disk where the app may write: WPE
    // puts it in /var/tmp, which the PS4 lacks (v00.22: "Could not create temp file
    // /var/tmp/WebKit-Media-XXXXXX", and YouTube's previews and other sites' plain <video> stopped)
    mkdir(WISP_DATA "/cache", 0777);
    mkdir(WISP_DATA "/cache/media", 0777);
    setenv("WPE_SHELL_MEDIA_DISK_CACHE_PATH", WISP_DATA "/cache/media", 1);
#endif
#if defined(WISP_CHILD_WEB)
    // the composited path's painting threads (tiles: CPU + GPU)
    char threads[8];
    snprintf(threads, sizeof threads, "%d", std::clamp(wisp_setting_int("paint_threads", 4), 1, 8));
    setenv("WEBKIT_SKIA_CPU_PAINTING_THREADS", threads, 1);
    // CPU rendering's whole-frame strips (WebKit patch 0014): off unless asked for (paint_strips=4):
    // v00.12's web process stopped answering while a page was typed into, the strips the new suspect
    char strips[8];
    snprintf(strips, sizeof strips, "%d", std::clamp(wisp_setting_int("paint_strips", 1), 1, 8));
    setenv("WISP_PAINT_STRIPS", strips, 1);
#endif
}

#if defined(WISP_CHILD_WEB)
// fontconfig up before WebKit asks it: the first start after an install reads every packaged font
// (~200 files) for its cache (/data/Wisp/cache/fontconfig), later starts read the cache; web.log says
// which and how long it took
static void load_fonts()
{
    const double start = wisp_seconds();
    const size_t before = wisp_flexible_free();
    if (!FcInit()) {
        wisp_log("fonts: fontconfig did not start (%s): pages may show boxes for most scripts", getenv("FONTCONFIG_FILE"));
        return;
    }
    FcFontSet *fonts = FcConfigGetFonts(nullptr, FcSetSystem);
    wisp_log("fonts: %d faces ready in %.2f s; flexible memory %zu -> %zu MiB", fonts ? fonts->nfont : 0,
        wisp_seconds() - start, before >> 20, wisp_flexible_free() >> 20);
}
#endif

#if defined(WISP_CHILD_WEB) && defined(WISP_HAVE_GSTREAMER)
extern "C" void gst_plugin_ps4audio_register(void);

// GStreamer up before WebKit asks for it (WebKit's own start then finds it running and adds its
// elements), with the PS4's audio sink among its plugins (child/gst_ps4_audiosink.c). The audio out's
// and the user service's modules first: a call into a module that is not loaded ends the process
// without a word.
static void start_media()
{
    const double start = wisp_seconds();
    sceSysmoduleLoadModuleInternal(0x80000001);  // AudioOut
    sceSysmoduleLoadModuleInternal(0x80000011);  // UserService
    GError *error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        wisp_log("media: GStreamer did not start (%s): pages get no video or sound", error ? error->message : "no reason given");
        g_clear_error(&error);
        return;
    }
    gst_plugin_ps4audio_register();
    GList *plugins = gst_registry_get_plugin_list(gst_registry_get());
    const unsigned count = g_list_length(plugins);
    gst_plugin_list_free(plugins);
    guint major = 0, minor = 0, micro = 0, nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    wisp_log("media: GStreamer %u.%u.%u with %u plugins (the PS4's audio out among them) in %.2f s", major, minor, micro,
        count, wisp_seconds() - start);
}
#endif

#if defined(WISP_WEB_GPU)
// EGL (Mesa's, linked in), declared here: the values are EGL's
extern "C" {
void *eglGetPlatformDisplay(unsigned int platform, void *native_display, const intptr_t *attributes);
unsigned int eglInitialize(void *display, int *major, int *minor);
int eglGetError(void);
const char *eglQueryString(void *display, int name);
}

// GPU rendering: start EGL on Zink before WebKit does (WebKit then gets the same display), and say how
// it went. Without it WebKit draws on the CPU (WebKit patch, DrawingAreaCoordinatedGraphicsGLib).
static void probe_gl()
{
    const double start = wisp_seconds();
    const size_t before = wisp_flexible_free();
    void *display = eglGetPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
    int major = 0, minor = 0;
    if (!display || !eglInitialize(display, &major, &minor)) {
        wisp_log("gpu: EGL on Zink did not start (EGL error %#x, %.2f s): this web process draws on the CPU",
            eglGetError(), wisp_seconds() - start);
        return;
    }
    const char *vendor = eglQueryString(display, 0x3053 /* EGL_VENDOR */);
    const char *apis = eglQueryString(display, 0x308D /* EGL_CLIENT_APIS */);
    wisp_log("gpu: EGL %d.%d (%s; %s) on Zink in %.2f s; flexible memory %zu -> %zu MiB", major, minor,
        vendor ? vendor : "?", apis ? apis : "?", wisp_seconds() - start, before >> 20, wisp_flexible_free() >> 20);
}

// (v00.37) Mesa's locks and fences wait on the kernel's umtx now (Wisp's Mesa: util/futex.c, libkernel's
// _umtx_op) instead of being pthread objects, which the PS4 makes from a small pool in each process (v00.36's
// Star Fox 64 emptied it). Checked once before Mesa starts: a wait on a stale value returns at once, a wait
// with a deadline ends on time, a wake ends a wait. web.log says how it went.
extern "C" int futex_wait(uint32_t *addr, int32_t value, const struct timespec *timeout);
extern "C" int futex_wake(uint32_t *addr, int32_t count);

namespace {
uint32_t futex_word;

void *futex_waker(void *)
{
    const timespec pause = { 0, 10000000 };
    nanosleep(&pause, nullptr);
    __atomic_store_n(&futex_word, 1, __ATOMIC_SEQ_CST);
    futex_wake(&futex_word, 1);
    return nullptr;
}
}

static void check_futex()
{
    uint32_t other = 5;
    double t0 = wisp_seconds();
    const int stale = futex_wait(&other, 4, nullptr);
    const double stale_ms = (wisp_seconds() - t0) * 1000;

    uint32_t idle = 0;
    timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += 20000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    t0 = wisp_seconds();
    const int timed = futex_wait(&idle, 0, &deadline);
    const double timed_ms = (wisp_seconds() - t0) * 1000;

    __atomic_store_n(&futex_word, 0, __ATOMIC_SEQ_CST);
    pthread_t waker;
    unsigned waits = 0;
    double woke_ms = -1;
    if (!pthread_create(&waker, nullptr, futex_waker, nullptr)) {
        timespec net;
        clock_gettime(CLOCK_MONOTONIC, &net);
        net.tv_sec += 2;
        t0 = wisp_seconds();
        while (__atomic_load_n(&futex_word, __ATOMIC_SEQ_CST) == 0 && wisp_seconds() - t0 < 2) {
            futex_wait(&futex_word, 0, &net);
            waits++;
        }
        woke_ms = (wisp_seconds() - t0) * 1000;
        pthread_join(waker, nullptr);
    }
    // a working wait sleeps until the wake: one or two waits; a wait that returns at once spins thousands
    const bool works = stale_ms < 5 && timed_ms >= 15 && timed_ms < 500 && woke_ms >= 0 && woke_ms < 1000 && waits <= 4;
    wisp_log("futex (Mesa's locks, on the kernel's umtx): %s: a stale value returned %d in %.2f ms; a 20 ms deadline "
        "returned %d after %.1f ms; a wake ended the wait after %.1f ms (%u wait%s)",
        works ? "works" : "DOES NOT WORK AS EXPECTED (Mesa's locks will spin)", stale, stale_ms, timed, timed_ms,
        woke_ms, waits, waits == 1 ? "" : "s");
}
#endif

#if defined(WISP_HAVE_DAWN)
void wisp_webgpu_selftest_start();   // (child/webgpu_selftest.cpp: the GPU web process with Dawn)
#endif
#if defined(WISP_CHILD_WEB)
extern "C" void wisp_kobj_census_start(void);   // (child/kobj_census.cpp, v00.41)
#endif

#if defined(WISP_HAVE_WEBGPU)
// WebGPU for pages (v00.40): WebCore asks this before it makes a page's navigator.gpu (WebKit patch 0039,
// WebGPUCreateImpl.cpp; its weak definition says no in the CPU web process). Dawn then runs on the kit's
// RADV beside Zink, as the self-test does. Settings: webgpu=0 (Settings > Drawing; the UI turns WebKit's
// feature off with it, so pages do not see navigator.gpu at all)
extern "C" bool wisp_webgpu_usable(void)
{
    static const bool usable = [] {
        const bool on = wisp_setting_int("webgpu", 1) != 0;
        wisp_log("webgpu: a page asked for WebGPU: %s", on ? "Dawn on the kit's RADV (Vulkan)" : "off in Settings (webgpu=0)");
        return on;
    }();
    return usable;
}
#endif

int main(int, char **)
{
#if defined(WISP_CHILD_WEB)
    const WispProcess role = WispProcess::Web;
    const char *name = "WPEWebProcess";
#else
    const WispProcess role = WispProcess::Network;
    const char *name = "WPENetworkProcess";
#endif
    // exit() here ends this process only (the kit's default closes the whole app)
    ps4_kit_set_exit_ends_process_only(1);
    // Thread stacks come out of the flexible memory pool the app's processes share (35-50 MiB left
    // with all three running; v00.06's web process died when its garbage collector could not start a
    // helper thread). The kit's minimum, 2 MiB a thread, is for GPU drivers' shader compilers;
    // WebKit's threads need far less: Sony's PlayStation port runs them on the system's 64 KiB, and
    // JavaScript threads ask for 512 KiB themselves (WTF's stackSize). Mesa's keep the kit's.
    // GPU rendering too (v00.10 gave every thread there 512 KiB and Mesa's 2 MiB: 100 threads emptied
    // the pool in a second and the web process aborted): the threads that draw with GL ask for more
    // themselves, the compositor and the scrolling thread 1 MiB (WebKit patch, WTF's Graphics
    // threads), Mesa's own 1 MiB (Wisp's Mesa patch, c11 thrd_create)
    ps4_kit_set_min_thread_stack(static_cast<size_t>(256) << 10);
    if (!wisp_setting_int("stacks_in_heap", 1))
        ps4_kit_set_thread_stacks_from_heap(0);
    // what WebKit's memory pressure handler measures the heap against (its caches go near it)
#if defined(WISP_CHILD_WEB)
    wisp_memory_set_budget(static_cast<size_t>(1536) << 20);
#else
    wisp_memory_set_budget(static_cast<size_t>(128) << 20);
#endif
    wisp_log_open(role);
    sceSysmoduleLoadModuleInternal(0x8000001C);  // libSceNet (DNS, sockets)
    sceSysmoduleLoadModuleInternal(0x80000010);  // SystemService
    char heap[160];
    ps4rt_heap_report(heap, sizeof heap);
    wisp_log("heap: %s (%s)", heap, ps4rt_heap_source());

    WispStart start {};
    int ipc_fd = -1;
    if (!wisp_child_handoff(&start, &ipc_fd)) {
        wisp_log("no handoff from the UI: ending");
        _exit(1);
    }
    wisp_log("handoff: WebKit process id %llu, IPC socket %d", static_cast<unsigned long long>(start.process_id), ipc_fd);
#if defined(WISP_CHILD_WEB)
    map_audio_ring();
#endif
#if defined(WISP_HAVE_DAWN)
    // the WebGPU self-test (v00.31, menu: WebGPU test): Dawn beside Zink, on a thread, a few seconds in
    if (start.log_flags & WISP_CHILD_WEBGPU_SELFTEST)
        wisp_webgpu_selftest_start();
#endif
    mkdir(WISP_PROFILE, 0777);
    mkdir(WISP_DATA "/tmp", 0777);
    mkdir(WISP_DATA "/run", 0777);
    mkdir(WISP_DATA "/cache", 0777);
    set_environment();
    wisp_log_capture_glib();
    wisp_memory_watch_start();
#if defined(WISP_CHILD_WEB)
    wisp_kernel_objects_selftest();
    // (v00.41) which calls take the PS4's internal memory: "kobj:" lines in web.log (settings: kobj_census)
    wisp_kobj_census_start();
#endif
    wisp_icu_setup();
#if defined(WISP_CHILD_WEB)
    wisp_log("JavaScriptCore: %s; %s rendering", g_javascript.c_str(),
#if defined(WISP_WEB_GPU)
        "GPU (Mesa)"
#else
        "CPU (no Mesa in this process)"
#endif
    );
    // (the About page says it too: ui/browser.cpp reads it)
    if (FILE *f = fopen(WISP_DATA "/run/javascript.txt", "w")) {
        fprintf(f, "%s\n", g_javascript.c_str());
        fclose(f);
    }
    load_fonts();
#if defined(WISP_HAVE_GSTREAMER)
    start_media();
#endif
#if defined(WISP_WEB_GPU)
    check_futex();
    probe_gl();
#endif
#else
    g_io_openssl_load(nullptr);
#endif

    char id[24], fd[16];
    snprintf(id, sizeof id, "%llu", static_cast<unsigned long long>(start.process_id));
    snprintf(fd, sizeof fd, "%d", ipc_fd);
    char *argv[] = { const_cast<char *>(name), id, fd, nullptr };
    wisp_log("%s: starting", name);
    // (WebKit's main run loop runs GLib's default context, where the watchdog's beat goes)
    wisp_watchdog_beat_start();
#if defined(WISP_CHILD_WEB)
    // and the stall log's tick (v00.33: what a page's hitches are made of; settings: stall_log)
    wisp_stall_log_start();
    // (v00.44) the kit's hot-spot profiler: every 10 s each busy thread's busiest code, in this process's kit
    // folder (kit-web-gpu/profile.log, the last run's as profile.prev.log; WebKit patch 0050 gives the threads
    // their names there). Settings: profiler (off by default from v1.00: it looks at every thread through signals)
    if (wisp_setting_int("profiler", 0))
        ps4_hot_profiler_set_enabled(1);
#endif
#if defined(WISP_CHILD_WEB)
    const int code = WebKit::WebProcessMain(3, argv);
#else
    const int code = WebKit::NetworkProcessMain(3, argv);
#endif
    wisp_log("%s: ended (%d)", name, code);
    _exit(code);
}
