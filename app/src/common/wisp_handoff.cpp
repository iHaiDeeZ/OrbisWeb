// Wisp: starting WebKit's web and network processes as local processes of the app, and handing
// them WebKit's IPC socket (common/wisp.h). The PS4 side is WinePS4's (port/ps4_spawn_desk.c):
// sceSystemServiceAddLocalProcess with one socket to the child, nothing else inherited.
#include "wisp.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <ps4_kit_diag.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
int sceSystemServiceAddLocalProcess(int app_id, const char *path, const char *const *argv, const void *param);
int sceSystemServiceKillLocalProcess(int app_id, int local_id);
int sceSystemServiceGetAppIdOfBigApp(void);
int sceSystemServiceGetParentSocket(void);
}

namespace {

constexpr int HELLO_TIMEOUT_MS = 20000;
constexpr int MAX_LOCALS = 8;

bool write_all(int fd, const void *data, size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool read_all(int fd, void *data, size_t size)
{
    char *p = static_cast<char *>(data);
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool read_msg(int fd, WispMsg *msg, int timeout_ms)
{
    pollfd pfd = { fd, POLLIN, 0 };
    int ret;
    while ((ret = poll(&pfd, 1, timeout_ms)) == -1 && errno == EINTR) { }
    return ret == 1 && read_all(fd, msg, sizeof *msg);
}

// --- UI side ------------------------------------------------------------------------------------

struct Local {
    int local_id = -1;
    int pid = -1;
    int control = -1;
    WispChild type = WispChild::Web;
};

std::mutex locals_lock;
Local locals[MAX_LOCALS];
int app_id = -1;
// each kind's heap in use (MiB), as its last STATS said (-1: none running yet): the UI's memory meter
std::atomic<int> child_heap[3] = { { -1 }, { -1 }, { -1 } };
// (v00.35) pages' sound rings (ui/audio_out.cpp, wisp_set_audio_ring_hooks): none set = none made
int (*g_ring_for)(int key) = nullptr;
void (*g_ring_gone)(int key) = nullptr;

const char *child_name(WispChild type) { return type == WispChild::Web ? "web" : "network"; }

// Watches a child's control socket: its heap figures, and its end (crash or exit)
void *watch_child(void *arg)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    int fd;
    WispChild type;
    {
        std::lock_guard<std::mutex> guard(locals_lock);
        fd = locals[index].control;
        type = locals[index].type;
    }
    double last_report = 0;
    // the 30-second memory line in ui.log (Settings: memory_log=0 turns it off)
    const bool memory_log = wisp_setting_int("memory_log", 1);
    for (;;) {
        WispMsg msg;
        if (!read_all(fd, &msg, sizeof msg))
            break;
        if (msg.magic == WISP_MSG_STATS)
            child_heap[static_cast<int>(type) % 3].store(msg.value, std::memory_order_relaxed);
        if (memory_log && msg.magic == WISP_MSG_STATS && wisp_seconds() - last_report >= 30) {
            last_report = wisp_seconds();
            wisp_log("%s process: heap %d MiB in use; %zu MiB flexible memory left", child_name(type), msg.value,
                wisp_flexible_free() >> 20);
        }
    }
    std::lock_guard<std::mutex> guard(locals_lock);
    child_heap[static_cast<int>(type) % 3].store(-1, std::memory_order_relaxed);
    wisp_log("%s process (local %d, pid %d) ended", child_name(type), locals[index].local_id, locals[index].pid);
    if (type == WispChild::Web && g_ring_gone)
        g_ring_gone(locals[index].local_id);
    close(locals[index].control);
    locals[index] = Local();
    return nullptr;
}

} // namespace

// A refusal for want of room in the shared pool (0x80aa001a) is not asked again at once: WebKit asks as
// soon as it hears of it (v00.21: a service worker's process, 225 times in 200 s, each try ~0.3 s of the
// UI's main thread). Within 2 s of one, a launch of that kind fails here unless a process of that kind
// has ended since (its program image gave room back).
namespace {
double refused_at[3] = { -100, -100, -100 };
int refused_alive[3] = { 0, 0, 0 };
std::atomic<uint32_t> g_next_web_flags{0};   // (wisp_set_next_web_flags)
}

void wisp_set_next_web_flags(uint32_t flags) { g_next_web_flags = flags; }

void wisp_set_audio_ring_hooks(int (*create)(int key), void (*release)(int key))
{
    g_ring_for = create;
    g_ring_gone = release;
}

int wisp_launch_local(WispChild type, uint64_t process_id, int ipc_fd, char *error, size_t error_size)
{
    if (app_id < 0 && (app_id = sceSystemServiceGetAppIdOfBigApp()) < 0) {
        snprintf(error, error_size, "no app id (0x%08x)", static_cast<unsigned>(app_id));
        return -1;
    }
    const int kind = static_cast<int>(type) & 3;
    if (wisp_seconds() - refused_at[kind] < 2.0 && wisp_local_count_of(type) >= refused_alive[kind]) {
        snprintf(error, error_size, "not asked again so soon: the PS4 had no room for one %.1f s ago (%zu MiB of the shared pool left)",
            wisp_seconds() - refused_at[kind], wisp_flexible_free() >> 20);
        return -1;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
        snprintf(error, error_size, "socketpair: %s", strerror(errno));
        return -1;
    }
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    fcntl(sv[1], F_SETFD, FD_CLOEXEC);

    // the request as the PS4's own PS2 emulator makes it: its size, the child's socket, crash reports
    static const char *const argv[] = { "--wisp-local", nullptr };
    uint32_t param[0x8c / 4];
    memset(param, 0, sizeof param);
    param[0] = sizeof param;
    param[1] = static_cast<uint32_t>(sv[1]);
    param[2] = 1;
    // the web process without Mesa unless GPU rendering is on (Mesa is ~23 MiB of the shared pool)
    const char *path = type == WispChild::Web ? (wisp_rendering() != WispRendering::Cpu ? WISP_WEB_GPU_BIN : WISP_WEB_BIN) : WISP_NET_BIN;
    const double t0 = wisp_seconds();
    const int id = sceSystemServiceAddLocalProcess(app_id, path, argv, param);
    if (id < 0) {
        // 0x80aa001a (v00.20, a second web process): ~101 MiB of the shared pool left, and the web
        // process's program image alone is ~125 MiB of it
        char pool[96];
        snprintf(pool, sizeof pool, ": not enough of the shared memory pool? (%zu MiB left)", wisp_flexible_free() >> 20);
        snprintf(error, error_size, "the PS4 did not start %s (0x%08x%s)", path, static_cast<unsigned>(id),
            static_cast<unsigned>(id) == 0x80aa0008u ? ": 7 local processes already"
            : static_cast<unsigned>(id) == 0x80aa001au ? pool : "");
        if (static_cast<unsigned>(id) == 0x80aa001au) {
            refused_at[kind] = wisp_seconds();
            refused_alive[kind] = wisp_local_count_of(type);
        }
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    // HELLO first; our copy of the child's end stays open until then (a unix socket whose every
    // descriptor is in flight can arrive emptied: WinePS4)
    WispMsg hello;
    if (!read_msg(sv[0], &hello, HELLO_TIMEOUT_MS) || hello.magic != WISP_MSG_HELLO) {
        snprintf(error, error_size, "%s (local %d) did not answer", path, id);
        sceSystemServiceKillLocalProcess(app_id, id);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    close(sv[1]);

    // START with WebKit's socket (and, for a web process, the flags the UI set for the next one, and the
    // ring its sound goes to: v00.35, ui/audio_out.cpp)
    WispStart start = { WISP_MSG_START, type, process_id, type == WispChild::Web ? g_next_web_flags.exchange(0) : 0, 0 };
    const int audio_fd = type == WispChild::Web && g_ring_for ? g_ring_for(id) : -1;
    const int fds[2] = { ipc_fd, audio_fd };
    const int fd_count = audio_fd >= 0 ? 2 : 1;
    iovec iov = { &start, sizeof start };
    union {
        cmsghdr align;
        char buf[CMSG_SPACE(2 * sizeof(int))];
    } control;
    memset(&control, 0, sizeof control);
    msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = control.buf;
    m.msg_controllen = CMSG_SPACE(fd_count * sizeof(int));
    cmsghdr *cmsg = CMSG_FIRSTHDR(&m);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fd_count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, fd_count * sizeof(int));
    ssize_t sent;
    while ((sent = sendmsg(sv[0], &m, 0)) == -1 && errno == EINTR) { }
    WispMsg ready;
    if (sent != static_cast<ssize_t>(sizeof start) || !read_msg(sv[0], &ready, HELLO_TIMEOUT_MS) || ready.magic != WISP_MSG_READY) {
        snprintf(error, error_size, "%s (local %d, pid %d): the handoff failed (%s)", path, id, hello.value,
            sent < 0 ? strerror(errno) : "no READY");
        sceSystemServiceKillLocalProcess(app_id, id);
        close(sv[0]);
        if (audio_fd >= 0 && g_ring_gone)
            g_ring_gone(id);
        return -1;
    }
    wisp_log("%s process started: local %d, pid %d, WebKit id %llu (%.0f ms); %zu MiB flexible memory left",
        child_name(type), id, hello.value, static_cast<unsigned long long>(process_id), (wisp_seconds() - t0) * 1000,
        wisp_flexible_free() >> 20);

    int index = -1;
    {
        std::lock_guard<std::mutex> guard(locals_lock);
        for (int i = 0; i < MAX_LOCALS && index < 0; i++)
            if (locals[i].local_id < 0)
                index = i;
        if (index >= 0) {
            locals[index].local_id = id;
            locals[index].pid = hello.value;
            locals[index].control = sv[0];
            locals[index].type = type;
        }
    }
    if (index < 0) {
        close(sv[0]);
        if (audio_fd >= 0 && g_ring_gone)   // (no watcher to let it go later: it plays its sound itself, or none)
            g_ring_gone(id);
        return id;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 << 10);
    pthread_t thread;
    if (!pthread_create(&thread, &attr, watch_child, reinterpret_cast<void *>(static_cast<intptr_t>(index))))
        pthread_detach(thread);
    pthread_attr_destroy(&attr);
    return id;
}

void wisp_kill_local(int local_id)
{
    if (local_id < 0 || app_id < 0)
        return;
    wisp_log("ending local process %d", local_id);
    sceSystemServiceKillLocalProcess(app_id, local_id);
}

int wisp_local_count()
{
    std::lock_guard<std::mutex> guard(locals_lock);
    int count = 0;
    for (auto &local : locals)
        count += local.local_id >= 0;
    return count;
}

int wisp_child_heap_mib(WispChild type)
{
    return child_heap[static_cast<int>(type) % 3].load(std::memory_order_relaxed);
}

int wisp_local_count_of(WispChild type)
{
    std::lock_guard<std::mutex> guard(locals_lock);
    int count = 0;
    for (auto &local : locals)
        count += local.local_id >= 0 && local.type == type;
    return count;
}

void wisp_ask_thread_dump(WispChild type, int value)
{
    std::lock_guard<std::mutex> guard(locals_lock);
    for (auto &local : locals) {
        if (local.local_id < 0 || local.type != type)
            continue;
        const WispMsg msg = { WISP_MSG_DUMP, value };
        if (write_all(local.control, &msg, sizeof msg))
            wisp_log("asked the %s process (local %d) for its threads' stacks (logs/%s.hang.log)", child_name(type),
                local.local_id, type == WispChild::Web ? "web" : "net");
    }
}

void wisp_end_all_local()
{
    int ids[MAX_LOCALS];
    int n = 0;
    {
        std::lock_guard<std::mutex> guard(locals_lock);
        for (auto &local : locals)
            if (local.local_id >= 0)
                ids[n++] = local.local_id;
    }
    for (int i = 0; i < n; i++)
        wisp_kill_local(ids[i]);
    // (each one's watcher clears its entry when its control socket closes)
    for (int i = 0; i < 40 && wisp_local_count(); i++) {
        const timespec pause = { 0, 50000000 };
        nanosleep(&pause, nullptr);
    }
}

// --- Child side -----------------------------------------------------------------------------------

namespace {

int child_control = -1;
int child_audio_fd = -1;   // (v00.35) the UI's sound ring, when START carried one

void *child_stats(void *)
{
    for (;;) {
        WispMsg msg = { WISP_MSG_STATS, static_cast<int32_t>(wisp_memory_footprint() >> 20) };
        if (!write_all(child_control, &msg, sizeof msg))
            break;
        sleep(2);
    }
    return nullptr;
}

// The UI's requests on the control socket: every thread's stack while a page does not answer (the
// watchdog's dump sees only a main thread that stops; v00.14's page stopped answering with its main
// loop running)
void *child_requests(void *)
{
    for (;;) {
        WispMsg msg;
        if (!read_all(child_control, &msg, sizeof msg))
            break;
        if (msg.magic != WISP_MSG_DUMP)
            continue;
        char path[96], reason[160];
        snprintf(path, sizeof path, WISP_LOGS "/%s.hang.log", wisp_process_name());
        snprintf(reason, sizeof reason, "OrbisWeb's UI asked: a page of this %s process has not answered for %d s",
            wisp_process_name(), msg.value);
        wisp_log("the UI asked for every thread's stack (a page has not answered for %d s): %s", msg.value, path);
        ps4_dump_thread_stacks(path, reason);
    }
    return nullptr;
}

} // namespace

int wisp_child_audio_fd() { return child_audio_fd; }

bool wisp_child_handoff(WispStart *start, int *ipc_fd)
{
    int sock = sceSystemServiceGetParentSocket();
    if (sock < 0 || fcntl(sock, F_GETFD) == -1)
        sock = 3;  // (where the system puts it)
    WispMsg hello = { WISP_MSG_HELLO, static_cast<int32_t>(getpid()) };
    if (fcntl(sock, F_GETFD) == -1 || !write_all(sock, &hello, sizeof hello)) {
        wisp_log("handoff: no parent socket (%d)", sock);
        return false;
    }
    union {
        cmsghdr align;
        char buf[CMSG_SPACE(4 * sizeof(int))];
    } control;
    iovec iov = { start, sizeof *start };
    msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = control.buf;
    m.msg_controllen = sizeof control.buf;
    ssize_t n;
    while ((n = recvmsg(sock, &m, MSG_WAITALL)) == -1 && errno == EINTR) { }
    int fd = -1;
    for (cmsghdr *cmsg = CMSG_FIRSTHDR(&m); n > 0 && cmsg; cmsg = CMSG_NXTHDR(&m, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || fd >= 0)
            continue;
        // WebKit's socket, then (a web process, v00.35) the UI's sound ring
        const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        int got[4] = { -1, -1, -1, -1 };
        memcpy(got, CMSG_DATA(cmsg), std::min<size_t>(count, 4) * sizeof(int));
        fd = got[0];
        if (count >= 2)
            child_audio_fd = got[1];
    }
    if (n != static_cast<ssize_t>(sizeof *start) || start->magic != WISP_MSG_START || fd < 0) {
        wisp_log("handoff: bad START (%zd bytes, magic %08x, fd %d, errno %d)", n, start->magic, fd, errno);
        return false;
    }
    WispMsg ready = { WISP_MSG_READY, 0 };
    if (!write_all(sock, &ready, sizeof ready))
        return false;
    fcntl(sock, F_SETFD, FD_CLOEXEC);
    child_control = sock;
    *ipc_fd = fd;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 << 10);
    pthread_t thread;
    if (!pthread_create(&thread, &attr, child_stats, nullptr))
        pthread_detach(thread);
    if (!pthread_create(&thread, &attr, child_requests, nullptr))
        pthread_detach(thread);
    pthread_attr_destroy(&attr);
    return true;
}
