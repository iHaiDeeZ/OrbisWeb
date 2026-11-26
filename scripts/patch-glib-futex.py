#!/usr/bin/env python3
# OrbisWeb v00.39: GLib's futex code on the PS4 kernel's umtx (libkernel's _umtx_op): its native GMutex and
# GCond (plain integers) and its bit locks, instead of a pthread object per GMutex/GCond and a GCond made and
# destroyed for every wait on a contended bit lock. Idempotent; run by build-deps.sh (b_glib):
#   python3 patch-glib-futex.py <glib source>
import sys
from pathlib import Path

src = Path(sys.argv[1])

# --- gthreadprivate.h: HAVE_FUTEX and g_futex_simple for the PS4 -------------------------------------------
h = src / "glib/gthreadprivate.h"
s = h.read_text()
if "g_futex_ps4" not in s:
    anchor = "#if defined(HAVE_FUTEX) || defined(HAVE_FUTEX_TIME64)\n#include <errno.h>\n#include <linux/futex.h>\n"
    assert s.count(anchor) == 1, "gthreadprivate.h anchor"
    ps4 = r'''#if defined(__PS4__)
/* PS4 (OrbisWeb v00.39): GLib's futex paths (the native GMutex and GCond, the bit locks) on the kernel's umtx,
 * through libkernel's _umtx_op (FreeBSD's UMTX_OP_WAIT_UINT_PRIVATE 15 and UMTX_OP_WAKE_PRIVATE 16; OpenOrbis
 * has neither <linux/futex.h> nor <sys/umtx.h>).
 *
 * Without futexes every GMutex and GCond was a pthread object, made on first use and destroyed by g_*_clear,
 * and every wait on a contended bit lock (GObject's weak references) made and destroyed a GCond. The PS4 makes
 * pthread objects from a small pool in each process and did not give it back: YouTube's playback emptied it
 * in 27 minutes (OrbisWeb v00.37), then pthread_cond_init failed and GLib aborted. */
#include <errno.h>
#include <time.h>
#define HAVE_FUTEX 1
#define FUTEX_WAIT_PRIVATE 0x80
#define FUTEX_WAKE_PRIVATE 0x81
int _umtx_op (void *obj, int op, unsigned long val, void *uaddr, void *uaddr2);

/* A wake, or a wait while *uaddr == val, for at most *timeout (relative) when given. Returns -1 with errno
 * ETIMEDOUT when a timed wait's time ran out (this C library's ETIMEDOUT: the kernel library's errno may
 * use FreeBSD's numbers), else 0: woken, interrupted, or the value had changed. */
static inline int
g_futex_ps4 (void *uaddr, gsize op, gsize val, const struct timespec *timeout)
{
  struct timespec start, now, relative;
  gint64 waited_ns, limit_ns;

  if (op == FUTEX_WAKE_PRIVATE)
    {
      _umtx_op (uaddr, 16, (unsigned long) val, NULL, NULL);
      return 0;
    }
  if (timeout == NULL)
    {
      _umtx_op (uaddr, 15, (unsigned long) (guint) val, NULL, NULL);
      return 0;
    }
  relative = *timeout;
  clock_gettime (CLOCK_MONOTONIC, &start);
  _umtx_op (uaddr, 15, (unsigned long) (guint) val, (void *) (gsize) sizeof (relative), &relative);
  clock_gettime (CLOCK_MONOTONIC, &now);
  waited_ns = (gint64) (now.tv_sec - start.tv_sec) * 1000000000 + (now.tv_nsec - start.tv_nsec);
  limit_ns = (gint64) timeout->tv_sec * 1000000000 + timeout->tv_nsec;
  if (waited_ns >= limit_ns)
    {
      errno = ETIMEDOUT;
      return -1;
    }
  return 0;
}

#define g_futex_simple(uaddr, futex_op, ...)                                  \
  G_STMT_START                                                                \
  {                                                                           \
    int saved_errno = errno;                                                  \
    (void) g_futex_ps4 ((void *) (uaddr), (gsize) (futex_op), __VA_ARGS__);   \
    errno = saved_errno;                                                      \
  }                                                                           \
  G_STMT_END

#elif defined(HAVE_FUTEX) || defined(HAVE_FUTEX_TIME64)
#include <errno.h>
#include <linux/futex.h>
'''
    s = s.replace(anchor, ps4, 1)
    h.write_text(s)
    print("gthreadprivate.h: PS4 futex")
else:
    print("gthreadprivate.h: already done")

# --- gthread-posix.c: the timed GCond wait --------------------------------------------------------------
c = src / "glib/gthread-posix.c"
s = c.read_text()
if "g_futex_ps4 (&cond->i[0]" not in s:
    anchor = """  sampled = cond->i[0];
  g_mutex_unlock (mutex);

#if defined(HAVE_FUTEX_TIME64)
"""
    assert s.count(anchor) == 1, "g_cond_wait_until_impl anchor"
    new = """  sampled = cond->i[0];
  g_mutex_unlock (mutex);

#if defined(__PS4__)
  /* PS4 (OrbisWeb v00.39): the kernel's umtx, relative time (gthreadprivate.h) */
  res = g_futex_ps4 (&cond->i[0], (gsize) FUTEX_WAIT_PRIVATE, (gsize) sampled, &span);
  success = (res < 0 && errno == ETIMEDOUT) ? FALSE : TRUE;
  g_mutex_lock (mutex);

  return success;
#endif

#if defined(HAVE_FUTEX_TIME64)
"""
    s = s.replace(anchor, new, 1)
    old_guard = """#if defined(HAVE_FUTEX)
  {
#  ifdef __kernel_long_t"""
    assert s.count(old_guard) == 1, "syscall block anchor"
    s = s.replace(old_guard, """#if defined(HAVE_FUTEX) && !defined(__PS4__)
  {
#  ifdef __kernel_long_t""", 1)
    tail_old = """#  undef KERNEL_SPAN_SEC_TYPE
#endif /* defined(HAVE_FUTEX) */

  /* We can't end up here because of the checks above */
  g_assert_not_reached ();"""
    assert s.count(tail_old) == 1, "tail anchor"
    s = s.replace(tail_old, """#  undef KERNEL_SPAN_SEC_TYPE
#endif /* defined(HAVE_FUTEX) && !defined(__PS4__) */

#if !defined(__PS4__)
  /* We can't end up here because of the checks above */
  g_assert_not_reached ();
#endif""", 1)
    c.write_text(s)
    print("gthread-posix.c: PS4 timed wait")
else:
    print("gthread-posix.c: already done")
