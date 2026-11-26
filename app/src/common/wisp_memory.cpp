// Wisp: the memory figures WebKit asks for (WTF/wtf/ps4/MemoryPS4.cpp in the WebKit patches):
// what this process has allocated from the kit's heaps, and the budget the app gives it.
#include "wisp.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <glib.h>
#include <pthread.h>
#include <ps4_kit.h>
#include <ps4_kit_diag.h>

namespace {
size_t budget = static_cast<size_t>(1024) << 20;
}

void wisp_memory_set_budget(size_t bytes)
{
    budget = bytes;
}

extern "C" size_t wisp_memory_footprint(void)
{
    return ps4_heap_in_use_mib(nullptr) << 20;
}

extern "C" size_t wisp_memory_budget(void)
{
    return budget;
}

extern "C" int sceKernelAvailableFlexibleMemorySize(size_t *size);
// (another name: OpenOrbis' headers may declare it)
extern "C" size_t wisp_kernel_direct_memory_size(void) __asm__("sceKernelGetDirectMemorySize");

// the app's direct memory in all (the heaps, the GPU's memory: about 4.5 GiB)
size_t wisp_direct_memory_size(void)
{
    return wisp_kernel_direct_memory_size();
}
// WebKit's pages in direct memory now, and what WTF's allocator holds (WebKit patch 0021)
extern "C" size_t wtf_ps4_page_memory(size_t *held) __attribute__((weak));
// (v00.44) the web process's frame counters (WebKit patches 0050, 0051: WTF/wtf/ps4/MemoryPS4.cpp)
extern "C" unsigned long long wisp_frame_counters[8] __attribute__((weak));
// (v00.45) WebAssembly's tiers and the JIT's code memory (WebKit patch 0054)
extern "C" unsigned long long wisp_jit_counters[8] __attribute__((weak));
// (a weak definition, not a weak reference: the network process's WebKit has no JIT and no such function, and
// the PS4's packager refuses a weak symbol left undefined; JavaScriptCore's own wins where it exists)
extern "C" __attribute__((weak)) size_t wisp_jit_code_bytes(size_t *region)
{
    if (region)
        *region = 0;
    return 0;
}

size_t wisp_flexible_free(void)
{
    size_t size = 0;
    return sceKernelAvailableFlexibleMemorySize(&size) ? 0 : size;
}

// (v00.37) The PS4's internal pool in this process: the kernel library makes every thread, mutex, condition
// variable, read-write lock and semaphore from it. When it runs out the log says "[ScePthread/System] Internal
// Memory is running out", pthread_mutex_init and pthread_create fail, and GLib ends the process (v00.36:
// GStreamer's clock, during Star Fox 64; Mesa had made all its locks pthread objects). SIZE_MAX: no answer.
extern "C" int sceKernelInternalMemoryGetAvailableSize(size_t *size);

size_t wisp_internal_memory_free(void)
{
    size_t size = 0;
    return sceKernelInternalMemoryGetAvailableSize(&size) ? SIZE_MAX : size;
}

