// OrbisWeb (v00.35): pages' sound on the PS4's audio out, played from the UI process.
//
// Every page's sound went to an audio out port the web process opened (child/gst_ps4_audiosink.c): the port
// opened, took every block at its pace, said its sound went to the TV, and nobody heard a thing (v00.26 to
// v00.34; the Settings page's tone too). The web process is a local process of the app; the UI is the app's
// own process, the one the player's sound (ui/player) comes from. Now the web process still mixes its
// players and writes the mixed blocks into a ring the UI made for it (common/wisp_audio.h, handed over with
// START); this thread takes them at the audio out's pace and plays them, with any other web process's (one
// starting while the last one ends) and the Settings page's own tone.
//
// Settings: audio_out=web (no ring: the web process opens a port itself, as before); audio_log (the lines).
#include "../common/wisp.h"
#include "../common/wisp_audio.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef SHM_ANON
#define SHM_ANON ((char *)1)   // FreeBSD's anonymous shared memory (the musl headers lack it; WebKit uses it too)
#endif

extern "C" void ps4_kit_set_thread_priority(int priority);

namespace {

constexpr int FRAMES = WISP_AUDIO_FRAMES;
constexpr long BLOCK_NS = static_cast<long>(FRAMES) * 1000000000L / 48000;
constexpr double IDLE_CLOSE_S = 3.0;    // the port goes after this long without sound (other apps get it back)
constexpr double REPORT_S = 10.0;       // the port's line in ui.log while it plays
constexpr int MAX_RINGS = 8;
constexpr double PI = 3.14159265358979323846;

struct Ring {
    int key = -1;                 // the web process's local id
    int fd = -1;
    WispAudioRing *ring = nullptr;
    uint64_t taken = 0;           // blocks played from it
    uint32_t peak = 0;            // its loudest sample since the last line
};

std::mutex g_lock;
Ring g_rings[MAX_RINGS];
bool g_started = false;
bool g_log = true;
std::atomic<int> g_tone_frames{0};     // the Settings page's tone: frames still to play
std::atomic<int> g_tone_total{0};

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

const char *level(uint32_t peak, char *text, size_t size)
{
    if (!peak)
        snprintf(text, size, "silent");
    else
        snprintf(text, size, "%.0f dBFS", 20.0 * log10(static_cast<double>(peak) / 32768.0));
    return text;
}

// Where the port's sound goes and how loud the system plays it (sceAudioOutGetPortState: output 1, 2, 4
// the TV's connections, 0x40 headphones, 0x80 other; volume -1 when the port has none of its own)
void port_state_line(int port, const char *when)
{
    union {
        struct {
            uint16_t output;
            uint8_t channel, reserved;
            int16_t volume;
            uint16_t reroutes;
            uint64_t flag;
            uint64_t reserved2[2];
        } s;
        uint8_t raw[128];
    } state;
    memset(&state, 0, sizeof state);
    const int result = sceAudioOutGetPortState(port, &state);
    if (result < 0) {
        wisp_log("audio: port %d %s: its state unknown (sceAudioOutGetPortState: 0x%08x)", port, when, static_cast<unsigned>(result));
        return;
    }
    char raw[3 * 16 + 1] = "";
    for (int i = 0; i < 16; i++)
        snprintf(raw + 3 * i, sizeof raw - 3 * i, "%02x ", state.raw[i]);
    wisp_log("audio: port %d %s: output 0x%x, channel %u, volume %d, reroutes %u, flags 0x%llx (%s)", port, when, state.s.output,
        state.s.channel, state.s.volume, state.s.reroutes, static_cast<unsigned long long>(state.s.flag), raw);
}

struct Port {
    int handle = -1;
    int user = -1;
    double opened_at = 0, last_sound = 0, report_at = 0, next_try = 0;
    uint64_t blocks = 0, report_blocks = 0, gaps = 0, report_gaps = 0;
    uint32_t peak = 0;
    int failures = 0, first_error = 0;
};

void set_port_open(bool open)
{
    for (Ring &r : g_rings)
        if (r.ring)
            __atomic_store_n(&r.ring->port_open, open ? 1u : 0u, __ATOMIC_RELAXED);
}

void port_open(Port &p)
{
    static bool initialized = false;
    if (!initialized) {
        sceAudioOutInit();   // (an error when the player did it before: harmless)
        initialized = true;
    }
    // the system's own id (255): a main port is the system's (v00.35's opens with the console's user all
    // failed, "sceMbusAddHandleByUserId 0x20000007", and 255's played); the console's user only if it fails
    OrbisUserServiceUserId user = 255;
    int handle = sceAudioOutOpen(255, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, FRAMES, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0 && !sceUserServiceGetInitialUser(&user) && user >= 0)
        handle = sceAudioOutOpen(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, FRAMES, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle < 0) {
        if (!p.failures++)
            wisp_log("audio: the PS4's audio out did not open (sceAudioOutOpen: 0x%08x): pages go on without sound, "
                     "and it is tried again every second", static_cast<unsigned>(handle));
        p.next_try = now_s() + 1.0;
        return;
    }
    p.handle = handle;
    p.user = user;
    p.opened_at = p.report_at = now_s();
    p.blocks = p.report_blocks = p.gaps = p.report_gaps = 0;
    p.peak = 0;
    p.first_error = 0;
    wisp_log("audio: the PS4's audio out open in the UI (port %d, %s %d, 48 kHz stereo; pages' sound and the sound test play here)%s",
        handle, user == 255 ? "the system's user" : "user", user, p.failures ? " after failing before" : "");
    p.failures = 0;
    // full volume on both channels (0 dB: 32768)
    int32_t volume[8] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768 };
    const int set = sceAudioOutSetVolume(handle, (1 << 0) | (1 << 1), volume);
    if (set < 0)
        wisp_log("audio: port %d: sceAudioOutSetVolume failed (0x%08x)", handle, static_cast<unsigned>(set));
    if (g_log)
        port_state_line(handle, "opened");
    std::lock_guard<std::mutex> guard(g_lock);
    set_port_open(true);
}

