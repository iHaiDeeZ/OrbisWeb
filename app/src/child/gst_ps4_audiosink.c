/* OrbisWeb: GStreamer's audio sink for the PS4 (the web process: <video> and <audio>, Web Audio).
 *
 * The PS4's audio out takes 48 kHz 16-bit stereo in blocks of a fixed size and blocks the caller until
 * the block before has started; GStreamer's audio ring buffer gives this sink whole segments of the
 * same size from its own thread. Registered as a static plugin ("ps4audio", element "ps4audiosink")
 * with a rank over every other sink, so autoaudiosink (what WebKit asks for) takes it: the web process
 * calls gst_plugin_ps4audio_register() after gst_init (child/child_main.cpp).
 *
 * One port for every player (v00.26). Up to v00.25 each sink opened a port of its own as its player
 * got ready, and the PS4 gives an app 8: YouTube's home page readies a player for each preview it may
 * play, so the video then chosen found none free ("sceAudioOutOpen: 0x80260005"), its sink failed,
 * and WebKit ended its player: no video at all. Now each sink queues its blocks (a "voice") and one
 * mixer thread adds up whatever the voices have, block by block, into one port, opened when there is
 * sound to play and closed after a while without any. Without a port (none to be had) the mixer keeps
 * the port's time, so players go on in silence and get sound once one opens.
 *
 * Played from the UI (v00.35): this process is a local process of the app, and its port opened, took
 * every block and played nothing anyone heard (v00.26 to v00.34). When the UI handed this process a ring
 * (common/wisp_audio.h; settings audio_out=ui, the default) the mixer writes each mixed block there
 * instead, and the UI's audio thread (ui/audio_out.cpp) plays it; the ring's room is the pace.
 */
/* (GST_PLUGIN_DEFINE names the plugin's package with PACKAGE, as a plugin's config.h would define it) */
#define PACKAGE "wisp"
#include "../common/wisp_audio.h"
#include <gst/audio/gstaudiosink.h>
#include <gst/gst.h>
#include <math.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <stdlib.h>
#include <string.h>

#define WISP_TYPE_PS4_AUDIO_SINK (wisp_ps4_audio_sink_get_type())
G_DECLARE_FINAL_TYPE(WispPs4AudioSink, wisp_ps4_audio_sink, WISP, PS4_AUDIO_SINK, GstAudioSink)

#define FRAMES 256                   /* a block: 256 frames, 5.3 ms */
#define BLOCK_BYTES (FRAMES * 4)
#define BLOCK_US (FRAMES * G_USEC_PER_SEC / 48000)
#define VOICE_BLOCKS 4               /* each sink's queue in front of the mixer: 21 ms */
#define IDLE_CLOSE_US (3 * G_USEC_PER_SEC)
#define REPORT_US (10 * G_USEC_PER_SEC)  /* (v00.34) the port's line in web.log while it plays */

/* (common/wisp_log.cpp, the kit) */
void wisp_log(const char *format, ...);
void ps4_kit_set_thread_priority(int priority);

/* --- The mixer: one port, every sink's voice ------------------------------------------------------- */

typedef struct Voice {
    guint8 blocks[VOICE_BLOCKS][BLOCK_BYTES];
    guint head, count;               /* the oldest queued block; how many are queued */
    guint generation;                /* a reset (flush, stop) bumps it: a write waiting for room gives up */
    guint64 mixed, silent;           /* blocks the mixer took, and how many of them were all zeros */
    gint64 started_at;
    char name[48];                   /* (v00.34) the pipeline it plays for: whose sound it is */
    guint32 peak;                    /* the loudest sample since the last report (0-32768) */
    struct Voice *next;
} Voice;

