// (v00.41) The kernel-object census: which calls take the PS4's internal memory in a web process.
//
// Every pthread mutex, condition variable, read-write lock, attribute object, semaphore, thread and kernel event
// queue comes out of a per-process pool of ~13.4 MiB (sceKernelInternalMemoryGetAvailableSize), and some of it
// stays taken after the object is destroyed: made and destroyed in a loop, objects use it up, and the process
// stops (GLib or libc++ abort on ENOMEM: v00.37's Star Fox, v00.39's YouTube). v00.38 and v00.39 moved
// GStreamer's clock and GLib's locks onto the kernel's futex, yet v00.40's Star Fox (EmulatorJS's N64 core)
// still emptied the pool in a minute (220 KiB/s) while Zelda (its GBA core) lost 15 KiB/s.
//
// Each call below (--wrap, app/CMakeLists.txt) measures the pool before and after and charges the difference
// to the code that called it: the first return addresses into the program found on the stack. A mutex, condition
// variable or read-write lock made statically (PTHREAD_*_INITIALIZER, std::mutex) gets its kernel object at its
// first use, so the lock and wait calls are measured when the object is still the initializer. Every 10 s in which
// the pool fell, web.log says by how much and which calls took most of it ("kobj:"; elf addresses: symbolize with
// llvm-symbolizer --obj=<binary>-vXX.YY.elf). Settings: kobj_census=0 (Diagnostics) leaves every call unmeasured.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <unistd.h>

#define PS4_KIT_DIAG_INTERNAL
#include "ps4_kit_diag.h"

#include "../common/wisp.h"

extern "C" {
int sceKernelInternalMemoryGetAvailableSize(size_t *size);
int sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
int sceKernelUsleep(unsigned int microseconds);
}

