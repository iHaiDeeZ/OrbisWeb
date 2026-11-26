/* OrbisWeb (v00.35): the web process's sound, played by the UI process.
 *
 * Up to v00.34 the web process opened the PS4's audio out itself (child/gst_ps4_audiosink.c): the port
 * opened, took every block at its pace, said its sound went to the TV, and nobody heard a thing (every
 * page's sound since v00.26, the Settings page's tone in v00.34). The web process is a local process of the
 * app; the UI is the app's own process, where the player's sound (ui/player) comes from. Now the web
 * process still mixes its players, and writes each mixed block into this ring in shared memory, which the
 * UI made and handed it with START (common/wisp_handoff.cpp); the UI's audio thread (ui/audio_out.cpp)
 * takes the blocks at the audio out's pace and plays them. Settings: audio_out=web brings the old way back.
 *
 * One writer (the web process's mixer thread) and one reader (the UI's audio thread): each moves only its
 * own counter, with __atomic stores (release) and loads (acquire), so C and C++ read it the same way.
 */
#pragma once
#include <stdint.h>

#define WISP_AUDIO_RING_MAGIC 0x52445541u /* "AUDR" */
#define WISP_AUDIO_FRAMES 256             /* a block: 256 frames of 48 kHz 16-bit stereo (5.3 ms) */
#define WISP_AUDIO_RING_BLOCKS 16         /* room for 85 ms */
#define WISP_AUDIO_AHEAD 6                /* the writer stays at most this many blocks ahead (32 ms) */
#define WISP_AUDIO_RING_BYTES 32768       /* the mapping (whole 16 KiB kernel pages) */

typedef struct WispAudioRing {
    uint32_t magic;          /* WISP_AUDIO_RING_MAGIC */
    uint32_t blocks;         /* WISP_AUDIO_RING_BLOCKS */
    uint64_t written;        /* blocks the web process wrote: its counter alone */
    uint64_t read;           /* blocks the UI took: its counter alone */
    uint64_t ui_rounds;      /* the UI's audio thread counts its rounds (it is there and running) */
    uint32_t port_open;      /* the UI has the audio out open */
    uint32_t reserved[7];
    int16_t data[WISP_AUDIO_RING_BLOCKS][WISP_AUDIO_FRAMES * 2];
} WispAudioRing;

#ifdef __cplusplus
static_assert(sizeof(WispAudioRing) <= WISP_AUDIO_RING_BYTES, "the ring fits its mapping");
extern "C" {
#endif
/* web process: the UI's ring, mapped after the handoff (child/child_main.cpp); NULL: this process plays
   its sound on an audio out port of its own */
extern WispAudioRing *wisp_audio_ring;
#ifdef __cplusplus
}
#endif