// (v00.38) What making and destroying each kind of kernel object costs that pool, once as the web process
// starts (memory_log): v00.37's Star Fox 64 lost ~550 bytes of it for every Web Audio buffer, whose clock
// wait made and destroyed a condition variable (with an attribute object) and a mutex. A kind whose cost
// stays above 0 is not given back when destroyed, and every such object made in a loop drains the pool.
// (Attribute objects get 16 bytes: the kernel library writes an 8-byte pointer, OpenOrbis' musl headers
// give them 4.)
void wisp_kernel_objects_selftest(void)
{
    if (!wisp_setting_int("memory_log", 1) || wisp_internal_memory_free() == SIZE_MAX)
        return;
    std::string line;
    auto measure = [&line](const char *what, int cycles, auto &&make_and_destroy) {
        const size_t before = wisp_internal_memory_free();
        for (int i = 0; i < cycles; i++)
            make_and_destroy();
        const size_t after = wisp_internal_memory_free();
        const long long lost = static_cast<long long>(before) - static_cast<long long>(after);
        char part[160];
        snprintf(part, sizeof part, "%s%s %lld bytes (%d made and destroyed)", line.empty() ? "" : "; ", what,
            lost / cycles, cycles);
        line += part;
    };
    measure("mutex", 64, [] {
        pthread_mutex_t mutex;
        pthread_mutex_init(&mutex, nullptr);
        pthread_mutex_lock(&mutex);
        pthread_mutex_unlock(&mutex);
        pthread_mutex_destroy(&mutex);
    });
    measure("condition variable", 64, [] {
        pthread_cond_t cond;
        pthread_cond_init(&cond, nullptr);
        pthread_cond_signal(&cond);
        pthread_cond_destroy(&cond);
    });
    measure("condition variable on the monotonic clock (attribute object + condition variable)", 64, [] {
        alignas(8) unsigned char attr[16] = {};
        auto *cond_attr = reinterpret_cast<pthread_condattr_t *>(attr);
        pthread_condattr_init(cond_attr);
        pthread_condattr_setclock(cond_attr, CLOCK_MONOTONIC);
        pthread_cond_t cond;
        pthread_cond_init(&cond, cond_attr);
        pthread_condattr_destroy(cond_attr);
        pthread_cond_destroy(&cond);
    });
    measure("recursive mutex (attribute object + mutex)", 64, [] {
        alignas(8) unsigned char attr[16] = {};
        auto *mutex_attr = reinterpret_cast<pthread_mutexattr_t *>(attr);
        pthread_mutexattr_init(mutex_attr);
        pthread_mutexattr_settype(mutex_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_t mutex;
        pthread_mutex_init(&mutex, mutex_attr);
        pthread_mutexattr_destroy(mutex_attr);
        pthread_mutex_lock(&mutex);
        pthread_mutex_unlock(&mutex);
        pthread_mutex_destroy(&mutex);
    });
    measure("GLib GMutex + GCond", 64, [] {
        GMutex mutex;
        GCond cond;
        g_mutex_init(&mutex);
        g_cond_init(&cond);
        g_mutex_lock(&mutex);
        g_cond_signal(&cond);
        g_mutex_unlock(&mutex);
        g_cond_clear(&cond);
        g_mutex_clear(&mutex);
    });
    measure("thread", 16, [] {
        pthread_t thread;
        if (!pthread_create(&thread, nullptr, [](void *) -> void * { return nullptr; }, nullptr))
            pthread_join(thread, nullptr);
    });
    wisp_log("memory: the PS4's internal memory each kernel object kept after it was destroyed: %s; %zu KiB left",
        line.c_str(), wisp_internal_memory_free() >> 10);
}

// --- What this process holds of the shared pool (from WinePS4's port/ps4_kstate.c) --------------------

// sceKernelVirtualQuery's record (72 bytes: shadPS4's OrbisVirtualQueryInfo)
struct WispVirtualQueryInfo {
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint8_t kind;   // 1 flexible, 2 direct, 4 stack, 8 pooled, 16 committed
    char name[32];
    uint8_t pad[7];
};
static_assert(sizeof(WispVirtualQueryInfo) == 72, "sceKernelVirtualQuery's record");

// (OpenOrbis declares it with its own record type)
extern "C" int wisp_virtual_query(const void *address, int flags, WispVirtualQueryInfo *info, size_t size)
    __asm__("sceKernelVirtualQuery");

void wisp_memory_census(WispMemoryCensus &census)
{
    census = WispMemoryCensus();
    struct Group {
        char name[32];
        uint64_t size;
        unsigned count;
        bool shared;
    } groups[48];
    int n = 0;
    const char *address = nullptr;
    WispVirtualQueryInfo info;
    for (int steps = 0; steps < 100000 && !wisp_virtual_query(address, 1 /* the next mapping */, &info, sizeof info); steps++) {
        if (info.end <= reinterpret_cast<uintptr_t>(address))
            break;
        address = reinterpret_cast<const char *>(static_cast<uintptr_t>(info.end));
        const uint64_t size = info.end > info.start ? info.end - info.start : 0;
        // flexible: every anonymous mapping and the kernel's, charged whole; shared: neither flexible
        // nor direct (shared memory, files), charged as touched
        const bool flexible = info.kind & 1;
        const bool shared = !flexible && !(info.kind & 2) && info.protection;
        if (flexible)
            census.flexible += size;
        else if (shared)
            census.shared += size;
        else
            continue;
        if (info.kind & 4) {   // a thread's stack: counted here, not by name (each has its own)
            census.stacks += size;
            census.stack_count++;
            census.stack_largest = std::max(census.stack_largest, size);
            continue;
        }
        info.name[sizeof info.name - 1] = 0;
        int i = 0;
        while (i < n && (groups[i].shared != shared || strcmp(groups[i].name, info.name)))
            i++;
        if (i == n) {
            if (n == static_cast<int>(sizeof groups / sizeof groups[0]))
                continue;
            memcpy(groups[n].name, info.name, sizeof groups[n].name);
            groups[n].size = 0;
            groups[n].count = 0;
            groups[n].shared = shared;
            n++;
        }
        groups[i].size += size;
        groups[i].count++;
    }
    // the largest five, by name
    size_t length = 0;
    for (int k = 0; k < 5 && k < n; k++) {
        int best = k;
        for (int j = k + 1; j < n; j++)
            if (groups[j].size > groups[best].size)
                best = j;
        std::swap(groups[k], groups[best]);
        const int wrote = snprintf(census.largest + length, sizeof census.largest - length, "%s'%s'%s %llu MiB x%u", k ? ", " : "",
            groups[k].name[0] ? groups[k].name : "(no name)", groups[k].shared ? " (shared)" : "",
            static_cast<unsigned long long>(groups[k].size >> 20), groups[k].count);
        if (wrote < 0 || static_cast<size_t>(wrote) >= sizeof census.largest - length)
            break;
        length += static_cast<size_t>(wrote);
    }
}

namespace {

bool memory_lines = true;
// the main loop's heartbeat (wisp_watchdog_beat_start): when it last ran; 0: not watched
std::atomic<double> last_beat{0};

gboolean beat(gpointer)
{
    last_beat.store(wisp_seconds(), std::memory_order_relaxed);
    return G_SOURCE_CONTINUE;
}

// The main thread has not run its loop for 8 s: every thread's stack into logs/<process>.hang.log
// (v00.12's web process stopped answering a typed page, reloads and new tabs with nothing said),
// again every 60 s while it stays so
void watchdog_check(double now)
{
    static double dumped_at = -1000;
    const double beat_time = last_beat.load(std::memory_order_relaxed);
    if (!beat_time || now - beat_time < 8 || now - dumped_at < 60)
        return;
    dumped_at = now;
    char path[96], reason[160];
    snprintf(path, sizeof path, WISP_LOGS "/%s.hang.log", wisp_process_name());
    snprintf(reason, sizeof reason, "OrbisWeb's watchdog: the %s process's main thread has not run its loop for %.0f s",
        wisp_process_name(), now - beat_time);
    wisp_log("watchdog: the main thread has not run its loop for %.0f s: every thread's stack in %s", now - beat_time, path);
    ps4_dump_thread_stacks(path, reason);
}

// Every second: a line when the shared pool moved by 8 MiB or more since the last one, and every 2 s
// while less than 48 MiB is left (a thread the system cannot start ends a process at once, and an
// empty pool freezes the app): what is left, what this process holds, its largest mappings; and the
// watchdog's check
void *memory_watch(void *)
{
    size_t last = SIZE_MAX;
    double last_time = -100;
    size_t internal_last = SIZE_MAX;
    double internal_time = -100;
    for (;;) {
        const size_t pool = wisp_flexible_free();
        const double now = wisp_seconds();
        watchdog_check(now);
        // (v00.37) the kernel library's internal pool: a line at the start, when it moved by 512 KiB, and every
        // 2 s while under 2 MiB
        const size_t internal = wisp_internal_memory_free();
        if (memory_lines && internal != SIZE_MAX) {
            const bool first = internal_last == SIZE_MAX;
            const bool shifted = !first &&
                (internal > internal_last ? internal - internal_last : internal_last - internal) >= (static_cast<size_t>(512) << 10);
            const bool scarce = internal < (static_cast<size_t>(2) << 20) && now - internal_time >= 2;
            if (first || shifted || scarce) {
                wisp_log("memory: %zu KiB of the PS4's internal memory left in this process (its threads, mutexes and "
                    "condition variables come from it)%s", internal >> 10,
                    internal < (static_cast<size_t>(2) << 20) ? ": LOW" : "");
                internal_last = internal;
                internal_time = now;
            }
        }
        const bool moved = last == SIZE_MAX || (pool > last ? pool - last : last - pool) >= (static_cast<size_t>(8) << 20);
        const bool low = pool < (static_cast<size_t>(48) << 20) && now - last_time >= 2;
        if (memory_lines && (moved || low)) {
            WispMemoryCensus census;
            wisp_memory_census(census);
            // WebKit's pages in direct memory (WebKit patch: WTF's OSAllocatorPS4.cpp; not in the census,
            // which counts what this process holds of the shared pool)
            size_t pages_held = 0;
            const size_t pages = wtf_ps4_page_memory ? wtf_ps4_page_memory(&pages_held) : 0;
            wisp_log("memory: %zu MiB of the shared pool left; this process holds %llu MiB flexible (stacks %llu MiB in %u, "
                "the largest %llu KiB), %llu MiB shared; largest: %s; WebKit's pages: %zu MiB in direct memory (%zu MiB held)",
                pool >> 20, static_cast<unsigned long long>(census.flexible >> 20),
                static_cast<unsigned long long>(census.stacks >> 20), census.stack_count,
                static_cast<unsigned long long>(census.stack_largest >> 10),
                static_cast<unsigned long long>(census.shared >> 20), census.largest, pages >> 20, pages_held >> 20);
            last = pool;
            last_time = now;
        }
        const timespec pause = { pool < (static_cast<size_t>(48) << 20) ? 0 : 1, pool < (static_cast<size_t>(48) << 20) ? 500000000 : 0 };
        nanosleep(&pause, nullptr);
    }
    return nullptr;
}

} // namespace

void wisp_watchdog_beat_start(void)
{
    if (!wisp_setting_int("watchdog", 1))
        return;
    // The main thread in the dumps too: the kit dumps the threads it watches, and it watches those
    // started through pthread_create (v00.13's three web dumps had every thread but the one that stopped)
    ps4_watch_thread("main");
    last_beat.store(wisp_seconds(), std::memory_order_relaxed);
    g_timeout_add(500, beat, nullptr);
}

namespace {

struct Stalls {
    double last_tick = 0;
    double since = 0;      // the window's start
    double longest = 0;
    double total = 0;
    unsigned count = 0;
};
Stalls stalls;

// (v00.44) The window's frames, beside its stalls: how often the page drew (its animation frames, style, layout and
// painting: WebPage::updateRendering) and how long that took, the compositor's frames and their time, and WebGPU
// canvases' frames (presented, not read back because the GPU was two behind, shown, textures made). A slow game
// shows here whether its own frame, the compositor or the GPU readback holds it back.
// (v00.45) WebAssembly's tiers when they moved: functions BBQ and OMG compiled (and failed, out of executable
// memory or otherwise), those too large for OMG and the largest, and the JIT's code memory. A game stuck in
// the interpreter or on BBQ shows here why.
void jit_line()
{
    if (!wisp_jit_counters)
        return;
    static unsigned long long last[7];
    unsigned long long value[8];
    bool moved = false;
    for (int i = 0; i < 8; i++) {
        value[i] = __atomic_load_n(&wisp_jit_counters[i], __ATOMIC_RELAXED);
        if (i < 7 && value[i] != last[i])
            moved = true;
    }
    if (!moved)
        return;
    size_t region = 0;
    const size_t code = wisp_jit_code_bytes(&region);
    wisp_log("wasm JIT: BBQ +%llu (%llu in all; %llu failed, %llu out of code memory), OMG +%llu (%llu in all; %llu failed, "
        "%llu out of code memory), %llu functions too large for OMG (the largest %llu KB); JIT code %zu of %zu MiB",
        value[0] - last[0], value[0], value[2], value[1], value[3] - last[3], value[3], value[5], value[4], value[6],
        value[7] / 1000, code >> 20, region >> 20);
    for (int i = 0; i < 7; i++)
        last[i] = value[i];
}

void frames_line(double seconds)
{
    if (!wisp_frame_counters || seconds <= 0)
        return;
    static unsigned long long last[8];
    unsigned long long delta[8];
    for (int i = 0; i < 8; i++) {
        const unsigned long long value = __atomic_load_n(&wisp_frame_counters[i], __ATOMIC_RELAXED);
        delta[i] = value - last[i];
        last[i] = value;
    }
    if (!delta[0] && !delta[2] && !delta[4])
        return;
    char webgpu[200] = "";
    if (delta[4])
        snprintf(webgpu, sizeof webgpu, "; WebGPU canvases: %.1f frames a second presented, %llu not read back (the GPU behind), "
            "%.1f a second shown, %llu textures made", delta[4] / seconds, delta[5], delta[6] / seconds, delta[7]);
    wisp_log("frames: the page drew %.1f times a second (%.1f ms each), the compositor %.1f frames a second (%.1f ms each)%s",
        delta[0] / seconds, delta[0] ? delta[1] / 1000.0 / delta[0] : 0.0, delta[2] / seconds,
        delta[2] ? delta[3] / 1000.0 / delta[2] : 0.0, webgpu);
}

gboolean stall_tick(gpointer)
{
    const double now = wisp_seconds();
    const double gap = now - stalls.last_tick;
    stalls.last_tick = now;
    if (gap >= 0.3) {
        stalls.count++;
        stalls.total += gap;
        stalls.longest = std::max(stalls.longest, gap);
    }
    if (now - stalls.since >= 10) {
        if (stalls.count)
            wisp_log("stalls: the main thread was held %u time%s in %.0f s (the longest %.2f s, %.1f s in all)", stalls.count,
                stalls.count == 1 ? "" : "s", now - stalls.since, stalls.longest, stalls.total);
        frames_line(now - stalls.since);
        jit_line();
        stalls.since = now;
        stalls.count = 0;
        stalls.longest = stalls.total = 0;
    }
    return G_SOURCE_CONTINUE;
}

} // namespace

void wisp_stall_log_start(void)
{
    if (!wisp_setting_int("stall_log", 1))
        return;
    stalls.last_tick = stalls.since = wisp_seconds();
    g_timeout_add(50, stall_tick, nullptr);
}

void wisp_memory_watch_start(void)
{
    memory_lines = wisp_setting_int("memory_log", 1) != 0;
    if (!memory_lines && !wisp_setting_int("watchdog", 1))
        return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(256) << 10);
    pthread_t thread;
    if (!pthread_create(&thread, &attr, memory_watch, nullptr))
        pthread_detach(thread);
    pthread_attr_destroy(&attr);
}

// GLib refers to LeakSanitizer's hooks weakly; an undefined weak symbol becomes an import the PS4's
// loader cannot satisfy (create-fself refuses it), so they are defined, doing nothing
extern "C" {
void __lsan_ignore_object(const void *) { }
void __lsan_disable(void) { }
void __lsan_enable(void) { }
}