namespace {

std::atomic<bool> g_on { false };

enum Kind : uint32_t {
    MutexInit, MutexDestroy, MutexFirstLock, CondInit, CondDestroy, CondFirstUse, RwlockInit, RwlockDestroy,
    RwlockFirstLock, AttrInit, AttrDestroy, MutexattrInit, MutexattrDestroy, CondattrInit, CondattrDestroy, SemInit,
    SemDestroy, KeyCreate, KeyDelete, EqueueCreate, EqueueDelete, SemaCreate, SemaDelete, EventFlagCreate,
    EventFlagDelete, DirectAllocate, DirectRelease, DirectMap, DirectMapNamed, ReserveRange, KindCount
};
const char *const KIND_NAMES[KindCount] = {
    "pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock (first)", "pthread_cond_init",
    "pthread_cond_destroy", "pthread_cond (first use)", "pthread_rwlock_init", "pthread_rwlock_destroy",
    "pthread_rwlock (first lock)", "pthread_attr_init", "pthread_attr_destroy", "pthread_mutexattr_init",
    "pthread_mutexattr_destroy", "pthread_condattr_init", "pthread_condattr_destroy", "sem_init", "sem_destroy",
    "pthread_key_create", "pthread_key_delete", "sceKernelCreateEqueue", "sceKernelDeleteEqueue",
    "sceKernelCreateSema", "sceKernelDeleteSema", "sceKernelCreateEventFlag", "sceKernelDeleteEventFlag",
    "sceKernelAllocateDirectMemory", "sceKernelReleaseDirectMemory", "sceKernelMapDirectMemory",
    "sceKernelMapNamedDirectMemory", "sceKernelReserveVirtualRange",
};

constexpr int FRAMES = 3;
constexpr unsigned SLOTS = 4096;   // a power of two
struct Slot {
    std::atomic<uint64_t> key { 0 };
    std::atomic<uint32_t> kind { 0 };
    std::atomic<uint64_t> frames[FRAMES];
    std::atomic<uint64_t> calls { 0 };
    std::atomic<int64_t> bytes { 0 };   // taken from the pool (negative: given back)
};
Slot g_slots[SLOTS];
std::atomic<uint64_t> g_dropped { 0 };   // calls with no free slot
int64_t g_reported[SLOTS];               // (the reporter's own: bytes at its last report)
uint64_t g_reportedCalls[SLOTS];

inline size_t pool()
{
    size_t size = 0;
    return sceKernelInternalMemoryGetAvailableSize(&size) ? 0 : size;
}

// The end of this thread's stack (the kit's ps4_runtime.cpp stackEnd(), here unexported): read once a thread
__attribute__((tls_model("initial-exec"))) thread_local uintptr_t t_stackEnd = 0;
extern "C" int __real_pthread_attr_init(pthread_attr_t *);
extern "C" int __real_pthread_attr_destroy(pthread_attr_t *);
uintptr_t stackEnd()
{
    if (t_stackEnd)
        return t_stackEnd;
    void *address = nullptr;
    size_t size = 0;
    pthread_attr_t attr;
    if (__real_pthread_attr_init(&attr) == 0) {
        if (pthread_attr_get_np(pthread_self(), &attr) == 0)
            pthread_attr_getstack(&attr, &address, &size);
        __real_pthread_attr_destroy(&attr);
    }
    const uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    uintptr_t end = size ? reinterpret_cast<uintptr_t>(address) + size : 0;
    if (end <= frame) {
        void *start = nullptr, *mapEnd = nullptr;
        uint32_t protection = 0;
        end = sceKernelQueryMemoryProtection(reinterpret_cast<void *>(frame), &start, &mapEnd, &protection) == 0
            ? reinterpret_cast<uintptr_t>(mapEnd) : ((frame + 4096) & ~static_cast<uintptr_t>(4095));
    }
    t_stackEnd = end;
    return end;
}

// The call's site: its return address, then the next return addresses into the program on the stack above
// (64 words at most, never past the stack's end)
void callers(uint64_t (&frames)[FRAMES], void *returnAddress, const void *frame)
{
    frames[0] = reinterpret_cast<uint64_t>(returnAddress);
    int found = 1;
    const uintptr_t end = stackEnd();
    auto *word = static_cast<const uint64_t *>(frame);
    for (int i = 0; i < 64 && found < FRAMES && reinterpret_cast<uintptr_t>(word + 1) <= end; i++, word++) {
        const uint64_t value = *word;
        if (value != frames[found - 1] && value != frames[0] && ps4diag::isReturnAddress(value))
            frames[found++] = value;
    }
    for (; found < FRAMES; found++)
        frames[found] = 0;
}

void record(Kind kind, size_t before, size_t after, void *returnAddress, const void *frame)
{
    uint64_t frames[FRAMES];
    callers(frames, returnAddress, frame);
    uint64_t key = (static_cast<uint64_t>(kind) + 1) * 0x9e3779b97f4a7c15ull;
    for (uint64_t f : frames)
        key = (key ^ f) * 0xff51afd7ed558ccdull;
    key |= 1;
    for (unsigned probe = 0; probe < 64; probe++) {
        Slot &slot = g_slots[(key + probe) & (SLOTS - 1)];
        uint64_t current = slot.key.load(std::memory_order_acquire);
        if (!current) {
            uint64_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
                slot.kind.store(kind, std::memory_order_relaxed);
                for (int i = 0; i < FRAMES; i++)
                    slot.frames[i].store(frames[i], std::memory_order_relaxed);
                current = key;
            } else {
                current = expected;
            }
        }
        if (current == key) {
            slot.calls.fetch_add(1, std::memory_order_relaxed);
            slot.bytes.fetch_add(static_cast<int64_t>(before) - static_cast<int64_t>(after), std::memory_order_relaxed);
            return;
        }
    }
    g_dropped.fetch_add(1, std::memory_order_relaxed);
}

// a statically made object: its kernel object not made yet (FreeBSD's initializers: 0, 1 adaptive, 2 destroyed)
inline bool unmade(const void *object) { return *static_cast<const uintptr_t *>(object) <= 2; }

