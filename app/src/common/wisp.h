// Wisp: what the three processes share (UI = eboot.bin, web = wisp-web.bin, network = wisp-net.bin).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#define WISP_VERSION_STRING WISP_VERSION   // "00.01", from CMake

// --- Paths on the console ------------------------------------------------------------------------
#define WISP_DATA "/data/Wisp"
#define WISP_LOGS WISP_DATA "/logs"
#define WISP_PROFILE WISP_DATA "/profile"        // cookies, storage, cache
#define WISP_DOWNLOADS WISP_DATA "/Downloads"
#define WISP_EXTENSIONS WISP_DATA "/extensions"
#define WISP_SETTINGS WISP_DATA "/settings.txt"
#define WISP_APP "/app0"
#define WISP_WEB_BIN WISP_APP "/wisp-web.bin"            // CPU rendering: no Mesa in it
#define WISP_WEB_GPU_BIN WISP_APP "/wisp-web-gpu.bin"    // GPU rendering (gpu=1): Mesa (EGL, Zink, RADV)
#define WISP_NET_BIN WISP_APP "/wisp-net.bin"

// --- Logging (common/wisp_log.cpp) ---------------------------------------------------------------
// Each process has its own file in /data/Wisp/logs (ui.log, web.log, net.log), lines stamped with
// the seconds since the app started. stderr goes there too (WebKit's and GLib's own messages).
enum class WispProcess { UI, Web, Network };
void wisp_log_open(WispProcess process);
extern "C" void wisp_log(const char *format, ...) __attribute__((format(printf, 1, 2)));   // (C too)
// GLib's and WebKit's g_log messages into the log
void wisp_log_capture_glib();
// "ui", "web", "net"
const char *wisp_process_name();

// --- Settings (/data/Wisp/settings.txt, key=value lines; common/wisp_settings.cpp) --------------
const char *wisp_setting(const char *key, const char *fallback);
int wisp_setting_int(const char *key, int fallback);
void wisp_setting_set(const char *key, const char *value);
// The key out of the file: its default applies again
void wisp_setting_remove(const char *key);

// How pages are drawn (settings: rendering=cpu|hybrid|gpu; hybrid by default, as a browser starts on
// the GPU and falls back to the CPU): all on the CPU (wisp-web.bin, no compositing); the CPU draws
// the page in tiles on painting threads and the GPU composites, scrolls and animates them (most
// browsers on a weak GPU; wisp-web-gpu.bin with WebKit's CPU tile rendering); or Skia draws on the
// GPU too (wisp-web-gpu.bin). The old gpu=0/1 key (v00.12 and before) no longer decides.
enum class WispRendering { Cpu, Hybrid, Gpu };
WispRendering wisp_rendering();
void wisp_set_rendering(WispRendering mode);
const char *wisp_rendering_name(WispRendering mode);   // "CPU", "CPU + GPU", "GPU"

// Where downloads go (v00.29; settings: download_folder, chosen on the Downloads page's Change the
// folder): the folder chosen, made again if it was deleted; WISP_DOWNLOADS when none was chosen, or
// when the one chosen can't be had now (its USB drive is out: *missing is then set). (UI process)
std::string wisp_download_folder(bool *missing = nullptr);
// The folder chosen, as saved ("" when none was: WISP_DOWNLOADS)
std::string wisp_download_folder_chosen();
// For a toast about a download that starts now: "" when it goes to the folder chosen, else
// " (in Downloads: <the folder chosen> isn't there)"
std::string wisp_download_fallback_note();

// --- The handoff between the UI and a local process (common/wisp_handoff.cpp) ------------------
// The PS4 starts a local process with one socket to its parent and nothing else (no descriptors, no
// environment: WinePS4/port/ps4_spawn_desk.c). Over it the child says HELLO (its pid), the UI sends
// START (the process type and WebKit's process identifier) with WebKit's IPC socket attached, and the
// child answers READY. The socket then stays open: its end tells either side the other is gone.
constexpr uint32_t WISP_MSG_HELLO = 0x4f4c4548;   // "HELO", value = pid
constexpr uint32_t WISP_MSG_START = 0x54525453;   // "STRT", + WispStart, 1 descriptor attached
constexpr uint32_t WISP_MSG_READY = 0x59444552;   // "REDY"
constexpr uint32_t WISP_MSG_STATS = 0x54415453;   // "STAT", value = heap MiB in use (every 2 s)
// UI -> child: write every thread's stack to logs/<process>.hang.log (value = seconds the page has not
// answered WebKit's UI): what its threads do while a page does not answer
constexpr uint32_t WISP_MSG_DUMP = 0x504d5544;    // "DUMP"

struct WispMsg {
    uint32_t magic;
    int32_t value;
};

enum class WispChild : uint32_t { Web = 1, Network = 2 };

struct WispStart {
    uint32_t magic;           // WISP_MSG_START
    WispChild type;
    uint64_t process_id;      // WebKit's process identifier (argv[1] of its process main)
    uint32_t log_flags;       // reserved
    uint32_t pad;
};

