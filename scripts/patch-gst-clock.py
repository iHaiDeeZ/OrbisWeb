#!/usr/bin/env python3
# OrbisWeb v00.38: GStreamer's system clock entries on the PS4 kernel's umtx words instead of a pthread
# condition variable, its attribute object and a mutex each. Idempotent; run by build-media.sh (b_gstreamer):
#   python3 patch-gst-clock.py <gstreamer source>/gst/gstsystemclock.c
import sys
from pathlib import Path

path = Path(sys.argv[1])
s = path.read_text()
anchor = "#if defined(HAVE_FUTEX) || defined(HAVE_FUTEX_TIME64)\n#include <unistd.h>\n#include <linux/futex.h>\n"
if "GST_PS4_UMTX_WAIT_UINT_PRIVATE" in s:
    print("gstsystemclock.c: PS4 entries already in")
    sys.exit(0)
assert s.count(anchor) == 1, "gstsystemclock.c: anchor not found"

ps4 = r'''#if defined(__PS4__)
/* PS4 (OrbisWeb v00.38): clock entries on the kernel's umtx words, through libkernel's _umtx_op
 * (FreeBSD's UMTX_OP_WAIT_UINT_PRIVATE 15 and UMTX_OP_WAKE_PRIVATE 16; OpenOrbis has no <sys/umtx.h>).
 *
 * The POSIX entries make a condition variable, its attribute object and a mutex for every entry. The
 * PS4's kernel library makes those from a small pool in each process (13 MiB), and making and destroying
 * them for every wait did not give it back: WebKit's Web Audio pipeline (clocksync: a new entry for every
 * 128-frame buffer, 375 a second) emptied the pool in about a minute of Star Fox 64 in EmulatorJS, then
 * pthread_mutex_init failed and g_error ended the web process. These entries are two words. */
int _umtx_op (void *obj, int op, unsigned long val, void *uaddr, void *uaddr2);
#define GST_PS4_UMTX_WAIT_UINT_PRIVATE 15
#define GST_PS4_UMTX_WAKE_PRIVATE 16

typedef struct
{
  guint val;                    /* 0 free, 1 locked, 2 locked and someone may wait */
} GstPs4Lock;

static inline void
gst_ps4_lock (GstPs4Lock * lock)
{
  guint c = 0;

  if (__atomic_compare_exchange_n (&lock->val, &c, 1, FALSE, __ATOMIC_ACQUIRE,
          __ATOMIC_RELAXED))
    return;
  if (c != 2)
    c = __atomic_exchange_n (&lock->val, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    _umtx_op (&lock->val, GST_PS4_UMTX_WAIT_UINT_PRIVATE, 2, NULL, NULL);
    c = __atomic_exchange_n (&lock->val, 2, __ATOMIC_ACQUIRE);
  }
}

static inline void
gst_ps4_unlock (GstPs4Lock * lock)
{
  if (__atomic_exchange_n (&lock->val, 0, __ATOMIC_RELEASE) == 2)
    _umtx_op (&lock->val, GST_PS4_UMTX_WAKE_PRIVATE, 1, NULL, NULL);
}

#define GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)          (&(entry)->lock)
#define GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry)          (&(entry)->cond_val)
#define GST_SYSTEM_CLOCK_ENTRY_LOCK(entry)              (gst_ps4_lock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_UNLOCK(entry)            (gst_ps4_unlock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_WAIT_UNTIL(entry,ns)     gst_ps4_cond_wait_until(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry),GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry),(ns))
#define GST_SYSTEM_CLOCK_ENTRY_BROADCAST(entry)         gst_ps4_cond_broadcast(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry))

#define CLOCK_MIN_WAIT_TIME 500 /* ns */

typedef struct _GstClockEntryPs4 GstClockEntryImpl;
struct _GstClockEntryPs4
{
  GstClockEntry entry;
  GWeakRef clock;
  GDestroyNotify destroy_entry;

  gboolean initialized;

  GstPs4Lock lock;
  guint cond_val;
};

static void
clear_entry (GstClockEntryImpl * entry)
{
}

static void
init_entry (GstClockEntryImpl * entry)
{
  entry->lock.val = 0;
  entry->cond_val = 0;
  entry->destroy_entry = (GDestroyNotify) clear_entry;
}

static void
gst_ps4_cond_broadcast (guint * cond_val)
{
  __atomic_add_fetch (cond_val, 1, __ATOMIC_RELEASE);
  _umtx_op (cond_val, GST_PS4_UMTX_WAKE_PRIVATE, G_MAXINT, NULL, NULL);
}

/* Called and returns with the entry's lock held. TRUE when a broadcast came, FALSE when end_time
 * (nanoseconds on g_get_monotonic_time's clock) passed: an early return of the kernel's wait (a
 * signal) waits again, as the POSIX entries' timed wait does. */
static gboolean
gst_ps4_cond_wait_until (guint * cond_val, GstPs4Lock * lock, gint64 end_time)
{
  const guint sampled = __atomic_load_n (cond_val, __ATOMIC_ACQUIRE);
  gboolean woken = FALSE;

  gst_ps4_unlock (lock);
  for (;;) {
    gint64 left;
    struct timespec relative;

    if (__atomic_load_n (cond_val, __ATOMIC_ACQUIRE) != sampled) {
      woken = TRUE;
      break;
    }
    left = end_time - g_get_monotonic_time () * 1000;
    if (left <= 0)
      break;
    relative.tv_sec = left / 1000000000;
    relative.tv_nsec = left % 1000000000;
    _umtx_op (cond_val, GST_PS4_UMTX_WAIT_UINT_PRIVATE, sampled,
        (void *) (guintptr) sizeof (relative), &relative);
  }
  gst_ps4_lock (lock);
  return woken;
}

#elif defined(HAVE_FUTEX) || defined(HAVE_FUTEX_TIME64)
#include <unistd.h>
#include <linux/futex.h>
'''
s = s.replace(anchor, ps4, 1)
path.write_text(s)
print("gstsystemclock.c: PS4 umtx entries added")