#define MEASURED(kind, call)                                                                    \
    do {                                                                                        \
        if (!g_on.load(std::memory_order_relaxed))                                              \
            return call;                                                                        \
        const size_t before = pool();                                                           \
        auto result = call;                                                                     \
        record(kind, before, pool(), __builtin_return_address(0), __builtin_frame_address(0)); \
        return result;                                                                          \
    } while (0)

#define MEASURED_IF(condition, kind, call)                                                      \
    do {                                                                                        \
        if (!g_on.load(std::memory_order_relaxed) || !(condition))                              \
            return call;                                                                        \
        const size_t before = pool();                                                           \
        auto result = call;                                                                     \
        record(kind, before, pool(), __builtin_return_address(0), __builtin_frame_address(0)); \
        return result;                                                                          \
    } while (0)

std::string elf(uint64_t address)
{
    char text[24];
    snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(ps4diag::toElf(address)));
    return text;
}

void *reporter(void *)
{
    size_t last = pool();
    int quiet = 0;
    for (;;) {
        sceKernelUsleep(10 * 1000 * 1000);
        const size_t now = pool();
        const int64_t fell = static_cast<int64_t>(last) - static_cast<int64_t>(now);
        last = now;
        // the slots that took most since the last report
        struct Top { unsigned slot; int64_t bytes; uint64_t calls; };
        Top top[8] = {};
        int64_t charged = 0;
        for (unsigned i = 0; i < SLOTS; i++) {
            if (!g_slots[i].key.load(std::memory_order_acquire))
                continue;
            const int64_t bytes = g_slots[i].bytes.load(std::memory_order_relaxed);
            const uint64_t calls = g_slots[i].calls.load(std::memory_order_relaxed);
            const int64_t delta = bytes - g_reported[i];
            const uint64_t deltaCalls = calls - g_reportedCalls[i];
            g_reported[i] = bytes;
            g_reportedCalls[i] = calls;
            charged += delta;
            for (int t = 0; t < 8; t++) {
                if (delta > top[t].bytes) {
                    for (int u = 7; u > t; u--)
                        top[u] = top[u - 1];
                    top[t] = { i, delta, deltaCalls };
                    break;
                }
            }
        }
        if (fell < 64 * 1024 && ++quiet < 6)
            continue;   // (a line at least every minute, and whenever the pool fell 64 KiB)
        quiet = 0;
        wisp_log("kobj: the PS4's internal memory fell %lld KiB in 10 s (%zu KiB left); the measured calls took %lld KiB%s",
            static_cast<long long>(fell / 1024), now >> 10, static_cast<long long>(charged / 1024),
            g_dropped.load() ? " (some calls went unrecorded: the table is full)" : "");
        for (const Top &entry : top) {
            if (entry.bytes < 1024)
                break;
            const Slot &slot = g_slots[entry.slot];
            std::string where;
            for (int f = 0; f < FRAMES; f++) {
                const uint64_t frame = slot.frames[f].load(std::memory_order_relaxed);
                if (!frame)
                    break;
                where += (f ? " < " : "") + (ps4diag::isCode(frame) ? elf(frame) : "system");
            }
            wisp_log("kobj:   +%lld KiB in %llu calls of %s, from elf %s", static_cast<long long>(entry.bytes / 1024),
                static_cast<unsigned long long>(entry.calls), KIND_NAMES[slot.kind.load(std::memory_order_relaxed)],
                where.c_str());
        }
    }
    return nullptr;
}

} // namespace

extern "C" void wisp_kobj_census_start(void)
{
    // (v00.42: off by default, kobj_census=1 on: v00.41's found its leak, Mesa's read-write locks, and its own
    // two pool queries per call showed in the page's stalls)
    if (!wisp_setting_int("kobj_census", 0) || !pool())
        return;
    g_on.store(true, std::memory_order_relaxed);
    pthread_t thread;
    pthread_attr_t attr;
    __real_pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(256) << 10);
    if (pthread_create(&thread, &attr, reporter, nullptr) == 0)
        pthread_detach(thread);
    __real_pthread_attr_destroy(&attr);
    wisp_log("kobj: the kernel-object census is on (kobj_census=0 turns it off): \"kobj:\" lines when the internal memory falls");
}

