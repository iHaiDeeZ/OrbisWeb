// Wisp: ICU's data for all three processes, loaded before WebKit starts (wisp.h).
//
// ICU finds its data archive itself (ICU_DATA=/app0/wisp: stat, open, then mmap MAP_SHARED), and when
// any step fails it says nothing: every service that needs the archive then fails. WebKit's text
// layout stopped the web process on the first break iterator it could not open
// (TextBreakIteratorICU's RELEASE_ASSERT, v00.05), though the archive is in the package and valid.
//
// Here the archive is read into direct memory of its own (the heap's block landed in the flexible
// pool in v00.12's web process) and given to ICU with udata_setCommonData. Not mapped: a file mapping
// may come out of the flexible memory pool the app's processes share, and the archive is 31 MiB.
// The log says what ICU's own loader met (stat, a mapping of the file), how long the read took and
// whether a break iterator opens.
#include "wisp.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <unicode/putil.h>
#include <unicode/ubrk.h>
#include <unicode/ucal.h>
#include <unicode/udata.h>
#include <unicode/utypes.h>

extern "C" {
int32_t wisp_allocate_direct(off_t start, off_t end, size_t len, size_t align, int32_t type, off_t *phys) __asm__("sceKernelAllocateDirectMemory");
int32_t wisp_map_direct(void **addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align) __asm__("sceKernelMapDirectMemory");
}
// (wisp_direct_memory_size: common/wisp_memory.cpp)

namespace {

constexpr const char *ICU_PATH = WISP_APP "/wisp/" U_ICUDATA_NAME ".dat";

// The archive in direct memory of its own, mapped for the CPU (cached): the app has gigabytes of
// direct memory, while a heap block this big could land in the flexible pool the processes share
// (v00.12's web process: a 30 MiB anonymous mapping, ~20% of the pool). nullptr: none to be had.
void *direct_block(size_t size)
{
    constexpr size_t page = static_cast<size_t>(16) << 10;
    const size_t length = (size + page - 1) & ~(page - 1);
    off_t phys = 0;
    if (wisp_allocate_direct(0, static_cast<off_t>(wisp_direct_memory_size()), length, page, 0 /* write-back */, &phys) != 0)
        return nullptr;
    void *address = nullptr;
    if (wisp_map_direct(&address, length, 0x3 /* CPU read and write */, 0, phys, page) != 0)
        return nullptr;
    return address;
}

const char *where = "the heap";

void *read_all(int fd, size_t size)
{
    auto *data = static_cast<char *>(direct_block(size));
    where = "direct memory";
    if (!data) {
        data = static_cast<char *>(malloc(size));
        where = "the heap";
    }
    if (!data)
        return nullptr;
    size_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, data + done, size - done, static_cast<off_t>(done));
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            if (!strcmp(where, "the heap"))
                free(data);
            return nullptr;
        }
        done += static_cast<size_t>(n);
    }
    return data;
}

} // namespace

void wisp_icu_setup(void)
{
    // what ICU's own loader met (umapfile.cpp: stat, open, mmap MAP_SHARED of the whole file)
    struct stat st {};
    const int stat_errno = stat(ICU_PATH, &st) ? errno : 0;

    const int fd = open(ICU_PATH, O_RDONLY);
    if (fd < 0) {
        wisp_log("ICU data: cannot open %s (%s); stat: %s", ICU_PATH, strerror(errno), stat_errno ? strerror(stat_errno) : "ok");
        return;
    }
    size_t size = 0;
    struct stat fst {};
    if (fstat(fd, &fst) == 0 && fst.st_size > 0)
        size = static_cast<size_t>(fst.st_size);
    else {
        const off_t end = lseek(fd, 0, SEEK_END);
        size = end > 0 ? static_cast<size_t>(end) : 0;
    }
    if (!size) {
        wisp_log("ICU data: %s has no size; stat: %s", ICU_PATH, stat_errno ? strerror(stat_errno) : "ok");
        close(fd);
        return;
    }
    // (one page: whether this file system maps files at all)
    void *probe = mmap(nullptr, 16384, PROT_READ, MAP_SHARED, fd, 0);
    const int map_errno = probe == MAP_FAILED ? errno : 0;
    if (probe != MAP_FAILED)
        munmap(probe, 16384);

    const size_t before = wisp_flexible_free();
    const double t0 = wisp_seconds();
    void *data = read_all(fd, size);
    const char *how = !strcmp(where, "direct memory") ? "read into direct memory" : "read into the heap";
    if (!data) {
        how = "mapped (no heap for it)";
        data = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED)
            data = nullptr;
    }
    close(fd);
    const double took = wisp_seconds() - t0;
    if (!data) {
        wisp_log("ICU data: %s could be neither read nor mapped (%s)", ICU_PATH, strerror(errno));
        return;
    }

    UErrorCode status = U_ZERO_ERROR;
    udata_setCommonData(data, &status);
    // ICU's own loader may not find the archive: at the first item a lookup misses (any locale it probes
    // that the data lacks) it maps the whole file from its data directory as "the extended data"
    // (udata.cpp's extendICUData) - the same 30 MiB again, out of the flexible memory pool (v00.13's web
    // process). Its directory is then one without the archive; everything ICU has is in the copy above.
    // Nor does it look for single data files before each item (stat and open calls that fail).
    if (U_SUCCESS(status)) {
        u_setDataDirectory(WISP_APP "/wisp/icu-data-is-in-memory");
        UErrorCode access_status = U_ZERO_ERROR;
        udata_setFileAccess(UDATA_ONLY_PACKAGES, &access_status);
    }
    // the iterator WebKit stopped on (grapheme clusters, the root locale)
    UErrorCode break_status = U_ZERO_ERROR;
    UBreakIterator *iterator = ubrk_open(UBRK_CHARACTER, "", nullptr, 0, &break_status);
    if (iterator)
        ubrk_close(iterator);
    wisp_log("ICU data: %s %s (%zu MiB, %.2f s; flexible memory %zu -> %zu MiB); %s; break iterator %s; "
        "ICU's own loader would have met: stat %s, mmap %s",
        U_ICUDATA_NAME ".dat", how, size >> 20, took, before >> 20, wisp_flexible_free() >> 20, u_errorName(status),
        iterator ? "ok" : u_errorName(break_status), stat_errno ? strerror(stat_errno) : "ok",
        map_errno ? strerror(map_errno) : "ok");
    if (U_SUCCESS(status))
        wisp_platform_setup();
}