static struct {
    GMutex lock;
    GCond more;                      /* a voice has a block for the mixer */
    GCond room;                      /* the mixer took blocks: the voices have room */
    Voice *voices;
    GThread *thread;
    /* the port (the mixer thread's alone) */
    int port, user;
    gint64 opened_at, last_sound, next_try;
    guint64 port_blocks;
    int failures, first_error;
    /* (v00.34) what the port did since its last line: blocks out, the loudest output sample */
    gint64 report_at;
    guint64 report_blocks;
    guint32 out_peak;
    int log;                         /* WISP_AUDIO_LOG (settings: audio_log): the lines; 0 keeps them out */
} M = { .port = -1, .log = 1 };

G_STATIC_ASSERT(FRAMES == WISP_AUDIO_FRAMES);

/* --- (v00.35) The UI's ring: the UI plays the mixed sound ------------------------------------------ */

static gboolean ring_stalled;        /* the UI took nothing for 200 ms: blocks go until it takes again */

/* The mixed block into the UI's ring, waiting while the ring is WISP_AUDIO_AHEAD blocks ahead (the UI
   takes one a block's time: the port's pace). FALSE when the UI is not taking any (the block goes). */
static gboolean ring_put(const gint16 *block)
{
    WispAudioRing *ring = wisp_audio_ring;
    const guint64 written = ring->written;   /* (this thread's own counter) */
    gint64 since = 0;
    for (;;) {
        const guint64 read = __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE);
        if (written - read < WISP_AUDIO_AHEAD) {
            if (ring_stalled) {
                ring_stalled = FALSE;
                wisp_log("audio: the UI takes this process's sound again");
            }
            break;
        }
        if (ring_stalled)
            return FALSE;
        const gint64 now = g_get_monotonic_time();
        if (!since)
            since = now;
        else if (now - since > 200 * 1000) {
            ring_stalled = TRUE;
            wisp_log("audio: the UI took no sound for 200 ms (its audio thread's rounds: %llu): this process's sound goes unheard until it takes some",
                (unsigned long long)__atomic_load_n(&ring->ui_rounds, __ATOMIC_RELAXED));
            return FALSE;
        }
        g_usleep(1000);
    }
    memcpy(ring->data[written % WISP_AUDIO_RING_BLOCKS], block, BLOCK_BYTES);
    __atomic_store_n(&ring->written, written + 1, __ATOMIC_RELEASE);
    return TRUE;
}

/* blocks in the UI's ring it has not played yet (0 without a ring) */
static guint ring_queued(void)
{
    WispAudioRing *ring = wisp_audio_ring;
    if (!ring)
        return 0;
    const guint64 written = __atomic_load_n(&ring->written, __ATOMIC_ACQUIRE);
    const guint64 read = __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE);
    return written > read ? (guint)MIN(written - read, (guint64)WISP_AUDIO_RING_BLOCKS) : 0;
}

/* a peak (0-32768) in decibels below full scale, or "silent" */
static const char *level(guint32 peak, char *text, size_t size)
{
    if (!peak)
        g_strlcpy(text, "silent", size);
    else
        g_snprintf(text, size, "%.0f dBFS", 20.0 * log10((double)peak / 32768.0));
    return text;
}

/* Where the port's sound goes and how loud the system plays it (sceAudioOutGetPortState: output 1, 2, 4
   the TV's connections, 0x40 headphones; volume -1 when the port has none of its own) */
static void port_state_line(int port, const char *when)
{
    /* (room past the 32 bytes the SDK's struct has, should a firmware's be longer; its first bytes raw too) */
    union {
        struct {
            guint16 output;
            guint8 channel, reserved;
            gint16 volume;
            guint16 reroutes;
            guint64 flag;
            guint64 reserved2[2];
        } s;
        guint8 raw[128];
    } state;
    memset(&state, 0, sizeof state);
    const int result = sceAudioOutGetPortState(port, &state);
    if (result < 0) {
        wisp_log("audio: port %d %s: its state unknown (sceAudioOutGetPortState: 0x%08x)", port, when, (unsigned)result);
        return;
    }
    char raw[3 * 16 + 1] = "";
    for (int i = 0; i < 16; i++)
        g_snprintf(raw + 3 * i, sizeof raw - 3 * i, "%02x ", state.raw[i]);
    wisp_log("audio: port %d %s: output 0x%x, channel %u, volume %d, reroutes %u, flags 0x%llx (%s)", port, when,
        state.s.output, state.s.channel, state.s.volume, state.s.reroutes, (unsigned long long)state.s.flag, raw);
}