void port_close(Port &p, const char *why)
{
    if (p.handle < 0)
        return;
    sceAudioOutOutput(p.handle, nullptr);   // (waits for what is queued)
    sceAudioOutClose(p.handle);
    wisp_log("audio: port %d closed (%s) after %.1f s: %llu blocks played, %llu of them silence (gaps, and the wait before closing)", p.handle, why,
        now_s() - p.opened_at, static_cast<unsigned long long>(p.blocks), static_cast<unsigned long long>(p.gaps));
    if (p.first_error)
        wisp_log("audio: sceAudioOutOutput failed: 0x%08x", static_cast<unsigned>(p.first_error));
    p.handle = -1;
    std::lock_guard<std::mutex> guard(g_lock);
    set_port_open(false);
}

// the port's line: blocks out since the last one against its rate (187.5 a second), the gaps filled with
// silence (a web process late), the loudest output sample, and each web process's loudest
void report(Port &p, double now)
{
    char rings[256] = "", peak[24];
    size_t length = 0;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        for (Ring &r : g_rings) {
            if (!r.ring || length >= sizeof rings - 64)
                continue;
            const int wrote = snprintf(rings + length, sizeof rings - length, "%sweb process %d %s", length ? ", " : "", r.key,
                level(r.peak, peak, sizeof peak));
            if (wrote > 0)
                length += static_cast<size_t>(wrote);
            r.peak = 0;
        }
    }
    const double seconds = now - p.report_at;
    wisp_log("audio: port %d: %llu blocks in %.1f s (%.0f%% of its rate), %llu gaps filled, output %s; from: %s", p.handle,
        static_cast<unsigned long long>(p.report_blocks), seconds, seconds > 0 ? 100.0 * static_cast<double>(p.report_blocks) / (seconds * 187.5) : 0.0,
        static_cast<unsigned long long>(p.report_gaps), level(p.peak, peak, sizeof peak), length ? rings : "the sound test only");
    p.report_at = now;
    p.report_blocks = p.report_gaps = 0;
    p.peak = 0;
}