// --- the wrapped calls (app/CMakeLists.txt: --wrap for each, in the two web processes) -----------------

extern "C" {
int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int __real_pthread_mutex_destroy(pthread_mutex_t *);
int __real_pthread_mutex_lock(pthread_mutex_t *);
int __real_pthread_mutex_trylock(pthread_mutex_t *);
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __real_pthread_cond_destroy(pthread_cond_t *);
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int __real_pthread_cond_signal(pthread_cond_t *);
int __real_pthread_cond_broadcast(pthread_cond_t *);
int __real_pthread_rwlock_init(pthread_rwlock_t *, const pthread_rwlockattr_t *);
int __real_pthread_rwlock_destroy(pthread_rwlock_t *);
int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);
int __real_pthread_mutexattr_init(pthread_mutexattr_t *);
int __real_pthread_mutexattr_destroy(pthread_mutexattr_t *);
int __real_pthread_condattr_init(pthread_condattr_t *);
int __real_pthread_condattr_destroy(pthread_condattr_t *);
int __real_sem_init(sem_t *, int, unsigned);
int __real_sem_destroy(sem_t *);
int __real_pthread_key_create(pthread_key_t *, void (*)(void *));
int __real_pthread_key_delete(pthread_key_t);
int __real_sceKernelCreateEqueue(void **, const char *);
int __real_sceKernelDeleteEqueue(void *);
int __real_sceKernelCreateSema(void **, const char *, uint32_t, int, int, const void *);
int __real_sceKernelDeleteSema(void *);
int __real_sceKernelCreateEventFlag(void **, const char *, uint32_t, uint64_t, const void *);
int __real_sceKernelDeleteEventFlag(void *);
int __real_sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int, off_t *);
int __real_sceKernelReleaseDirectMemory(off_t, size_t);
int __real_sceKernelMapDirectMemory(void **, size_t, int, int, off_t, size_t);
int __real_sceKernelMapNamedDirectMemory(void **, size_t, int, int, off_t, size_t, const char *);
int __real_sceKernelReserveVirtualRange(void **, size_t, int, size_t);