static void port_open(void)
{
    /* the user who started the app; the system's own id (255) when this process's user service has none
       (a main port opens for either) */
    OrbisUserServiceUserId user = -1;
    int port = -1;
    if (!sceUserServiceGetInitialUser(&user) && user >= 0)
        port = sceAudioOutOpen(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, FRAMES, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    else
        user = -1;
    if (port < 0) {
        port = sceAudioOutOpen(255, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, FRAMES, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
        user = 255;
    }
    if (port < 0) {
        if (!M.failures++)
            wisp_log("audio: the PS4's audio out did not open (sceAudioOutOpen: 0x%08x): players go on without sound, "
                "and it is tried again every second", (unsigned)port);
        M.next_try = g_get_monotonic_time() + G_USEC_PER_SEC;
        return;
    }
    M.port = port;
    M.user = user;
    M.opened_at = g_get_monotonic_time();
    M.port_blocks = 0;
    M.first_error = 0;
    M.report_at = M.opened_at;
    M.report_blocks = 0;
    M.out_peak = 0;
    wisp_log("audio: the PS4's audio out open (port %d, %s, 48 kHz stereo; every player's sound mixed into it)%s", port,
        user == 255 ? "the system's user" : "the console's user", M.failures ? " after failing before" : "");
    M.failures = 0;
    /* (v00.34) full volume on both channels, said rather than assumed (0 dB: 32768), and where it plays */
    int32_t volume[8] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768 };
    const int set = sceAudioOutSetVolume(port, (1 << 0) | (1 << 1), volume);
    if (set < 0)
        wisp_log("audio: port %d: sceAudioOutSetVolume failed (0x%08x)", port, (unsigned)set);
    if (M.log)
        port_state_line(port, "opened");
}

static void port_close(const char *why)
{
    if (M.port < 0)
        return;
    sceAudioOutOutput(M.port, NULL); /* (waits for what is queued) */
    sceAudioOutClose(M.port);
    wisp_log("audio: port %d closed (%s) after %.1f s: %llu blocks played", M.port, why,
        (double)(g_get_monotonic_time() - M.opened_at) / 1e6, (unsigned long long)M.port_blocks);
    if (M.first_error)
        wisp_log("audio: sceAudioOutOutput failed: 0x%08x", (unsigned)M.first_error);
    M.port = -1;
}

/* (with M.lock held) the port's line: blocks out since the last one against the port's rate (187.5 a
   second), the loudest output sample, and each player's loudest; the peaks start again */
static void report(gint64 now)
{
    char voices[512] = "", peak[24];
    size_t length = 0;
    for (Voice *v = M.voices; v && length < sizeof voices - 64; v = v->next) {
        const int wrote = g_snprintf(voices + length, sizeof voices - length, "%s%s %s", length ? ", " : "", v->name,
            level(v->peak, peak, sizeof peak));
        if (wrote < 0)
            break;
        length += (size_t)wrote;
        v->peak = 0;
    }
    const double seconds = (double)(now - M.report_at) / 1e6;
    const guint64 blocks = M.report_blocks;
    const guint32 out_peak = M.out_peak;
    const int port = M.port;
    M.report_at = now;
    M.report_blocks = 0;
    M.out_peak = 0;
    g_mutex_unlock(&M.lock);
    if (wisp_audio_ring)
        wisp_log("audio: to the UI: %llu blocks in %.1f s (%.0f%% of the port's rate), %u in the ring, the UI %s, output %s; players: %s",
            (unsigned long long)blocks, seconds, seconds > 0 ? 100.0 * (double)blocks / (seconds * 187.5) : 0.0, ring_queued(),
            __atomic_load_n(&wisp_audio_ring->port_open, __ATOMIC_RELAXED) ? "playing it" : "without a port",
            level(out_peak, peak, sizeof peak), length ? voices : "none");
    else
        wisp_log("audio: port %d: %llu blocks in %.1f s (%.0f%% of its rate), output %s; players: %s", port,
            (unsigned long long)blocks, seconds, seconds > 0 ? 100.0 * (double)blocks / (seconds * 187.5) : 0.0,
            level(out_peak, peak, sizeof peak), length ? voices : "none");
    g_mutex_lock(&M.lock);
}

static gpointer mixer_main(gpointer unused)
{
    (void)unused;
    ps4_kit_set_thread_priority(400);   /* (over WebKit's threads: a late block is a gap in the sound) */
    static gint32 sum[FRAMES * 2];
    static gint16 out[FRAMES * 2] __attribute__((aligned(64)));
    gint64 clock = 0;   /* without a port: when the next block would start */
    g_mutex_lock(&M.lock);
    for (;;) {
        gboolean any = FALSE;
        for (Voice *v = M.voices; v && !any; v = v->next)
            any = v->count != 0;
        if (!any) {
            /* nothing to play: the port goes after a while of that (other apps, and the PS4's own
               sounds, have it back) */
            if (M.port >= 0 && g_get_monotonic_time() - M.last_sound > IDLE_CLOSE_US) {
                g_mutex_unlock(&M.lock);
                port_close("no sound for 3 s");
                g_mutex_lock(&M.lock);
                continue;
            }
            g_cond_wait_until(&M.more, &M.lock, g_get_monotonic_time() + G_USEC_PER_SEC / 2);
            continue;
        }
        memset(sum, 0, sizeof sum);
        if (wisp_audio_ring && g_get_monotonic_time() - M.last_sound > G_USEC_PER_SEC) {
            /* (the UI's ring) sound again after a pause: the next line counts from here */
            M.report_at = g_get_monotonic_time();
            M.report_blocks = 0;
            M.out_peak = 0;
        }
        for (Voice *v = M.voices; v; v = v->next) {
            if (!v->count)
                continue;
            const gint16 *samples = (const gint16 *)v->blocks[v->head];
            gboolean silent = TRUE;
            guint32 peak = v->peak;
            for (int i = 0; i < FRAMES * 2; i++) {
                const gint32 s = samples[i];
                sum[i] += s;
                silent &= s == 0;
                const guint32 a = (guint32)(s < 0 ? -s : s);
                if (a > peak)
                    peak = a;
            }
            v->peak = peak;
            v->head = (v->head + 1) % VOICE_BLOCKS;
            v->count--;
            v->mixed++;
            v->silent += silent;
        }
        g_cond_broadcast(&M.room);
        g_mutex_unlock(&M.lock);
        guint32 out_peak = M.out_peak;   /* (the report fields: this thread's, under the lock or not) */
        for (int i = 0; i < FRAMES * 2; i++) {
            out[i] = (gint16)CLAMP(sum[i], -32768, 32767);
            const guint32 a = (guint32)(out[i] < 0 ? -(gint32)out[i] : out[i]);
            if (a > out_peak)
                out_peak = a;
        }
        M.out_peak = out_peak;
        const gint64 now = g_get_monotonic_time();
        gboolean paced = FALSE;   /* the block went at the port's pace (the port's wait, or the ring's room) */
        if (wisp_audio_ring) {
            /* (v00.35) the UI plays it */
            if (ring_put(out)) {
                M.port_blocks++;
                M.report_blocks++;
                paced = TRUE;
            }
        } else {
            if (M.port < 0 && now >= M.next_try)
                port_open();
            if (M.port >= 0) {
                /* blocks until the block before has started */
                const int result = sceAudioOutOutput(M.port, out);
                if (result < 0) {
                    if (!M.first_error)
                        M.first_error = result;
                    port_close("an output failed");
                    M.next_try = g_get_monotonic_time() + G_USEC_PER_SEC;
                } else {
                    M.port_blocks++;
                    M.report_blocks++;
                }
                paced = TRUE;
            }
        }
        if (paced) {
            clock = 0;
        } else {
            /* the port's pace without the port (or without the UI taking any) */
            if (!clock || now - clock > 100 * 1000)
                clock = now;
            clock += BLOCK_US;
            if (clock > now)
                g_usleep((gulong)(clock - now));
        }
        g_mutex_lock(&M.lock);
        M.last_sound = g_get_monotonic_time();
        if (M.log && (M.port >= 0 || wisp_audio_ring) && M.last_sound - M.report_at >= REPORT_US)
            report(M.last_sound);
    }
    return NULL;
}

/* --- The sink: a voice of the mixer ---------------------------------------------------------------- */

struct _WispPs4AudioSink {
    GstAudioSink parent;
    Voice *voice;                    /* while prepared */
    guint8 block[BLOCK_BYTES];       /* a block being filled from GStreamer's segments */
    guint filled;
};

G_DEFINE_TYPE(WispPs4AudioSink, wisp_ps4_audio_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, format = (string) S16LE, layout = (string) interleaved, rate = (int) 48000, channels = (int) 2"));

static gboolean sink_open(GstAudioSink *base)
{
    static gboolean initialized = FALSE;
    (void)base;
    if (!initialized) {
        sceAudioOutInit(); /* (an error when the process did it before: harmless) */
        const char *log = getenv("WISP_AUDIO_LOG");
        M.log = !log || strcmp(log, "0") != 0;
        initialized = TRUE;
    }
    return TRUE;
}

/* the name of the pipeline an element is in (WebKit names its players' and Web Audio's pipelines) */
static void pipeline_name(GstObject *element, char *name, size_t size)
{
    GstObject *at = gst_object_ref(element);
    GstObject *parent;
    while ((parent = gst_object_get_parent(at))) {
        gst_object_unref(at);
        at = parent;
    }
    gchar *text = gst_object_get_name(at);
    g_strlcpy(name, text ? text : "?", size);
    g_free(text);
    gst_object_unref(at);
}

static gboolean sink_prepare(GstAudioSink *base, GstAudioRingBufferSpec *spec)
{
    WispPs4AudioSink *self = WISP_PS4_AUDIO_SINK(base);
    Voice *voice = g_new0(Voice, 1);
    voice->started_at = g_get_monotonic_time();
    pipeline_name(GST_OBJECT(base), voice->name, sizeof voice->name);
    if (M.log)
        wisp_log("audio: a player's sound starts (%s: %d Hz, %d channels)", voice->name, GST_AUDIO_INFO_RATE(&spec->info),
            GST_AUDIO_INFO_CHANNELS(&spec->info));
    g_mutex_lock(&M.lock);
    voice->next = M.voices;
    M.voices = voice;
    if (!M.thread)
        M.thread = g_thread_new("wisp-audio", mixer_main, NULL);
    g_mutex_unlock(&M.lock);
    self->voice = voice;
    self->filled = 0;
    /* the ring buffer's segments: one block each, 8 of them (43 ms) */
    spec->segsize = BLOCK_BYTES;
    spec->segtotal = 8;
    return TRUE;
}

static gboolean sink_unprepare(GstAudioSink *base)
{
    WispPs4AudioSink *self = WISP_PS4_AUDIO_SINK(base);
    Voice *voice = self->voice;
    if (!voice)
        return TRUE;
    g_mutex_lock(&M.lock);
    /* what is queued plays out first (21 ms at most) */
    const gint64 until = g_get_monotonic_time() + 50 * 1000;
    while (voice->count && g_cond_wait_until(&M.room, &M.lock, until))
        ;
    for (Voice **at = &M.voices; *at; at = &(*at)->next) {
        if (*at == voice) {
            *at = voice->next;
            break;
        }
    }
    g_mutex_unlock(&M.lock);
    if (voice->mixed)
        wisp_log("audio: a player's sound ended (%s) after %.1f s: %llu blocks mixed, %llu of them silent%s", voice->name,
            (double)(g_get_monotonic_time() - voice->started_at) / 1e6, (unsigned long long)voice->mixed,
            (unsigned long long)voice->silent, voice->silent == voice->mixed ? " (all: the page sent silence, muted?)" : "");
    g_free(voice);
    self->voice = NULL;
    return TRUE;
}

static gboolean sink_close(GstAudioSink *base)
{
    (void)base;
    return TRUE;
}

/* a whole block to the voice's queue, waiting for room; FALSE: a reset came meanwhile (the block goes) */
static gboolean queue_block(Voice *voice, const guint8 *block)
{
    g_mutex_lock(&M.lock);
    const guint generation = voice->generation;
    while (voice->count == VOICE_BLOCKS && voice->generation == generation)
        g_cond_wait(&M.room, &M.lock);
    const gboolean queued = voice->generation == generation;
    if (queued) {
        memcpy(voice->blocks[(voice->head + voice->count) % VOICE_BLOCKS], block, BLOCK_BYTES);
        voice->count++;
        g_cond_signal(&M.more);
    }
    g_mutex_unlock(&M.lock);
    return queued;
}

static gint sink_write(GstAudioSink *base, gpointer data, guint length)
{
    WispPs4AudioSink *self = WISP_PS4_AUDIO_SINK(base);
    const guint8 *bytes = data;
    guint done = 0;
    if (!self->voice)
        return (gint)length;
    while (done < length) {
        const guint take = MIN(length - done, BLOCK_BYTES - self->filled);
        memcpy(self->block + self->filled, bytes + done, take);
        self->filled += take;
        done += take;
        if (self->filled == BLOCK_BYTES) {
            self->filled = 0;
            if (!queue_block(self->voice, self->block))
                break;   /* reset: the rest of this write goes too */
        }
    }
    return (gint)length;
}

static guint sink_delay(GstAudioSink *base)
{
    WispPs4AudioSink *self = WISP_PS4_AUDIO_SINK(base);
    /* frames written but not heard yet: the partial block, this voice's queue, the UI's ring (v00.35), and
       about two blocks in the audio out (the one playing, the one waiting) */
    guint queued = 0;
    if (self->voice) {
        g_mutex_lock(&M.lock);
        queued = self->voice->count;
        g_mutex_unlock(&M.lock);
    }
    return self->filled / 4 + queued * FRAMES + (ring_queued() + 2) * FRAMES;
}

static void sink_reset(GstAudioSink *base)
{
    WispPs4AudioSink *self = WISP_PS4_AUDIO_SINK(base);
    if (self->voice) {
        g_mutex_lock(&M.lock);
        self->voice->count = 0;
        self->voice->generation++;
        g_cond_broadcast(&M.room);
        g_mutex_unlock(&M.lock);
    }
    self->filled = 0;
}

static void wisp_ps4_audio_sink_class_init(WispPs4AudioSinkClass *klass)
{
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *sink_class = GST_AUDIO_SINK_CLASS(klass);
    gst_element_class_set_static_metadata(element_class, "PS4 audio out", "Sink/Audio", "Plays sound on the PS4's audio out", "OrbisWeb");
    gst_element_class_add_static_pad_template(element_class, &sink_template);
    sink_class->open = sink_open;
    sink_class->prepare = sink_prepare;
    sink_class->unprepare = sink_unprepare;
    sink_class->close = sink_close;
    sink_class->write = sink_write;
    sink_class->delay = sink_delay;
    sink_class->reset = sink_reset;
}

static void wisp_ps4_audio_sink_init(WispPs4AudioSink *self)
{
    self->voice = NULL;
    self->filled = 0;
}

static gboolean plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "ps4audiosink", GST_RANK_PRIMARY + 100, WISP_TYPE_PS4_AUDIO_SINK);
}

#define PACKAGE "wisp"
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, ps4audio, "The PS4's audio out", plugin_init, "1.0", "LGPL", "Wisp", "https://localhost/")