void *audio_main(void *)
{
    ps4_kit_set_thread_priority(400);   // (over the UI's own threads: a late block is a gap in the sound)
    static int32_t sum[FRAMES * 2];
    static int16_t out[FRAMES * 2] __attribute__((aligned(64)));
    Port p;
    double phase = 0;
    for (;;) {
        bool any = false;
        memset(sum, 0, sizeof sum);
        {
            std::lock_guard<std::mutex> guard(g_lock);
            for (Ring &r : g_rings) {
                if (!r.ring)
                    continue;
                __atomic_add_fetch(&r.ring->ui_rounds, 1, __ATOMIC_RELAXED);
                const uint64_t written = __atomic_load_n(&r.ring->written, __ATOMIC_ACQUIRE);
                uint64_t read = __atomic_load_n(&r.ring->read, __ATOMIC_RELAXED);
                if (written - read > WISP_AUDIO_RING_BLOCKS) {
                    // (a writer never gets that far ahead: taken as a restart, from its next block on)
                    read = written;
                    __atomic_store_n(&r.ring->read, read, __ATOMIC_RELEASE);
                }
                if (written == read)
                    continue;
                const int16_t *block = r.ring->data[read % WISP_AUDIO_RING_BLOCKS];
                uint32_t peak = r.peak;
                for (int i = 0; i < FRAMES * 2; i++) {
                    sum[i] += block[i];
                    const uint32_t a = static_cast<uint32_t>(block[i] < 0 ? -static_cast<int32_t>(block[i]) : block[i]);
                    if (a > peak)
                        peak = a;
                }
                r.peak = peak;
                __atomic_store_n(&r.ring->read, read + 1, __ATOMIC_RELEASE);
                r.taken++;
                any = true;
            }
        }
        // the Settings page's tone (440 Hz at -12 dBFS, 10 ms fades)
        int tone = g_tone_frames.load(std::memory_order_relaxed);
        if (tone > 0) {
            const int total = g_tone_total.load(std::memory_order_relaxed);
            const double step = 2.0 * PI * 440.0 / 48000.0;
            for (int i = 0; i < FRAMES && tone - i > 0; i++) {
                const int done = total - (tone - i);
                const int left = tone - i;
                const double fade = std::min(1.0, std::min(done, left) / 480.0);
                const int32_t s = static_cast<int32_t>(8192.0 * fade * sin(phase));
                phase += step;
                if (phase > 2.0 * PI)
                    phase -= 2.0 * PI;
                sum[2 * i] += s;
                sum[2 * i + 1] += s;
            }
            g_tone_frames.store(std::max(0, tone - FRAMES), std::memory_order_relaxed);
            any = true;
        }
        const double now = now_s();
        if (!any) {
            if (p.handle >= 0) {
                if (now - p.last_sound > IDLE_CLOSE_S) {
                    port_close(p, "no sound for 3 s");
                    continue;
                }
                // a gap (the web process late, or a pause between two sounds): silence keeps the port's time
                memset(out, 0, sizeof out);
                const int result = sceAudioOutOutput(p.handle, out);
                if (result < 0) {
                    if (!p.first_error)
                        p.first_error = result;
                    port_close(p, "an output failed");
                    p.next_try = now_s() + 1.0;
                    continue;
                }
                p.blocks++;
                p.report_blocks++;
                p.gaps++;
                p.report_gaps++;
            } else {
                const timespec idle = { 0, 10 * 1000000L };   // (nothing playing: a look every 10 ms)
                nanosleep(&idle, nullptr);
            }
            continue;
        }
        p.last_sound = now;
        uint32_t peak = p.peak;
        for (int i = 0; i < FRAMES * 2; i++) {
            const int32_t s = sum[i] < -32768 ? -32768 : sum[i] > 32767 ? 32767 : sum[i];
            out[i] = static_cast<int16_t>(s);
            const uint32_t a = static_cast<uint32_t>(s < 0 ? -s : s);
            if (a > peak)
                peak = a;
        }
        p.peak = peak;
        if (p.handle < 0 && now >= p.next_try)
            port_open(p);
        if (p.handle >= 0) {
            // (blocks until the block before has started: the pace every web process's mixer follows)
            const int result = sceAudioOutOutput(p.handle, out);
            if (result < 0) {
                if (!p.first_error)
                    p.first_error = result;
                port_close(p, "an output failed");
                p.next_try = now_s() + 1.0;
            } else {
                p.blocks++;
                p.report_blocks++;
            }
        } else {
            // no port to be had: its pace without it (the pages go on in silence)
            const timespec block = { 0, BLOCK_NS };
            nanosleep(&block, nullptr);
        }
        if (g_log && p.handle >= 0 && now_s() - p.report_at >= REPORT_S)
            report(p, now_s());
    }
    return nullptr;
}