int __wrap_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { MEASURED(MutexInit, __real_pthread_mutex_init(m, a)); }
int __wrap_pthread_mutex_destroy(pthread_mutex_t *m) { MEASURED(MutexDestroy, __real_pthread_mutex_destroy(m)); }
int __wrap_pthread_mutex_lock(pthread_mutex_t *m) { MEASURED_IF(unmade(m), MutexFirstLock, __real_pthread_mutex_lock(m)); }
int __wrap_pthread_mutex_trylock(pthread_mutex_t *m) { MEASURED_IF(unmade(m), MutexFirstLock, __real_pthread_mutex_trylock(m)); }
int __wrap_pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { MEASURED(CondInit, __real_pthread_cond_init(c, a)); }
int __wrap_pthread_cond_destroy(pthread_cond_t *c) { MEASURED(CondDestroy, __real_pthread_cond_destroy(c)); }
int __wrap_pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { MEASURED_IF(unmade(c), CondFirstUse, __real_pthread_cond_wait(c, m)); }
int __wrap_pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { MEASURED_IF(unmade(c), CondFirstUse, __real_pthread_cond_timedwait(c, m, t)); }
int __wrap_pthread_cond_signal(pthread_cond_t *c) { MEASURED_IF(unmade(c), CondFirstUse, __real_pthread_cond_signal(c)); }
int __wrap_pthread_cond_broadcast(pthread_cond_t *c) { MEASURED_IF(unmade(c), CondFirstUse, __real_pthread_cond_broadcast(c)); }
int __wrap_pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a) { MEASURED(RwlockInit, __real_pthread_rwlock_init(l, a)); }
int __wrap_pthread_rwlock_destroy(pthread_rwlock_t *l) { MEASURED(RwlockDestroy, __real_pthread_rwlock_destroy(l)); }
int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *l) { MEASURED_IF(unmade(l), RwlockFirstLock, __real_pthread_rwlock_rdlock(l)); }
int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *l) { MEASURED_IF(unmade(l), RwlockFirstLock, __real_pthread_rwlock_wrlock(l)); }
int __wrap_pthread_attr_init(pthread_attr_t *a) { MEASURED(AttrInit, __real_pthread_attr_init(a)); }
int __wrap_pthread_attr_destroy(pthread_attr_t *a) { MEASURED(AttrDestroy, __real_pthread_attr_destroy(a)); }
int __wrap_pthread_mutexattr_init(pthread_mutexattr_t *a) { MEASURED(MutexattrInit, __real_pthread_mutexattr_init(a)); }
int __wrap_pthread_mutexattr_destroy(pthread_mutexattr_t *a) { MEASURED(MutexattrDestroy, __real_pthread_mutexattr_destroy(a)); }
int __wrap_pthread_condattr_init(pthread_condattr_t *a) { MEASURED(CondattrInit, __real_pthread_condattr_init(a)); }
int __wrap_pthread_condattr_destroy(pthread_condattr_t *a) { MEASURED(CondattrDestroy, __real_pthread_condattr_destroy(a)); }
int __wrap_sem_init(sem_t *s, int shared, unsigned value) { MEASURED(SemInit, __real_sem_init(s, shared, value)); }
int __wrap_sem_destroy(sem_t *s) { MEASURED(SemDestroy, __real_sem_destroy(s)); }
int __wrap_pthread_key_create(pthread_key_t *k, void (*destructor)(void *)) { MEASURED(KeyCreate, __real_pthread_key_create(k, destructor)); }
int __wrap_pthread_key_delete(pthread_key_t k) { MEASURED(KeyDelete, __real_pthread_key_delete(k)); }
int __wrap_sceKernelCreateEqueue(void **q, const char *name) { MEASURED(EqueueCreate, __real_sceKernelCreateEqueue(q, name)); }
int __wrap_sceKernelDeleteEqueue(void *q) { MEASURED(EqueueDelete, __real_sceKernelDeleteEqueue(q)); }
int __wrap_sceKernelCreateSema(void **s, const char *name, uint32_t attr, int init, int max, const void *opt) { MEASURED(SemaCreate, __real_sceKernelCreateSema(s, name, attr, init, max, opt)); }
int __wrap_sceKernelDeleteSema(void *s) { MEASURED(SemaDelete, __real_sceKernelDeleteSema(s)); }
int __wrap_sceKernelCreateEventFlag(void **f, const char *name, uint32_t attr, uint64_t init, const void *opt) { MEASURED(EventFlagCreate, __real_sceKernelCreateEventFlag(f, name, attr, init, opt)); }
int __wrap_sceKernelDeleteEventFlag(void *f) { MEASURED(EventFlagDelete, __real_sceKernelDeleteEventFlag(f)); }
int __wrap_sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int type, off_t *phys) { MEASURED(DirectAllocate, __real_sceKernelAllocateDirectMemory(start, end, len, align, type, phys)); }
int __wrap_sceKernelReleaseDirectMemory(off_t start, size_t len) { MEASURED(DirectRelease, __real_sceKernelReleaseDirectMemory(start, len)); }
int __wrap_sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys, size_t align) { MEASURED(DirectMap, __real_sceKernelMapDirectMemory(addr, len, prot, flags, phys, align)); }
int __wrap_sceKernelMapNamedDirectMemory(void **addr, size_t len, int prot, int flags, off_t phys, size_t align, const char *name) { MEASURED(DirectMapNamed, __real_sceKernelMapNamedDirectMemory(addr, len, prot, flags, phys, align, name)); }
int __wrap_sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align) { MEASURED(ReserveRange, __real_sceKernelReserveVirtualRange(addr, len, flags, align)); }
}
