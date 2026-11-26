// Wisp: the log of each process (common/wisp.h). One file per process in /data/Wisp/logs, written
// with write() from a formatted line (no stdio buffer to lose when a process dies), stderr and stdout
// pointed at it so WebKit's and GLib's own output lands there too.
#include "wisp.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int log_fd = -1;
WispProcess log_process = WispProcess::UI;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
timespec start_time;

const char *file_name(WispProcess process)
{
    switch (process) {
    case WispProcess::UI: return WISP_LOGS "/ui.log";
    case WispProcess::Web: return WISP_LOGS "/web.log";
    case WispProcess::Network: return WISP_LOGS "/net.log";
    }
    return WISP_LOGS "/wisp.log";
}

void write_all(int fd, const char *data, size_t size)
{
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        data += n;
        size -= static_cast<size_t>(n);
    }
}

// The day this file was compiled (__DATE__, "Nov 19 2026") at 00:00 UTC: the real time is later
long long build_day()
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    tm parts = {};
    for (int m = 0; m < 12; m++)
        if (!strncmp(date, months + m * 3, 3))
            parts.tm_mon = m;
    parts.tm_mday = atoi(date + 4);
    parts.tm_year = atoi(date + 7) - 1900;
    return static_cast<long long>(timegm(&parts));
}

} // namespace

bool wisp_clock_behind()
{
    return static_cast<long long>(time(nullptr)) < build_day() - 86400;   // a day for time zones
}

void wisp_format_utc(long long t, char *out, size_t size)
{
    const time_t when = static_cast<time_t>(t);
    tm parts;
    if (!gmtime_r(&when, &parts) || !strftime(out, size, "%Y-%m-%d %H:%M UTC", &parts))
        snprintf(out, size, "%lld", t);
}

double wisp_seconds()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start_time.tv_sec) + (now.tv_nsec - start_time.tv_nsec) / 1e9;
}

const char *wisp_process_name()
{
    switch (log_process) {
    case WispProcess::UI: return "ui";
    case WispProcess::Web: return "web";
    case WispProcess::Network: return "net";
    }
    return "?";
}

void wisp_log_open(WispProcess process)
{
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    log_process = process;
    mkdir(WISP_DATA, 0777);
    mkdir(WISP_LOGS, 0777);
    const char *path = file_name(process);
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    if (process == WispProcess::UI) {
        // a new run: the last runs' logs move along, *.log -> *.prev.log -> *.prev2.log -> *.prev3.log
        // (a run that ended at once must not push the one that went wrong out); the children append
        // to theirs, which the UI rotates here, before it starts them
        static const char *const names[] = { "ui", "web", "net" };
        static const char *const ages[] = { ".prev3", ".prev2", ".prev", "" };
        for (const char *name : names) {
            for (int i = 1; i < 4; i++) {
                char from[96], to[96];
                snprintf(from, sizeof from, WISP_LOGS "/%s%s.log", name, ages[i]);
                snprintf(to, sizeof to, WISP_LOGS "/%s%s.log", name, ages[i - 1]);
                rename(from, to);
            }
        }
        flags |= O_TRUNC;
    }
    log_fd = open(path, flags, 0666);
    if (log_fd < 0)
        return;
    // stdout and stderr into the log too
    dup2(log_fd, 1);
    dup2(log_fd, 2);
    char clock[40];
    wisp_format_utc(time(nullptr), clock, sizeof clock);
    wisp_log("--- OrbisWeb %s, %s process, pid %d; console clock %s%s", WISP_VERSION_STRING, wisp_process_name(),
        getpid(), clock, wisp_clock_behind() ? " (wrong: earlier than this build)" : "");
}

extern "C" void wisp_log(const char *format, ...)
{
    char line[2048];
    const double t = wisp_seconds();
    int n = snprintf(line, sizeof line, "[%9.3f] ", t);
    va_list args;
    va_start(args, format);
    int m = vsnprintf(line + n, sizeof line - n - 1, format, args);
    va_end(args);
    if (m < 0)
        m = 0;
    size_t len = std::min(sizeof line - 2, static_cast<size_t>(n + m));
    if (!len || line[len - 1] != '\n')
        line[len++] = '\n';
    pthread_mutex_lock(&log_lock);
    if (log_fd >= 0)
        write_all(log_fd, line, len);
    pthread_mutex_unlock(&log_lock);
}

static GLogWriterOutput glib_writer(GLogLevelFlags level, const GLogField *fields, gsize count, gpointer)
{
    const char *domain = "", *message = "";
    for (gsize i = 0; i < count; i++) {
        if (!strcmp(fields[i].key, "GLIB_DOMAIN") && fields[i].value)
            domain = static_cast<const char *>(fields[i].value);
        else if (!strcmp(fields[i].key, "MESSAGE") && fields[i].value)
            message = static_cast<const char *>(fields[i].value);
    }
    const char *kind = (level & G_LOG_LEVEL_ERROR) ? "ERROR" : (level & G_LOG_LEVEL_CRITICAL) ? "CRITICAL"
        : (level & G_LOG_LEVEL_WARNING) ? "warning" : (level & G_LOG_LEVEL_MESSAGE) ? "message"
        : (level & G_LOG_LEVEL_INFO) ? "info" : "debug";
    wisp_log("[%s %s] %s", domain, kind, message);
    return G_LOG_WRITER_HANDLED;
}

void wisp_log_capture_glib()
{
    g_log_set_writer_func(glib_writer, nullptr, nullptr);
}
