// Wisp's screen (screen.h), from WinePS4's. The buffers are write-combined direct memory, which the
// display reads: the browser composes its frame and writes it in once, front to back. BGRA
// (A8R8G8B8): WebKit's frames are copied in as they are; the toolbar is converted as it is drawn.
#include "screen.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <pthread.h>

#include <orbis/SystemService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

extern "C" {
int32_t ps4s_allocate_direct(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t *phys) __asm__("sceKernelAllocateDirectMemory");
int32_t ps4s_map_direct(void **addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align) __asm__("sceKernelMapDirectMemory");
size_t ps4s_direct_memory_size() __asm__("sceKernelGetDirectMemorySize");
// (OpenOrbis's header declares it without its parameters)
int32_t ps4s_add_vblank_event(OrbisKernelEqueue queue, int32_t handle, void *data) __asm__("sceVideoOutAddVblankEvent");
void ps4_boot_trace(const char *line);
}

namespace shell {
namespace {

constexpr size_t BUFFER_SIZE = 8ull << 20;  // 1920x1080x4 rounded up to 2 MiB pages
constexpr int32_t WC_GARLIC = 3;            // write-combined, what the display reads
constexpr int32_t PROT_CPU_GPU_RW = 0x33;
constexpr int FIRST_SLOT = 12;              // ps4-kit's app slots (ps4_vk_wsi.h kAppFirstBuffer)
constexpr int64_t FLIP_ARG_BASE = 0x57490000;  // our flips numbers, to tell when one is shown
constexpr uint32_t PIXEL_FORMAT_A8R8G8B8_SRGB = 0x80000000;  // (SCE_VIDEO_OUT_PIXEL_FORMAT_; not in OpenOrbis)

int video = -1;
uint32_t *buffers[2] = {};
int current = 0;
int64_t flips = 0;
bool splash_hidden = false;
OrbisKernelEqueue vblank_queue = 0;
void (*vblank_callback)() = nullptr;

void trace(const char *fmt, unsigned a = 0, unsigned b = 0) {
    char line[160];
    std::snprintf(line, sizeof line, fmt, a, b);
    std::fprintf(stderr, "shell: %s\n", line);
    ps4_boot_trace(line);
}

// waits for each vblank and tells the browser (which composes its frame right after one, so the frame
// is ready well before the next)
void *vblank_thread(void *) {
    for (;;) {
        OrbisKernelEvent event;
        int count = 0;
        if (sceKernelWaitEqueue(vblank_queue, &event, 1, &count, nullptr) < 0) {
            const timespec pause = {0, 16000000};
            nanosleep(&pause, nullptr);
        }
        vblank_callback();
    }
    return nullptr;
}

} // namespace

bool screen_start_vblank(void (*callback)()) {
    if (video < 0 || !callback)
        return false;
    vblank_callback = callback;
    const int created = sceKernelCreateEqueue(&vblank_queue, "wisp vblank");
    const int added = created < 0 ? -1 : ps4s_add_vblank_event(vblank_queue, video, nullptr);
    if (created < 0 || added < 0) {
        trace("screen: no vblank events (%08x %08x): frames on a timer", static_cast<unsigned>(created), static_cast<unsigned>(added));
        return false;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 10);
    pthread_t thread;
    const int started = pthread_create(&thread, &attr, vblank_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (started != 0) {
        trace("screen: the vblank thread did not start (%d): frames on a timer", static_cast<unsigned>(started));
        return false;
    }
    trace("screen: frames paced by the TV's vblank");
    return true;
}

bool screen_flip_pending() {
    if (!flips)
        return false;
    OrbisVideoOutFlipStatus status{};
    if (sceVideoOutGetFlipStatus(video, &status) < 0)
        return false;
    return status.flipArg < FLIP_ARG_BASE + flips;
}

bool screen_open() {
    if (video >= 0)
        return true;
    video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (video < 0) {
        trace("screen: sceVideoOutOpen failed (%08x)", static_cast<unsigned>(video));
        return false;
    }
    off_t phys = 0;
    void *base = nullptr;
    const int32_t allocated = ps4s_allocate_direct(0, static_cast<off_t>(ps4s_direct_memory_size()), 2 * BUFFER_SIZE,
                                                   2 << 20, WC_GARLIC, &phys);
    const int32_t mapped = allocated != 0 ? -1 : ps4s_map_direct(&base, 2 * BUFFER_SIZE, PROT_CPU_GPU_RW, 0, phys, 2 << 20);
    if (allocated != 0 || mapped != 0) {
        trace("screen: no direct memory for the screen (%08x %08x)", static_cast<unsigned>(allocated), static_cast<unsigned>(mapped));
        video = -1;
        return false;
    }
    buffers[0] = static_cast<uint32_t *>(base);
    buffers[1] = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(base) + BUFFER_SIZE);
    std::memset(base, 0, 2 * BUFFER_SIZE);
    void *addresses[2] = {buffers[0], buffers[1]};
    OrbisVideoOutBufferAttribute attribute;
    sceVideoOutSetBufferAttribute(&attribute, PIXEL_FORMAT_A8R8G8B8_SRGB, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  0, SCREEN_W, SCREEN_H, SCREEN_W);
    const int set = sceVideoOutRegisterBuffers(video, FIRST_SLOT, addresses, 2, &attribute);
    if (set < 0) {
        trace("screen: sceVideoOutRegisterBuffers failed (%08x)", static_cast<unsigned>(set));
        video = -1;
        return false;
    }
    trace("screen: ready (VideoOut %u, buffers %u-...)", static_cast<unsigned>(video), FIRST_SLOT);
    return true;
}

uint32_t *screen_back_buffer() {
    // until the last flip is on screen, so the buffer about to be written isn't: this paces the
    // desktop to the TV's refresh
    if (flips) {
        for (int i = 0; i < 80; i++) {
            OrbisVideoOutFlipStatus status{};
            if (sceVideoOutGetFlipStatus(video, &status) < 0 || status.flipArg >= FLIP_ARG_BASE + flips)
                break;
            const timespec pause = {0, 500000};
            nanosleep(&pause, nullptr);
        }
    }
    return buffers[current];
}

int screen_back_index() { return current; }

// the freeze report's memory probe (port/ps4_kstate.c): a pixel of the screen, read to see whether the
// desktop's direct memory answers; nullptr without a screen
extern "C" volatile uint32_t *ps4_screen_probe_pixel() { return buffers[0]; }

void screen_flip() {
    __builtin_ia32_sfence();  // the write-combined stores out before the flip
    const int flip = sceVideoOutSubmitFlip(video, FIRST_SLOT + current, ORBIS_VIDEO_OUT_FLIP_VSYNC, FLIP_ARG_BASE + ++flips);
    if (!splash_hidden) {
        trace("screen: first frame (flip %08x)", static_cast<unsigned>(flip));
        // the system shows the app's splash until the app has a picture of its own
        sceSystemServiceHideSplashScreen();
        splash_hidden = true;
    }
    current ^= 1;
}

} // namespace shell