// WispStart::log_flags (v00.31): what the UI asks of a web process it starts
constexpr uint32_t WISP_CHILD_WEBGPU_SELFTEST = 1u << 0;   // the GPU web process's WebGPU self-test (child/webgpu_selftest.cpp)
// UI: flags for the next web process started (taken by it, once)
void wisp_set_next_web_flags(uint32_t flags);

// UI: starts the local process for type with WebKit's IPC socket; its local id (>= 0), or -1 and
// *error set. The process's control socket is kept by the UI (wisp_handoff_control_fd).
int wisp_launch_local(WispChild type, uint64_t process_id, int ipc_fd, char *error, size_t error_size);
// UI: ends a local process started by wisp_launch_local
void wisp_kill_local(int local_id);
// UI: how many local processes run (the PS4 allows 7 per app); of one type
int wisp_local_count();
int wisp_local_count_of(WispChild type);
// UI: a local process kind's heap in use (MiB) as it last said (every 2 s); -1 while none runs
int wisp_child_heap_mib(WispChild type);
// UI: asks each local process of type for its threads' stacks (WISP_MSG_DUMP); value goes with it
void wisp_ask_thread_dump(WispChild type, int value);
// UI: ends every local process and waits (up to 2 s) until they are gone (before a restart)
void wisp_end_all_local();

// Child: the handoff on the parent socket; true with *start filled and *ipc_fd WebKit's socket
bool wisp_child_handoff(WispStart *start, int *ipc_fd);

// --- Pages' sound from the UI (v00.35; common/wisp_audio.h, ui/audio_out.cpp) ------------------------
// UI: where web processes' sound rings come from (ui/audio_out.cpp sets these when settings audio_out=ui):
// `create` makes one for the web process about to get START (key: its local id) and gives its descriptor,
// which START carries as a second one (-1: none, the process opens a port itself); `release` lets it go
// when that process ends
void wisp_set_audio_ring_hooks(int (*create)(int key), void (*release)(int key));
// Child: the ring's descriptor START carried (-1: none)
int wisp_child_audio_fd();
// UI: pages' sound plays from the UI (unless settings audio_out=web); call once, before any web process
void wisp_audio_out_start();
// UI: the Settings page's own tone (440 Hz), played from the UI
void wisp_audio_tone(double seconds);

// --- Memory (common/wisp_memory.cpp) -------------------------------------------------------------
// What WebKit asks (WTF/wtf/ps4/MemoryPS4.cpp): bytes this process uses, and its budget.
extern "C" size_t wisp_memory_footprint(void);
extern "C" size_t wisp_memory_budget(void);
void wisp_memory_set_budget(size_t bytes);
// What is left of the flexible memory pool the app and its local processes share (each process's
// program image, stacks, shared memory): when it runs out the PS4 freezes the app, no crash
size_t wisp_flexible_free(void);
// What is left of this process's internal pool for threads, mutexes and condition variables (SIZE_MAX:
// the PS4 did not answer); when it runs out pthread_mutex_init fails and GLib ends the process (v00.36)
size_t wisp_internal_memory_free(void);
// (v00.38) logs what making and destroying each kind of kernel object costs that pool (memory_log)
void wisp_kernel_objects_selftest(void);
// the app's direct memory in all (sceKernelGetDirectMemorySize: the heaps and the GPU's memory)
size_t wisp_direct_memory_size(void);
// What this process holds of that pool: its flexible mappings (charged whole), its shared ones
// (shared memory and files, charged as touched), and its five largest by mapping name
struct WispMemoryCensus {
    uint64_t flexible = 0, shared = 0;
    uint64_t stacks = 0, stack_largest = 0;   // the mappings the kernel marks as stacks (of the flexible)
    unsigned stack_count = 0;
    char largest[400] = "";
};
void wisp_memory_census(WispMemoryCensus &census);
// A thread that logs the pool and this process's census when the pool moves by 8 MiB, and every 2 s
// while less than 48 MiB is left (settings: memory_log); it is the watchdog's too
void wisp_memory_watch_start(void);
// The watchdog (settings: watchdog): the main loop beats every 500 ms (call on the main thread before
// its loop runs); 8 s without a beat writes every thread's stack to logs/<process>.hang.log
void wisp_watchdog_beat_start(void);
// The main loop's stalls (v00.33, settings: stall_log): a 50 ms tick on the main loop; a tick 0.3 s or
// more apart from the last counts, and a line says at most every 10 s how many there were, the longest
// and the time in all (what a page's hitches are made of). Call on the main thread before its loop runs.
void wisp_stall_log_start(void);

// --- ICU (common/wisp_icu.cpp) ---------------------------------------------------------------------
// ICU's data archive (/app0/wisp/icudt77l.dat) mapped and handed to ICU, before WebKit uses it; then
// wisp_platform_setup
void wisp_icu_setup(void);
// What pages read of the console: its time zone (ICU's default zone, TZ) and processor count
void wisp_platform_setup(void);

// --- Time ------------------------------------------------------------------------------------------
double wisp_seconds();   // since the process started
// The console's clock is wrong: earlier than the day this build was made. A PS4 whose clock was lost
// reads 1 January 1970 (the .43 console, v00.08): every site's certificate is then "not valid yet".
bool wisp_clock_behind();
// t (seconds since 1970) as "2026-11-19 14:05 UTC"
void wisp_format_utc(long long t, char *out, size_t size);
