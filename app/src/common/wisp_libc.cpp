// Wisp: C library functions that the OpenOrbis C library only has as stubs, where WebKit relies on
// them. Linked into all three processes (an object file's definition wins over the archive's stub).
//
// uname: the stub fails with ENOSYS and leaves the struct alone. WebKit's default user agent
// (WebCore/platform/glib/UserAgentGLib.cpp) does not check, so it was built from stack garbage and
// WebKit stopped the UI on the invalid header value; every webkit_settings_new() builds it before
// the app sets its own (v00.02). GLib (g_get_os_info) and Mesa (RADV's debug dumps) read it too.
// The PS4's kernel is FreeBSD 9 on x86-64, so that is what it says.
//
// link: the stub fails with ENOSYS. WebKit's disk cache (NetworkCache's BlobStorage) keeps each body
// once and hard-links it into every record that has it: "Failed to create hard link" for every
// resource it cached (v00.05), and the records lost their bodies. libkernel exports no link to apps,
// so this is FreeBSD's system call 9, made directly (as WinePS4's port makes the calls libkernel lacks:
// an error comes back with the carry flag set and the error number in rax). If the kernel refuses
// links there, the file is copied: each record then has its own copy of the body.
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {
void put(char *field, size_t size, const char *value)
{
    strncpy(field, value, size - 1);
    field[size - 1] = '\0';
}

long raw_syscall2(long number, long a, long b, bool &failed)
{
    long ret;
    unsigned char carry;
    __asm__ volatile("syscall\n\tsetc %1"
                     : "=a"(ret), "=qm"(carry)
                     : "a"(number), "D"(a), "S"(b)
                     : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
    failed = carry != 0;
    return ret;
}

bool copy_file(const char *from, const char *to)
{
    const int in = open(from, O_RDONLY);
    if (in < 0)
        return false;
    const int out = open(to, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        const int error = errno;
        close(in);
        errno = error;
        return false;
    }
    char buffer[65536];
    bool ok = true;
    for (;;) {
        const ssize_t n = read(in, buffer, sizeof buffer);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = write(out, buffer + done, static_cast<size_t>(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            done += w;
        }
        if (!ok)
            break;
    }
    const int error = errno;
    close(in);
    close(out);
    if (!ok) {
        unlink(to);
        errno = error;
    }
    return ok;
}
}

extern "C" int link(const char *from, const char *to)
{
    if (!from || !to) {
        errno = EFAULT;
        return -1;
    }
    bool failed = false;
    const long result = raw_syscall2(9 /* SYS_link */, reinterpret_cast<long>(from), reinterpret_cast<long>(to), failed);
    if (!failed)
        return 0;
    const int error = static_cast<int>(result);
    // the caller's mistakes stay theirs; anything else (EPERM, ENOSYS, EOPNOTSUPP, EXDEV, ...) is the
    // kernel or the file system refusing links: a copy instead
    if (error == EEXIST || error == ENOENT || error == ENOTDIR || error == ENAMETOOLONG || error == EFAULT) {
        errno = error;
        return -1;
    }
    if (copy_file(from, to))
        return 0;
    return -1;
}

extern "C" int uname(struct utsname *name)
{
    if (!name) {
        errno = EFAULT;
        return -1;
    }
    memset(name, 0, sizeof *name);
    put(name->sysname, sizeof name->sysname, "FreeBSD");
    put(name->nodename, sizeof name->nodename, "ps4");
    put(name->release, sizeof name->release, "9.0-RELEASE");
    put(name->version, sizeof name->version, "PlayStation 4");
    put(name->machine, sizeof name->machine, "amd64");
    return 0;
}