// (g_lock held) the thread, once
void start_thread()
{
    if (g_started)
        return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 << 10);
    pthread_t thread;
    if (!pthread_create(&thread, &attr, audio_main, nullptr)) {
        pthread_detach(thread);
        g_started = true;
    } else {
        wisp_log("audio: the UI's audio thread did not start: pages' sound stays silent");
    }
    pthread_attr_destroy(&attr);
}

// A ring for the web process with local id `key`, about to get START: its descriptor (kept here until
// ring_gone; START carries a copy), or -1 (none: that process opens a port of its own)
int ring_for(int key)
{
    int fd = shm_open(SHM_ANON, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        wisp_log("audio: no shared memory for the web process's sound (shm_open: %s): it plays it itself", strerror(errno));
        return -1;
    }
    void *map = MAP_FAILED;
    if (ftruncate(fd, WISP_AUDIO_RING_BYTES) == 0)
        map = mmap(nullptr, WISP_AUDIO_RING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        wisp_log("audio: the sound ring could not be mapped (%s): the web process plays its sound itself", strerror(errno));
        close(fd);
        return -1;
    }
    auto *ring = static_cast<WispAudioRing *>(map);
    memset(ring, 0, WISP_AUDIO_RING_BYTES);
    ring->magic = WISP_AUDIO_RING_MAGIC;
    ring->blocks = WISP_AUDIO_RING_BLOCKS;
    std::lock_guard<std::mutex> guard(g_lock);
    for (Ring &r : g_rings) {
        if (r.ring)
            continue;
        r.key = key;
        r.fd = fd;
        r.ring = ring;
        r.taken = 0;
        r.peak = 0;
        start_thread();
        return fd;
    }
    munmap(map, WISP_AUDIO_RING_BYTES);
    close(fd);
    return -1;
}

// The web process with local id `key` ended (or its start failed): its ring goes
void ring_gone(int key)
{
    std::lock_guard<std::mutex> guard(g_lock);
    for (Ring &r : g_rings) {
        if (!r.ring || r.key != key)
            continue;
        if (r.taken)
            wisp_log("audio: the web process %d's sound: %llu blocks played", key, static_cast<unsigned long long>(r.taken));
        munmap(r.ring, WISP_AUDIO_RING_BYTES);
        close(r.fd);
        r = Ring();
    }
}

} // namespace

void wisp_audio_out_start()
{
    g_log = wisp_setting_int("audio_log", 1) != 0;
    const std::string where = wisp_setting("audio_out", "ui");
    if (where == "web") {
        wisp_log("audio: pages' sound plays from the web process (settings: audio_out=web)");
        return;
    }
    wisp_set_audio_ring_hooks(ring_for, ring_gone);
    wisp_log("audio: pages' sound plays from the UI process (the web process mixes it into a ring of %d blocks)", WISP_AUDIO_RING_BLOCKS);
}

void wisp_audio_tone(double seconds)
{
    const int frames = static_cast<int>(seconds * 48000.0);
    g_tone_total.store(frames, std::memory_order_relaxed);
    g_tone_frames.store(frames, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(g_lock);
    start_thread();
    wisp_log("audio: the sound test's tone from the UI (%.1f s of 440 Hz)", seconds);
}