// --- What pages read of the console: its time zone, its processor count --------------------------------

extern "C" int32_t sceSystemServiceParamGetInt(int32_t id, int32_t *value);

namespace {

// A zone ICU knows for an offset east of UTC in minutes: Etc/GMT-N for whole hours (Etc's signs are
// the other way round), else the place that keeps that offset
const char *zone_for(int minutes)
{
    static char etc[16];
    switch (minutes) {
    case 330: return "Asia/Kolkata";
    case 345: return "Asia/Kathmandu";
    case 270: return "Asia/Kabul";
    case 210: return "Asia/Tehran";
    case 390: return "Asia/Yangon";
    case 570: return "Australia/Darwin";
    case 630: return "Australia/Adelaide";
    case 765: return "Pacific/Chatham";
    case -210: return "America/St_Johns";
    case -150: return "America/St_Johns";
    case -570: return "Pacific/Marquesas";
    }
    const int hours = (minutes >= 0 ? minutes + 30 : minutes - 30) / 60;
    if (!hours)
        return "Etc/UTC";
    snprintf(etc, sizeof etc, "Etc/GMT%+d", -hours);
    return etc;
}

} // namespace

void wisp_platform_setup(void)
{
    // the console's time zone (Settings > Date and Time), daylight saving time included: JavaScript's
    // Date and Intl take ICU's default zone, the C library's localtime TZ (there is no /etc/localtime:
    // pages said UTC)
    int32_t zone = 0, summer = 0;
    const bool have_zone = sceSystemServiceParamGetInt(4 /* TIME_ZONE: minutes east of UTC */, &zone) == 0;
    if (have_zone && sceSystemServiceParamGetInt(5 /* SUMMERTIME */, &summer) == 0 && summer)
        zone += 60;
    if (have_zone && zone >= -14 * 60 && zone <= 14 * 60) {
        const char *id = zone_for(zone);
        UChar uid[32];
        int n = 0;
        for (; id[n] && n < 31; n++)
            uid[n] = static_cast<UChar>(id[n]);
        uid[n] = 0;
        UErrorCode zone_status = U_ZERO_ERROR;
        ucal_setDefaultTimeZone(uid, &zone_status);
        // POSIX: the offset west of UTC
        char tz[32];
        const int east = zone, h = (east >= 0 ? east : -east) / 60, m = (east >= 0 ? east : -east) % 60;
        if (m)
            snprintf(tz, sizeof tz, "<%c%02d%02d>%c%d:%02d", east >= 0 ? '+' : '-', h, m, east >= 0 ? '-' : '+', h, m);
        else
            snprintf(tz, sizeof tz, "<%c%02d>%c%d", east >= 0 ? '+' : '-', h, east >= 0 ? '-' : '+', h);
        setenv("TZ", tz, 1);
        tzset();
        wisp_log("time zone: UTC%+d:%02d%s (%s, TZ=%s)%s", east / 60, (east >= 0 ? east : -east) % 60,
            summer ? ", summer time" : "", id, tz, U_SUCCESS(zone_status) ? "" : " (ICU refused it)");
    } else {
        wisp_log("time zone: the console's could not be read (%d): UTC", static_cast<int>(zone));
    }
    // WTF's processor count (navigator.hardwareConcurrency, JavaScriptCore's collector threads): it has
    // no way to ask the PS4 and took 1; the app has 7 of the 8 cores
    setenv("WTF_numberOfProcessorCores", "7", 0);
    // A write to a socket the other end closed raises SIGPIPE, which ends a process: the sockets' code
    // (GLib's, aria2's) passes MSG_NOSIGNAL, but OpenOrbis' sys/socket.h has Linux's value for it
    // (FreeBSD's MSG_NBIO to this kernel), so the signal is never held back there
    signal(SIGPIPE, SIG_IGN);
}
