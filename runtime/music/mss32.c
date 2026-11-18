/* Miles Sound System 6 stand-in (mss32.dll): the AIL functions the ports' games import (Off-Road
   64 of Miles 6.0m, Enter the Matrix 55 of Miles 6.5, LEGO Island 2 73 of Miles 6 with MIDI),
   played through the runtime's mixer (dsound.c's runtime voices, sound.h). From offroad-native's
   copy (itself from etm-native's, from li2-native's), with li2's MIDI, DLS and sequence calls
   (midi.c). Miles 6.0 takes volumes and pans as integers 0..127 (pan 64 centre), 6.5 as floats
   0..1 (pan 0.5 centre); each version's calls have their own names here, so one file serves both.

   What a game typically does: AIL_startup, AIL_waveOutOpen or AIL_open_digital_driver,
   AIL_enumerate_3D_providers / AIL_open_3D_provider (only "Miles Fast 2D Positional Audio" is
   offered: no EAX attributes), sample handles filled with AIL_set_sample_file /
   AIL_set_3D_sample_file from WAV images in memory (PCM or IMA ADPCM), AIL_file_read for files,
   AIL_open_stream for music (WAV and MP3 files, decoded with FFmpeg), MIDI sequences through a
   DLS collection (midi.c).

   A port can sort samples into classes with their own volume (its options menu): music is class
   1 (streams), effects 0, and port_miles_class(path, previous) gives a sample file another
   (Off-Road: its engines, class 2); miles_set_class_gain sets a class's level. */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "runtime.h"
/* Streams (AIL_open_stream: the music) decode through FFmpeg, any format; with
   PORT_MILES_NO_FFMPEG (port.h) there is no FFmpeg dependency and streams play WAV (PCM) and, if
   the port links runtime/music/stb_vorbis_impl.c, Ogg Vorbis (Enter the Matrix's Deck copy) */
#if !defined(PORT_MILES_NO_FFMPEG)
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#endif
#include "sound.h"
#include "options.h"

#define LOGS(...) LOG(LOG_SOUND, __VA_ARGS__)

enum { SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4, SMP_STOPPED = 8 };
enum { K_DRIVER = 1, K_SAMPLE, K_3DSAMPLE, K_PROVIDER, K_LISTENER, K_STREAM, K_MIDI, K_DLS, K_SEQUENCE };
/* Miles sample types (AIL_set_sample_type) */
enum { DIG_F_16BITS = 1, DIG_F_STEREO = 2, DIG_F_ADPCM = 4 };
#define MAGIC 0x53534d4cu                    /* "LMSS" */
#define MAXOBJ 512

typedef struct Obj {
    int kind, used;
    uint32_t handle;                    /* the program's handle */
    int voice;                          /* runtime voice, -1 none */
    int ended;                          /* played to its end: the position stays at the end */
    int cls;                            /* its sample's class (the port's volume levels) */
    int midi;                           /* sequences: midi.c's sequence, -1 none */
    uint32_t callback;                  /* streams: AIL_register_stream_callback's */
    int16_t *decoded;                   /* ADPCM decoded for the voice (owned) */
    uint32_t dec_frames;
    int status;
    /* what to play */
    uint32_t addr, len;                 /* the data in program memory */
    int type;                           /* DIG_F_* */
    uint32_t block;                     /* ADPCM block size */
    uint32_t rate;                      /* playback rate */
    int loops;                          /* 1 once, 0 forever, n passes */
    float volume, pan;                  /* 0..1, pan 0.5 centre */
    float send;                         /* reverb send: 3D samples by EAX's reverb mix, 2D by
                                           AIL_set_sample_reverb_levels (none by default) */
    /* 3D */
    float pos[3], front[3], up[3];
    float maxd, mind;
} Obj;

static Obj objs[MAXOBJ];
static uint32_t redist_dir, last_error_str;
static int listener = -1;
static float rolloff = 1.0f, master = 1.0f;
static uint32_t prefs[64];

/* ------------------------------------------------------------------ handles */
static uint32_t obj_new(int kind)
{
    for (int i = 1; i < MAXOBJ; i++)
        if (!objs[i].used) {
            Obj *o = &objs[i];
            uint32_t h = o->handle;
            memset(o, 0, sizeof *o);
            if (!h)
                h = galloc(64);
            memset(gptr(h), 0, 64);
            W32(h, MAGIC);
            W32(h + 4, (uint32_t)i);
            o->handle = h;
            o->used = 1;
            o->kind = kind;
            o->voice = -1;
            o->status = SMP_DONE;
            o->volume = 1.0f;
            o->pan = 0.5f;
            o->loops = 1;
            o->rate = 22050;
            o->type = DIG_F_16BITS;
            o->maxd = 100.0f;
            o->mind = 1.0f;
            o->front[2] = 1.0f;
            o->midi = -1;
            o->up[1] = 1.0f;
            return h;
        }
    fatal("Miles stand-in: out of handles");
}

static Obj *obj_get(uint32_t h, int kind)
{
    if (!h || R32(h) != MAGIC)
        return NULL;
    uint32_t i = R32(h + 4);
    if (i == 0 || i >= MAXOBJ || !objs[i].used || (kind && objs[i].kind != kind))
        return NULL;
    return &objs[i];
}
/* a 2D or 3D sample */
static Obj *smp_get(uint32_t h)
{
    Obj *o = obj_get(h, 0);
    return o && (o->kind == K_SAMPLE || o->kind == K_3DSAMPLE) ? o : NULL;
}

static float fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fu(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ------------------------------------------------------------------ IMA ADPCM */
static const int ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
    80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499,
    2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static int16_t ima_nibble(int nib, int *pred, int *idx)
{
    int step = ima_step[*idx], diff = step >> 3;
    if (nib & 4) diff += step;
    if (nib & 2) diff += step >> 1;
    if (nib & 1) diff += step >> 2;
    *pred += nib & 8 ? -diff : diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *idx += ima_index[nib];
    if (*idx < 0) *idx = 0;
    if (*idx > 88) *idx = 88;
    return (int16_t)*pred;
}

/* frames a block holds: the header's sample and two a byte after the headers */
static uint32_t ima_per_block(uint32_t align, uint32_t ch)
{
    return align > 4 * ch ? (align - 4 * ch) * 2 / ch + 1 : 0;
}

/* IMA ADPCM (Microsoft's block layout) to 16-bit PCM; returns the frames written (a last partial
   block is decoded as far as it goes) */
static uint32_t ima_decode(const uint8_t *data, uint32_t n, uint32_t align, uint32_t ch, int16_t *d)
{
    uint32_t per = ima_per_block(align, ch);
    if (!per || ch < 1 || ch > 2)
        return 0;
    uint32_t frames = 0;
    for (uint32_t off = 0; off + 4 * ch <= n; off += align) {
        const uint8_t *q = data + off, *qend = data + (off + align < n ? off + align : n);
        int pred[2], idx[2];
        for (uint32_t c = 0; c < ch; c++) {
            pred[c] = (int16_t)(q[4 * c] | q[4 * c + 1] << 8);
            idx[c] = q[4 * c + 2] > 88 ? 88 : q[4 * c + 2];
            d[c] = (int16_t)pred[c];
        }
        d += ch;
        frames++;
        q += 4 * ch;
        /* then groups of 4 bytes (8 frames) per channel, the channels taking turns */
        for (uint32_t k = 0; k < (per - 1) / 8 && q + 4 * ch <= qend; k++) {
            for (uint32_t c = 0; c < ch; c++) {
                for (int j = 0; j < 4; j++) {
                    uint8_t v = q[j];
                    d[(2 * j) * ch + c] = ima_nibble(v & 15, &pred[c], &idx[c]);
                    d[(2 * j + 1) * ch + c] = ima_nibble(v >> 4, &pred[c], &idx[c]);
                }
                q += 4;
            }
            d += 8 * ch;
            frames += 8;
        }
    }
    return frames;
}

static uint32_t ima_frames(uint32_t n, uint32_t align, uint32_t ch)
{
    return align ? (n / align + 1) * ima_per_block(align, ch) : 0;
}

/* ------------------------------------------------------------------ playing */
static void free_voice(Obj *o)
{
    if (o->voice >= 0)
        voice_free(o->voice);
    o->voice = -1;
    free(o->decoded);
    o->decoded = NULL;
    o->dec_frames = 0;
}

static int channels_of(const Obj *o) { return o->type & DIG_F_STEREO ? 2 : 1; }

static void apply_gain(Obj *o);

/* a voice for what the sample was given (at start) */
static void make_voice(Obj *o)
{
    free_voice(o);
    if (!o->addr || !o->len)
        return;
    int ch = channels_of(o);
    if (o->type & DIG_F_ADPCM) {
        uint32_t cap = ima_frames(o->len, o->block, (uint32_t)ch);
        if (!cap)
            return;
        o->decoded = malloc((size_t)cap * ch * 2 + 16);
        o->dec_frames = ima_decode((const uint8_t *)gptr(o->addr), o->len, o->block, (uint32_t)ch, o->decoded);
        if (o->dec_frames)
            o->voice = voice_new(o->decoded, o->dec_frames, ch, (int)o->rate);
    } else {
#if defined(PORT_MILES_COPY_SAMPLES)
        /* (li2-native's: the samples decoded into a copy the voice keeps in program memory) */
        int bits = o->type & DIG_F_16BITS ? 16 : 8;
        uint32_t frames = o->len / (uint32_t)(ch * (bits / 8));
        o->decoded = malloc((size_t)frames * ch * 2 + 16);
        const uint8_t *src = (const uint8_t *)gptr(o->addr);
        for (uint32_t k = 0; k < frames * (uint32_t)ch; k++)
            o->decoded[k] = bits == 16 ? (int16_t)(src[2 * k] | src[2 * k + 1] << 8) : (int16_t)((src[k] - 128) << 8);
        o->dec_frames = frames;
        if (frames)
            o->voice = voice_new(o->decoded, frames, ch, (int)o->rate);
#else
        o->voice = voice_new_guest(o->addr, o->len, ch, o->type & DIG_F_16BITS ? 16 : 8, (int)o->rate);
#endif
    }
#if defined(PORT_MILES_COPY_SAMPLES)
    if (o->voice >= 0)                  /* (li2-native's set_pcm: the gain; voice_new set the rate) */
        apply_gain(o);
#else
    if (o->voice >= 0 && o->rate)
        voice_set_rate(o->voice, o->rate);
#endif
}

/* the 3D listener's view of a sample: gain and the left/right balance */
static void place_3d(const Obj *o, float *g, float *pan)
{
    *g = 1.0f;
    *pan = 0.0f;
    const Obj *l = listener > 0 && objs[listener].used ? &objs[listener] : NULL;
    if (!l)
        return;
    float d[3] = {o->pos[0] - l->pos[0], o->pos[1] - l->pos[1], o->pos[2] - l->pos[2]};
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (o->maxd > 0 && dist > o->maxd) {
        *g = 0.0f;
        return;
    }
    float mind = o->mind > 0.001f ? o->mind : 0.001f;
    *g = dist > mind ? mind / (mind + rolloff * (dist - mind)) : 1.0f;
    if (dist > 1e-4f) {
        const float *f = l->front, *u = l->up;
        float r[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 1e-6f)
            *pan = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (rl * dist);
    }
}

/* SAMPLE CLASSES (the port's volume sliders): 0 effects, 1 music (streams), 2.. the port's own */
#define MILES_CLASSES 8
static float class_gain[MILES_CLASSES] = {1, 1, 1, 1, 1, 1, 1, 1};
__attribute__((weak)) int port_miles_class(const char *path, int previous) { (void)path; (void)previous; return 0; }

void miles_set_class_gain(int cls, float gain)
{
    if (cls < 0 || cls >= MILES_CLASSES)
        return;
    class_gain[cls] = gain < 0 ? 0 : gain;
    for (int i = 0; i < MAXOBJ; i++)
        if (objs[i].used && objs[i].voice >= 0)
            apply_gain(&objs[i]);
}

/* the class of the sample data at [addr, addr + n) read from file `win` (AIL_file_read) */
static uint32_t cls_addr[256], cls_end[256];
static int cls_of[256], cls_n;
static void note_sample(const char *win, uint32_t addr, uint32_t n)
{
    static int last;                    /* (a converted copy may come back under another name) */
    int c = port_miles_class(win, last);
    last = c;
    if (c > 0 && cls_n < 256) {
        cls_addr[cls_n] = addr;
        cls_end[cls_n] = addr + n;
        cls_of[cls_n++] = c;
    }
}
static int class_of_data(uint32_t addr)
{
    for (int i = 0; i < cls_n; i++)
        if (addr >= cls_addr[i] && addr < cls_end[i])
            return cls_of[i];
    return 0;
}

static void apply_gain(Obj *o)
{
    if (o->voice < 0)
        return;
    float v = o->volume < 0 ? 0 : o->volume > 1 ? 1 : o->volume;
    float p = (o->pan < 0 ? 0 : o->pan > 1 ? 1 : o->pan) * 2.0f - 1.0f;   /* -1..1 */
    if (o->kind == K_3DSAMPLE) {
        float g, pan;
        place_3d(o, &g, &pan);
        v *= g;
        p = pan * 0.8f;                 /* (a 3D sound is never fully in one ear) */
    } else {
        /* the driver's master is for 2D samples and streams: the game puts it into every 3D sample's own volume,
           volume * master / 127 [verified: disassembly 0x423d58, 0x424000], so here it would count twice: effects at
           0.55 x 0.55 against music at 0.55 at the game's settings: music was far louder than vehicle audio) */
        v *= master;
    }
    /* the port's level for the sample's class, on top of the game's own volumes */
    v *= class_gain[o->kind == K_STREAM ? 1 : o->cls];
    /* equal-power pan, unity in the centre */
    float l = p > 0 ? 1.0f - p : 1.0f, r = p < 0 ? 1.0f + p : 1.0f;
    voice_set_gain(o->voice, v * l, v * r);
    voice_set_send(o->voice, o->send);
}

/* PORT_MILES_COPY_SAMPLES (LEGO Island 2's recorded runs): a sample's voice is made when its file
   is set, from a decoded copy in program memory, and a start rewinds it; by default the voice is
   made at each start and plays the program's own memory (as Miles does, with no copy). Either way
   the sound is the same; the program's heap differs. */
static void start(Obj *o)
{
#if defined(PORT_MILES_COPY_SAMPLES)
    if (o->voice >= 0)
        voice_rewind(o->voice);
#else
    make_voice(o);
#endif
    if (o->voice < 0) {
        o->status = SMP_DONE;
        return;
    }
    apply_gain(o);
    voice_play(o->voice, o->loops);
    o->status = SMP_PLAYING;
    o->ended = 0;
}

static int status_of(Obj *o)
{
    if (o->status == SMP_PLAYING && (o->voice < 0 || !voice_playing(o->voice, NULL))) {
        o->status = SMP_DONE;
        o->ended = 1;
    }
    return o->status;
}

static void stop(Obj *o)
{
    if (status_of(o) == SMP_PLAYING) {
        voice_pause(o->voice);
        o->status = SMP_STOPPED;
    }
}

static void resume(Obj *o)
{
    if (o->status == SMP_STOPPED && o->voice >= 0) {
        voice_play(o->voice, o->loops);
        o->status = SMP_PLAYING;
    } else if (o->status == SMP_DONE && o->addr) {
        start(o);                       /* (Miles: resuming a sample never started starts it) */
    }
}

static void end(Obj *o)
{
#if defined(PORT_MILES_COPY_SAMPLES)
    if (o->voice >= 0) {
        voice_pause(o->voice);
        voice_rewind(o->voice);
    }
#else
    free_voice(o);
#endif
    o->status = SMP_DONE;
    o->ended = 0;
}

static void release(Obj *o)
{
    free_voice(o);
    o->used = 0;
    W32(o->handle, 0);
}

static uint32_t total_frames(const Obj *o);

/* frames played so far (in the data's own frames). A sample played to its end stays at its end:
   the movie player takes current == total as "the sound is over" (0x7bddcb) [verified: Ghidra
   decompile; with 0 after the end the 1.5 s transition movie, whose sound is 1.0 s, never ended] */
static uint32_t played_frames(Obj *o)
{
    uint32_t pos = 0;
    if (o->voice >= 0 && status_of(o) != SMP_DONE)
        voice_playing(o->voice, &pos);
    else if (o->ended)
        return total_frames(o);
    return pos;
}

static uint32_t total_frames(const Obj *o)
{
    int ch = channels_of(o);
    if (o->type & DIG_F_ADPCM)
        return o->decoded ? o->dec_frames : ima_frames(o->len, o->block, (uint32_t)ch);
    return o->len / (uint32_t)(ch * (o->type & DIG_F_16BITS ? 2 : 1));
}

/* the game thread, once a frame: 3D gains follow the listener */
void mss_frame(void)
{
    for (int i = 1; i < MAXOBJ; i++) {
        Obj *o = &objs[i];
        if (o->used && o->kind == K_3DSAMPLE && o->status == SMP_PLAYING)
            apply_gain(o);
    }
}

/* ------------------------------------------------------------------ the AIL functions */
#define AIL(name, n) HOSTFN(w_MSS32__AIL_##name##_##n, (n) / 4)

/* PORT_MILES_OWN_STRINGS (LEGO Island 2's recorded runs): Miles keeps its own copy of the redist
   folder's name, and its error text lives outside the program's memory until AIL_last_error asks
   for it, so neither moves the program's heap at start-up or at the first error. Without it the
   error buffer is in program memory from AIL_startup (the other ports' runs). */
#if defined(PORT_MILES_OWN_STRINGS)
static char error_text[256];
static void set_error(const char *s) { snprintf(error_text, sizeof error_text, "%s", s); }
#else
static void set_error(const char *s)
{
    if (!last_error_str)
        last_error_str = galloc(256);
    gstrncpy(last_error_str, s, 256);
}
#endif

#if defined(PORT_MILES_OWN_STRINGS)
AIL(startup, 0) { LOGS("AIL_startup"); return 1; }
#else
AIL(startup, 0) { LOGS("AIL_startup"); set_error(""); return 1; }
#endif
AIL(shutdown, 0)
{
    LOGS("AIL_shutdown");
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].used)
            free_voice(&objs[i]);
    return 0;
}
AIL(set_redist_directory, 4)
{
#if defined(PORT_MILES_OWN_STRINGS)
    uint32_t old = redist_dir;              /* (li2-native's: its own copy; the old one's address back) */
    if (ARG(0)) {
        if (redist_dir) gfree(redist_dir);
        redist_dir = gstrdup(gstr(ARG(0)));
    }
    LOGS("AIL_set_redist_directory(%s)", ARG(0) ? gstr(ARG(0)) : "NULL");
    return old ? old : redist_dir;
#else
    uint32_t prev = redist_dir;
    redist_dir = ARG(0);
    return prev;
#endif
}
#if defined(PORT_MILES_OWN_STRINGS)
AIL(last_error, 0)
{
    if (!last_error_str)
        last_error_str = galloc(256);
    gstrncpy(last_error_str, error_text, 256);
    return last_error_str;
}
#else
AIL(last_error, 0) { if (!last_error_str) set_error(""); return last_error_str; }
#endif
/* (the program's allocator: our own allocations go through the runtime's) */
AIL(mem_use_malloc, 4) { return 0; }
AIL(mem_use_free, 4) { return 0; }
AIL(mem_free_lock, 4) { if (ARG(0)) gfree(ARG(0)); return 0; }
/* (number, value): the previous value */
AIL(set_preference, 8)
{
    uint32_t n = ARG(0), prev = n < 64 ? prefs[n] : 0;
    if (n < 64)
        prefs[n] = ARG(1);
    return prev;
}
/* (frequency, bits, channels, flags) */
AIL(open_digital_driver, 16)
{
    LOGS("AIL_open_digital_driver(%u Hz, %u bits, %u ch)", ARG(0), ARG(1), ARG(2));
    return obj_new(K_DRIVER);
}
/* (driver, level 0..1) */
AIL(set_digital_master_volume_level, 8)
{
    master = fl(ARG(1));
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].used && objs[i].status == SMP_PLAYING)
            apply_gain(&objs[i]);
    return 0;
}

/* 2D samples */
AIL(allocate_sample_handle, 4) { return obj_new(K_SAMPLE); }
AIL(release_sample_handle, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) release(o); return 0; }
AIL(init_sample, 4)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) {
        free_voice(o);
        o->status = SMP_DONE;
        o->ended = 0;
        o->addr = o->len = 0;
        o->type = DIG_F_16BITS;
        o->block = 0;
        o->rate = 22050;
        o->loops = 1;
        o->volume = 1.0f;
        o->pan = 0.5f;
    }
    return 0;
}
/* (sample, start, length) */
AIL(set_sample_address, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->addr = ARG(1); o->len = ARG(2); o->ended = 0; }
    return 0;
}
/* (sample, format, flags) */
AIL(set_sample_type, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) o->type = (int)ARG(1);
    return 0;
}
AIL(set_sample_adpcm_block_size, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) o->block = ARG(1);
    return 0;
}
AIL(set_sample_playback_rate, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->rate = ARG(1); if (o->voice >= 0 && o->rate) voice_set_rate(o->voice, o->rate); }
    return 0;
}
/* (sample, volume 0..1, pan 0..1) */
AIL(set_sample_volume_pan, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->volume = fl(ARG(1)); o->pan = fl(ARG(2)); apply_gain(o); }
    return 0;
}
AIL(sample_volume_pan, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (ARG(1)) W32(ARG(1), fu(o ? o->volume : 0));
    if (ARG(2)) W32(ARG(2), fu(o ? o->pan : 0.5f));
    return 0;
}
/* (sample, dry, wet): EAX reverb levels (no reverb yet) */
/* (sample, dry 0..1, wet 0..1) */
AIL(set_sample_reverb_levels, 12)
{
    Obj *o = smp_get(ARG(0));
    if (o) { o->send = fl(ARG(2)); apply_gain(o); }
    return 0;
}
AIL(set_sample_loop_count, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) o->loops = (int)ARG(1);
    return 0;
}
AIL(start_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) start(o); return 0; }
AIL(stop_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) stop(o); return 0; }
AIL(resume_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) resume(o); return 0; }
AIL(end_sample, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); if (o) end(o); return 0; }
AIL(sample_status, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); return o ? (uint32_t)status_of(o) : SMP_FREE; }
/* (sample): the byte offset reached in the data */
AIL(sample_position, 4)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (!o)
        return 0;
    uint32_t f = played_frames(o), ch = (uint32_t)channels_of(o);
    if (o->type & DIG_F_ADPCM) {
        uint32_t per = ima_per_block(o->block, ch);
        return per ? f / per * o->block + (f % per) * ch / 2 : 0;
    }
    return f * ch * (o->type & DIG_F_16BITS ? 2 : 1);
}
/* (sample, &total ms, &current ms) */
AIL(sample_ms_position, 12)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    uint32_t rate = o && o->rate ? o->rate : 22050;
    uint32_t tot = o ? (uint32_t)((uint64_t)total_frames(o) * 1000 / rate) : 0;
    uint32_t cur = o ? (uint32_t)((uint64_t)played_frames(o) * 1000 / rate) : 0;
    if (ARG(1)) W32(ARG(1), tot);
    if (ARG(2)) W32(ARG(2), cur);
    return 0;
}

/* 3D providers: "Miles Fast 2D Positional Audio" unless the port lists others (a NULL-ended list
   from a strong port_miles_providers: Enter the Matrix opens "Creative Labs EAX 3 (TM)", then EAX
   2, EAX, then Fast 2D, and gets its reverb level from the first that opens) */
static const char *const default_providers[] = {"Miles Fast 2D Positional Audio", NULL};
__attribute__((weak)) const char *const *port_miles_providers(void) { return default_providers; }
#define MAX_PROVIDERS 8
static uint32_t provider_handles[MAX_PROVIDERS], provider_strs[MAX_PROVIDERS];

/* (&next, &provider, &name): 0 when there are no more */
AIL(enumerate_3D_providers, 12)
{
    const char *const *provider_names = port_miles_providers();
    uint32_t k = ARG(0) ? R32(ARG(0)) : 0;
    if (k >= MAX_PROVIDERS || !provider_names[k])
        return 0;
    if (!provider_handles[k]) {
        provider_handles[k] = obj_new(K_PROVIDER);
        provider_strs[k] = gstrdup(provider_names[k]);
    }
    if (ARG(1)) W32(ARG(1), provider_handles[k]);
    if (ARG(2)) W32(ARG(2), provider_strs[k]);
    if (ARG(0)) W32(ARG(0), k + 1);
    return 1;
}
AIL(open_3D_provider, 4)
{
    LOGS("AIL_open_3D_provider(%08x)", ARG(0));
    return obj_get(ARG(0), K_PROVIDER) ? 0 : 1;        /* M3D_NOERR / M3D_NOTFOUND */
}
/* (provider, name, &value) */
AIL(3D_provider_attribute, 12)
{
    const char *n = gstr(ARG(1));
    if (ARG(2) && n && !strcasecmp(n, "Maximum supported samples"))
        W32(ARG(2), 64);
    return 0;
}
/* (provider, name, &value). "EAX all parameters": the room as EAX 3 listener properties
   (environment, size, diffusion, room, room HF, room LF (mB), decay time, decay HF and LF ratios,
   reflections (mB), reflections delay, reflections pan, reverb (mB), reverb delay, ...); the game
   hands the address of a pointer to one of its presets (0x9437b8 Generic, ...) [verified: values
   at run time], or (EAX 2) the properties themselves. The reverb (dsound.c) takes the decay time,
   its high-frequency ratio, the level room + reverb and the pre-delay. */
static void eax_room(uint32_t p)
{
    if (!p)
        return;
    uint32_t first = R32(p);
    if (first >= 0x400000u && first < 0x1000000u)   /* a pointer to the preset */
        p = first;
    uint32_t env = R32(p);
    float size = fl(R32(p + 4)), decay = fl(R32(p + 24)), hf = fl(R32(p + 28)), rdelay = fl(R32(p + 40)),
          vdelay = fl(R32(p + 60));
    int32_t room = (int32_t)R32(p + 12), reverb = (int32_t)R32(p + 56);
    if (env > 64 || !(size > 0.9f && size < 101) || !(decay > 0.09f && decay < 21) || room < -10000 || room > 0) {
        LOGS("EAX all parameters: not EAX 3 properties (env %u, size %g, decay %g): left as it was", env, size, decay);
        return;
    }
    float gain = (float)pow(10.0, (room + reverb) / 2000.0) * 2.0f;
    LOGS("EAX room: environment %u, decay %.2f s (HF x%.2f), room %d + reverb %d mB, delays %.3f + %.3f s",
         env, decay, hf, room, reverb, rdelay, vdelay);
    sound_reverb_set(decay, hf, gain, rdelay + vdelay);
}

AIL(set_3D_provider_preference, 12)
{
    const char *n = gstr(ARG(1));
    LOGS("AIL_set_3D_provider_preference(%s)", n ? n : "?");
    if (n && !strcasecmp(n, "EAX all parameters"))
        eax_room(ARG(2));
    return 0;
}
AIL(3D_speaker_type, 4) { return 0; }                   /* AIL_3D_2_SPEAKER */
AIL(set_3D_room_type, 8) { LOGS("AIL_set_3D_room_type(%u)", ARG(1)); return 0; }
AIL(set_3D_distance_factor, 8) { return 0; }
AIL(set_3D_rolloff_factor, 8)
{
    float r = fl(ARG(1));
    rolloff = r >= 0 && r < 100 ? r : 1.0f;
    return 0;
}
AIL(open_3D_listener, 4)
{
    uint32_t h = obj_new(K_LISTENER);
    listener = (int)R32(h + 4);
    return h;
}
AIL(close_3D_listener, 4)
{
    Obj *o = obj_get(ARG(0), K_LISTENER);
    if (o) {
        if (listener == (int)R32(ARG(0) + 4))
            listener = -1;
        release(o);
    }
    return 0;
}

/* 3D samples */
AIL(allocate_3D_sample_handle, 4)
{
    uint32_t h = obj_new(K_3DSAMPLE);
    obj_get(h, K_3DSAMPLE)->send = 1;
    return h;
}
AIL(release_3D_sample_handle, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) release(o); return 0; }
/* (sample, AILSOUNDINFO *): format, data, length, rate, bits, channels, samples, block size,
   initial pointer. 1 if it can play it. */
AIL(set_3D_sample_info, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    uint32_t i = ARG(1);
    if (!o || !i)
        return 0;
    end(o);
    uint32_t fmt = R32(i), bits = R32(i + 16), ch = R32(i + 20);
    o->addr = R32(i + 4);
    o->len = R32(i + 8);
    o->rate = R32(i + 12);
    o->block = R32(i + 28);
    if (fmt == 1 && (bits == 8 || bits == 16) && (ch == 1 || ch == 2))
        o->type = (bits == 16 ? DIG_F_16BITS : 0) | (ch == 2 ? DIG_F_STEREO : 0);
    else if (fmt == 0x11 && (ch == 1 || ch == 2) && o->block)
        o->type = DIG_F_ADPCM | DIG_F_16BITS | (ch == 2 ? DIG_F_STEREO : 0);
    else {
        LOGS("AIL_set_3D_sample_info: format %u, %u bits, %u ch not supported", fmt, bits, ch);
        set_error("unsupported format");
        o->addr = o->len = 0;
        return 0;
    }
    return 1;
}
AIL(set_3D_sample_playback_rate, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) { o->rate = ARG(1); if (o->voice >= 0 && o->rate) voice_set_rate(o->voice, o->rate); }
    return 0;
}
/* (Miles 6.0: an integer 0..127; 6.5: a float 0..1, same name and size: PORT_MILES_VERSION) */
#ifndef PORT_MILES_VERSION
#define PORT_MILES_VERSION 60
#endif
AIL(set_3D_sample_volume, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
#if PORT_MILES_VERSION >= 65
    if (o) { o->volume = fl(ARG(1)); apply_gain(o); }
#else
    if (o) { o->volume = (float)(int32_t)ARG(1) / 127.0f; apply_gain(o); }
#endif
    return 0;
}
AIL(set_3D_sample_loop_count, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) o->loops = (int)ARG(1);
    return 0;
}
/* (sample, max, min) */
AIL(set_3D_sample_distances, 12)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) { o->maxd = fl(ARG(1)); o->mind = fl(ARG(2)); apply_gain(o); }
    return 0;
}
/* (sample, name, &value): EAX settings (not heard) */
/* (sample, name, &value): "EAX sample reverb mix" (-1: by distance, as EAX does: a full send
   whose level follows the direct sound's) */
AIL(set_3D_sample_preference, 12)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    const char *n = gstr(ARG(1));
    if (o && n && ARG(2) && !strcasecmp(n, "EAX sample reverb mix")) {
        float m = fl(R32(ARG(2)));
        o->send = m < 0 ? 1 : m > 1 ? 1 : m;
        apply_gain(o);
    }
    return 0;
}
AIL(start_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) start(o); return 0; }
AIL(stop_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) stop(o); return 0; }
AIL(resume_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) resume(o); return 0; }
AIL(end_3D_sample, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); if (o) end(o); return 0; }
AIL(3D_sample_status, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); return o ? (uint32_t)status_of(o) : SMP_FREE; }
/* (object, x, y, z): a 3D sample or the listener */
AIL(set_3D_position, 16)
{
    Obj *o = obj_get(ARG(0), 0);
    if (o) {
        o->pos[0] = fl(ARG(1)); o->pos[1] = fl(ARG(2)); o->pos[2] = fl(ARG(3));
        if (o->kind == K_3DSAMPLE) apply_gain(o);
    }
    return 0;
}
/* (object, front x y z, up x y z) */
AIL(set_3D_orientation, 28)
{
    Obj *o = obj_get(ARG(0), 0);
    if (o)
        for (int k = 0; k < 3; k++) { o->front[k] = fl(ARG(1 + k)); o->up[k] = fl(ARG(4 + k)); }
    return 0;
}

/* ------------------------------------------------------------------ files and decompression */
/* (file name, destination): -1 = allocate, the size in the first dword and the data after it
   (FILE_READ_WITH_SIZE); NULL = allocate, data only */
AIL(file_read, 8)
{
    char host[1200];
    const char *win = gstr(ARG(0));
    FILE *f = win && path_to_host(win, host, sizeof host, 0) ? fopen(host, "rb") : NULL;
    if (!f) {
        LOGS("AIL_file_read(%s): not found", win ? win : "NULL");
        set_error("file not found");
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t dst = ARG(1), data;
    if (dst == 0xffffffffu) {
        dst = galloc((uint32_t)n + 4);
        W32(dst, (uint32_t)n);
        data = dst + 4;
    } else {
        if (!dst)
            dst = galloc((uint32_t)n ? (uint32_t)n : 1);
        data = dst;
    }
    size_t got = fread(gptr(data), 1, (size_t)n, f);
    fclose(f);
    LOGS("AIL_file_read(%s): %ld bytes at %08x", win, n, dst);
    note_sample(win, data, (uint32_t)n);
    (void)got;
    return dst;
}

/* (AILSOUNDINFO *, &out, &out size): IMA ADPCM to a 16-bit PCM WAV image (allocated) */
AIL(decompress_ADPCM, 12)
{
    uint32_t i = ARG(0);
    uint32_t fmt = R32(i), data = R32(i + 4), len = R32(i + 8), rate = R32(i + 12), ch = R32(i + 20),
             block = R32(i + 28);
    if (fmt != 0x11 || !block || (ch != 1 && ch != 2) || !data) {
        set_error("not IMA ADPCM");
        return 0;
    }
    uint32_t cap = ima_frames(len, block, ch);
    int16_t *pcm = malloc((size_t)cap * ch * 2 + 16);
    uint32_t frames = ima_decode((const uint8_t *)gptr(data), len, block, ch, pcm);
    uint32_t bytes = frames * ch * 2, out = galloc(44 + bytes);
    uint8_t *w = (uint8_t *)gptr(out);
    uint32_t v;
    memcpy(w, "RIFF", 4); v = 36 + bytes; memcpy(w + 4, &v, 4); memcpy(w + 8, "WAVEfmt ", 8);
    v = 16; memcpy(w + 16, &v, 4);
    uint16_t s = 1; memcpy(w + 20, &s, 2); s = (uint16_t)ch; memcpy(w + 22, &s, 2);
    memcpy(w + 24, &rate, 4); v = rate * ch * 2; memcpy(w + 28, &v, 4);
    s = (uint16_t)(ch * 2); memcpy(w + 32, &s, 2); s = 16; memcpy(w + 34, &s, 2);
    memcpy(w + 36, "data", 4); memcpy(w + 40, &bytes, 4);
    memcpy(w + 44, pcm, bytes);
    free(pcm);
    if (ARG(1)) W32(ARG(1), out);
    if (ARG(2)) W32(ARG(2), 44 + bytes);
    return 1;
}

/* ------------------------------------------------------------------ Miles 6.0's own calls */
/* A WAV image in program memory: the format and data chunks. 0 if it is not one. */
static int wav_parse(uint32_t img, uint32_t *fmt, uint32_t *ch, uint32_t *rate, uint32_t *bits,
                     uint32_t *align, uint32_t *data, uint32_t *len, uint32_t *samples)
{
    if (!img || R32(img) != 0x46464952u || R32(img + 8) != 0x45564157u)
        return 0;
    uint32_t end = img + 8 + R32(img + 4), p = img + 12;
    int have_fmt = 0;
    *data = *len = 0;
    *samples = 0;
    while (p + 8 <= end) {
        uint32_t id = R32(p), n = R32(p + 4);
        if (id == 0x20746d66u) {                 /* "fmt " */
            *fmt = R16(p + 8);
            *ch = R16(p + 10);
            *rate = R32(p + 12);
            *align = R16(p + 20);
            *bits = R16(p + 22);
            if (*fmt == 0x11 && n >= 20)
                *samples = 0;
            have_fmt = 1;
        } else if (id == 0x61746164u) {          /* "data" */
            *data = p + 8;
            *len = n;
        } else if (id == 0x74636166u && n >= 4) { /* "fact" */
            *samples = R32(p + 8);
        }
        p += 8 + ((n + 1) & ~1u);
    }
    if (!have_fmt || !*data)
        return 0;
    if (*fmt == 1 && *bits)
        *samples = *len / (*ch * *bits / 8);
    return 1;
}

static int set_file(Obj *o, uint32_t img)
{
    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0, align = 0, data = 0, len = 0, samples = 0;
#if defined(PORT_MILES_COPY_SAMPLES)
    /* the old voice is freed as the new one is made, not paused and rewound first (li2-native's
       set_pcm: each of those reads the --test clock, and LEGO Island 2's town route counts them) */
    o->status = SMP_DONE;
    o->ended = 0;
#else
    end(o);
#endif
    o->addr = o->len = 0;
    if (!wav_parse(img, &fmt, &ch, &rate, &bits, &align, &data, &len, &samples)) {
#if defined(PORT_MILES_COPY_SAMPLES)
        free_voice(o);
#endif
        set_error("not a WAV image");
        return 0;
    }
    o->addr = data;
    o->cls = class_of_data(data);
    o->len = len;
    o->rate = rate;
    o->block = align;
    if (fmt == 1 && (bits == 8 || bits == 16) && (ch == 1 || ch == 2))
        o->type = (bits == 16 ? DIG_F_16BITS : 0) | (ch == 2 ? DIG_F_STEREO : 0);
    else if (fmt == 0x11 && (ch == 1 || ch == 2) && align)
        o->type = DIG_F_ADPCM | DIG_F_16BITS | (ch == 2 ? DIG_F_STEREO : 0);
    else {
        LOGS("sample file: format %u, %u bits, %u ch not supported", fmt, bits, ch);
#if defined(PORT_MILES_COPY_SAMPLES)
        free_voice(o);
#endif
        set_error("unsupported format");
        o->addr = o->len = 0;
        return 0;
    }
#if defined(PORT_MILES_COPY_SAMPLES)
    make_voice(o);                      /* (the voice and its copy now, as li2-native's set_pcm) */
    o->status = SMP_DONE;
#endif
    return 1;
}

/* (sample, image, block) */
AIL(set_sample_file, 12) { Obj *o = obj_get(ARG(0), K_SAMPLE); return o ? (uint32_t)set_file(o, ARG(1)) : 0; }
/* (sample, image) */
AIL(set_3D_sample_file, 8) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); return o ? (uint32_t)set_file(o, ARG(1)) : 0; }
AIL(set_sample_volume, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->volume = (float)(int32_t)ARG(1) / 127.0f; apply_gain(o); }
    return 0;
}
AIL(set_sample_pan, 8)
{
    Obj *o = obj_get(ARG(0), K_SAMPLE);
    if (o) { o->pan = (float)(int32_t)ARG(1) / 127.0f; apply_gain(o); }
    return 0;
}
AIL(set_3D_sample_effects_level, 8)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (o) { float e = fl(ARG(1)); o->send = e < 0 ? 0 : e > 1 ? 1 : e; apply_gain(o); }
    return 0;
}
AIL(set_3D_speaker_type, 8) { return 0; }
AIL(set_3D_velocity_vector, 16) { return 0; }
AIL(close_3D_provider, 4) { return 0; }
AIL(active_3D_sample_count, 4)
{
    uint32_t n = 0;
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].used && objs[i].kind == K_3DSAMPLE && status_of(&objs[i]) == SMP_PLAYING)
            n++;
    return n;
}
AIL(get_preference, 4) { return ARG(0) < 64 ? prefs[ARG(0)] : 0; }
AIL(mem_alloc_lock, 4) { return galloc(ARG(0) ? ARG(0) : 1); }

/* The driver: the format it was opened with */
static uint32_t drv_rate = 22050, drv_bits = 16, drv_ch = 2, master_level = 127;
/* (&driver, &waveout, device, WAVEFORMAT *): 0 = opened */
AIL(waveOutOpen, 16)
{
    uint32_t f = ARG(3);
    if (f) {
        drv_ch = R16(f + 2);
        drv_rate = R32(f + 4);
        drv_bits = R16(f + 12) == 2 * drv_ch || R16(f + 12) == drv_ch ? 8 * R16(f + 12) / drv_ch : 16;
    }
    LOGS("AIL_waveOutOpen: %u Hz, %u bits, %u ch", drv_rate, drv_bits, drv_ch);
    if (ARG(0)) W32(ARG(0), obj_new(K_DRIVER));
    if (ARG(1)) W32(ARG(1), 0);
    return 0;
}
AIL(waveOutClose, 4)
{
    Obj *o = obj_get(ARG(0), K_DRIVER);
    if (o) release(o);
    return 0;
}
/* (driver, &rate, &format, string) */
AIL(digital_configuration, 16)
{
    if (ARG(1)) W32(ARG(1), drv_rate);
    if (ARG(2)) W32(ARG(2), (drv_bits == 16 ? DIG_F_16BITS : 0) | (drv_ch == 2 ? DIG_F_STEREO : 0));
    if (ARG(3)) gstrncpy(ARG(3), "DirectSound", 32);
    return 0;
}
AIL(digital_master_volume, 4) { return master_level; }
AIL(set_digital_master_volume, 8)
{
    master_level = ARG(1);
    master = (float)(int32_t)master_level / 127.0f;
    for (int i = 1; i < MAXOBJ; i++)
        if (objs[i].used && (objs[i].kind == K_SAMPLE || objs[i].kind == K_3DSAMPLE) && objs[i].status == SMP_PLAYING)
            apply_gain(&objs[i]);
    return 0;
}

/* Files */
AIL(file_size, 4)
{
    char host[1200];
    const char *win = gstr(ARG(0));
    FILE *f = win && path_to_host(win, host, sizeof host, 0) ? fopen(host, "rb") : NULL;
    if (!f)
        return 0xffffffffu;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (uint32_t)n;
}
/* (data, size): 1 PCM WAV, 2 ADPCM WAV, 3 other WAV, 0 unknown */
AIL(file_type, 8)
{
    uint32_t fmt = 0, ch, rate, bits, align, data, len, samples;
    if (!wav_parse(ARG(0), &fmt, &ch, &rate, &bits, &align, &data, &len, &samples))
        return 0;
    return fmt == 1 ? 1 : fmt == 0x11 ? 2 : 3;
}
/* (data, AILSOUNDINFO *) */
AIL(WAV_info, 8)
{
    uint32_t fmt = 0, ch = 0, rate = 0, bits = 0, align = 0, data = 0, len = 0, samples = 0, i = ARG(1);
    if (!wav_parse(ARG(0), &fmt, &ch, &rate, &bits, &align, &data, &len, &samples)) {
        set_error("not a WAV image");
        return 0;
    }
    W32(i, fmt);
    W32(i + 4, data);
    W32(i + 8, len);
    W32(i + 12, rate);
    W32(i + 16, bits);
    W32(i + 20, ch);
    W32(i + 24, samples);
    W32(i + 28, align);
    W32(i + 32, data);
    return 1;
}

/* Format conversion: AILMIXINFO starts with an AILSOUNDINFO; 44 bytes each [guess: the size, only
   one source is ever passed here (0x41ed80)]. Linear interpolation, as Miles' mixer. */
static uint32_t mix_frames(uint32_t info, uint32_t dest_rate)
{
    uint32_t len = R32(info + 8), rate = R32(info + 12), bits = R32(info + 16), ch = R32(info + 20);
    if (!rate || !bits || !ch)
        return 0;
    uint64_t src = len / (ch * bits / 8);
    return (uint32_t)(src * dest_rate / rate);
}
static uint32_t fmt_bytes(uint32_t f) { return (f & DIG_F_16BITS ? 2 : 1) * (f & DIG_F_STEREO ? 2 : 1); }
/* (dest rate, dest format, sources, AILMIXINFO *): the bytes needed */
AIL(size_processed_digital_audio, 16)
{
    return (mix_frames(ARG(3), ARG(0)) + 1) * fmt_bytes(ARG(1));
}
/* (dest, dest size, dest rate, dest format, sources, AILMIXINFO *): the bytes written */
AIL(process_digital_audio, 24)
{
    uint32_t dest = ARG(0), cap = ARG(1), drate = ARG(2), dfmt = ARG(3), info = ARG(5);
    uint32_t data = R32(info + 4), rate = R32(info + 12), bits = R32(info + 16), ch = R32(info + 20);
    uint32_t n = mix_frames(info, drate), dbytes = fmt_bytes(dfmt), written = 0;
    uint64_t src = rate && bits && ch ? R32(info + 8) / (ch * bits / 8) : 0;
    uint32_t dch = dfmt & DIG_F_STEREO ? 2 : 1;
    if (!src)
        return 0;
    const uint8_t *in = (const uint8_t *)gptr(data);
    #define SAMPLE(fr, c) (bits == 16 ? (float)((const int16_t *)in)[(fr) * ch + (c)] \
                                      : ((float)in[(fr) * ch + (c)] - 128.0f) * 256.0f)
    uint8_t *out = (uint8_t *)gptr(dest);
    for (uint32_t i = 0; i < n && written + dbytes <= cap; i++) {
        double t = (double)i * rate / drate;
        uint64_t a = (uint64_t)t, b = a + 1 < src ? a + 1 : a;
        float fr = (float)(t - (double)a);
        for (uint32_t c = 0; c < dch; c++) {
            uint32_t sc = ch == 2 ? c : 0;
            float v = SAMPLE(a, sc) * (1 - fr) + SAMPLE(b, sc) * fr;
            if (dch == 1 && ch == 2)
                v = (v + (SAMPLE(a, 1) * (1 - fr) + SAMPLE(b, 1) * fr)) / 2;
            if (dfmt & DIG_F_16BITS) {
                int16_t s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                memcpy(out + written, &s, 2);
                written += 2;
            } else {
                int s = (int)(v / 256.0f) + 128;
                out[written++] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
            }
        }
    }
    #undef SAMPLE
    return written;
}
/* (file name, data, length, rate, format) */
AIL(WAV_file_write, 20)
{
    char host[1200];
    const char *win = gstr(ARG(0));
    if (!win)
        return 0;
    path_to_host(win, host, sizeof host, 1);
    FILE *f = fopen(host, "wb");
    if (!f) {
        set_error("can't write file");
        return 0;
    }
    uint32_t len = ARG(2), rate = ARG(3) & 0xffff, df = ARG(4), ch = df & DIG_F_STEREO ? 2 : 1,
             bits = df & DIG_F_16BITS ? 16 : 8, v;
    uint16_t s;
    fwrite("RIFF", 1, 4, f); v = 36 + len; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f); s = (uint16_t)ch; fwrite(&s, 2, 1, f);
    fwrite(&rate, 4, 1, f); v = rate * ch * bits / 8; fwrite(&v, 4, 1, f);
    s = (uint16_t)(ch * bits / 8); fwrite(&s, 2, 1, f); s = (uint16_t)bits; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&len, 4, 1, f);
    fwrite(gptr(ARG(1)), 1, len, f);
    fclose(f);
    return 1;
}

/* Streams: the music, decoded whole when opened (a few seconds of work at most for an MP3; WAVs
   are read straight) and played by a voice of their own. */
#if !defined(PORT_MILES_NO_FFMPEG)
/* An MP3 frame's length in bytes from its header, -1 if it isn't one */
static int mp3_frame_len(const uint8_t *h, int n)
{
    static const int br1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int br2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int sr[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
    if (n < 4 || h[0] != 0xff || (h[1] & 0xe0) != 0xe0 || ((h[1] >> 1) & 3) != 1)   /* (layer III) */
        return -1;
    int ver = (h[1] >> 3) & 3, rate = sr[ver][(h[2] >> 2) & 3], br = (ver == 3 ? br1 : br2)[h[2] >> 4] * 1000;
    if (!rate || !br)
        return -1;
    return (ver == 3 ? 144 : 72) * br / rate + ((h[2] >> 1) & 1);
}

static int decode_file(const char *path, int16_t **pcm_out, uint32_t *frames_out, int *ch_out, int *rate_out)
{
    AVFormatContext *f = NULL;
    AVCodecContext *ad = NULL;
    SwrContext *swr = NULL;
    AVFrame *fr = NULL;
    AVPacket *pkt = NULL;
    int16_t *pcm = NULL;
    size_t cap = 0, have = 0;
    int ok = 0;
    if (avformat_open_input(&f, path, NULL, NULL) < 0)
        return 0;
    if (avformat_find_stream_info(f, NULL) < 0)
        goto done;
    int as = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (as < 0)
        goto done;
    const AVCodec *codec = avcodec_find_decoder(f->streams[as]->codecpar->codec_id);
    if (!codec || !(ad = avcodec_alloc_context3(codec)))
        goto done;
    avcodec_parameters_to_context(ad, f->streams[as]->codecpar);
    if (avcodec_open2(ad, codec, NULL) < 0)
        goto done;
    int ch = ad->ch_layout.nb_channels >= 2 ? 2 : 1;
    AVChannelLayout out;
    av_channel_layout_default(&out, ch);
    if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, ad->sample_rate, &ad->ch_layout, ad->sample_fmt,
                            ad->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0)
        goto done;
    fr = av_frame_alloc();
    pkt = av_packet_alloc();
    int eof = 0;
    while (!eof) {
        if (av_read_frame(f, pkt) < 0) {
            eof = 1;
            avcodec_send_packet(ad, NULL);
        } else if (pkt->stream_index == as) {
            /* an MP3 frame cut short (the end of LEGO Island 2's speech: 50 of 156 bytes) is left
               out, as Miles' decoder and dr_mp3 do; FFmpeg would conceal it as a frame of near
               silence, and each line ended 1,152 samples late [verified: LECSIN01, 288,000 samples
               from dr_mp3 against FFmpeg's 289,152, identical but for that last frame] */
            int want = ad->codec_id == AV_CODEC_ID_MP3 ? mp3_frame_len(pkt->data, pkt->size) : -1;
            if (want < 0 || pkt->size >= want)
                avcodec_send_packet(ad, pkt);
        }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ad, fr) == 0) {
            if (have + (size_t)fr->nb_samples > cap) {
                cap = (have + (size_t)fr->nb_samples) * 2 + 4096;
                pcm = realloc(pcm, cap * ch * 2);
            }
            uint8_t *o = (uint8_t *)(pcm + have * ch);
            int got = swr_convert(swr, &o, fr->nb_samples, (const uint8_t *const *)fr->extended_data, fr->nb_samples);
            if (got > 0)
                have += (size_t)got;
        }
    }
    if (have) {
        *pcm_out = pcm;
        pcm = NULL;
        *frames_out = (uint32_t)have;
        *ch_out = ch;
        *rate_out = ad->sample_rate;
        ok = 1;
    }
done:
    free(pcm);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    swr_free(&swr);
    avcodec_free_context(&ad);
    avformat_close_input(&f);
    return ok;
}
#else
/* stb_vorbis (third_party/stb_vorbis, built by stb_vorbis_impl.c), if the port links it */
extern int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate,
                                    short **output) __attribute__((weak));

static int decode_file(const char *path, int16_t **pcm_out, uint32_t *frames_out, int *ch_out, int *rate_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *d = n > 12 ? malloc((size_t)n) : NULL;
    int ok = d && fread(d, 1, (size_t)n, fp) == (size_t)n;
    fclose(fp);
    if (!ok) {
        free(d);
        return 0;
    }
    ok = 0;
    if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) {
        uint32_t fmt = 0, ch = 0, rate = 0, bits = 0;
        for (long i = 12; i + 8 <= n; ) {
            uint32_t len;
            memcpy(&len, d + i + 4, 4);
            if (!memcmp(d + i, "fmt ", 4) && len >= 16 && i + 24 <= n) {
                fmt = d[i + 8] | d[i + 9] << 8;
                ch = d[i + 10] | d[i + 11] << 8;
                memcpy(&rate, d + i + 12, 4);
                bits = d[i + 22] | d[i + 23] << 8;
            } else if (!memcmp(d + i, "data", 4) && fmt == 1 && ch && (bits == 8 || bits == 16)) {
                uint32_t avail = (uint32_t)(n - i - 8) < len ? (uint32_t)(n - i - 8) : len;
                uint32_t frames = avail / (ch * (bits / 8));
                int16_t *pcm = malloc((size_t)frames * ch * 2 + 2);
                if (pcm) {
                    const uint8_t *src = d + i + 8;
                    for (uint32_t k = 0; k < frames * ch; k++)
                        pcm[k] = bits == 16 ? (int16_t)(src[2 * k] | src[2 * k + 1] << 8)
                                            : (int16_t)((src[k] - 128) << 8);
                    *pcm_out = pcm;
                    *frames_out = frames;
                    *ch_out = (int)ch;
                    *rate_out = (int)rate;
                    ok = frames > 0;
                }
                break;
            }
            i += 8 + len + (len & 1);
        }
    } else if (!memcmp(d, "OggS", 4) && stb_vorbis_decode_memory) {
        int ch = 0, rate = 0;
        short *out = NULL;
        int frames = stb_vorbis_decode_memory(d, (int)n, &ch, &rate, &out);
        if (frames > 0 && out) {
            *pcm_out = out;
            *frames_out = (uint32_t)frames;
            *ch_out = ch;
            *rate_out = rate;
            ok = 1;
        }
    } else
        LOGS("stream %s: neither WAV nor Ogg (a build without FFmpeg)", path);
    free(d);
    return ok;
}
#endif

/* (driver, file, memory) */
AIL(open_stream, 12)
{
    char host[1200];
    const char *win = gstr(ARG(1));
    int16_t *pcm = NULL;
    uint32_t frames = 0;
    int ch = 0, rate = 0;
    if (!win || !path_to_host(win, host, sizeof host, 0) || !decode_file(host, &pcm, &frames, &ch, &rate)) {
        LOGS("AIL_open_stream(%s): can't", win ? win : "NULL");
        set_error("can't open the stream");
        return 0;
    }
    uint32_t h = obj_new(K_STREAM);
    Obj *o = obj_get(h, K_STREAM);
    o->decoded = pcm;
    o->dec_frames = frames;
    o->type = DIG_F_16BITS | (ch == 2 ? DIG_F_STEREO : 0);
    o->rate = (uint32_t)rate;
    o->voice = voice_new(pcm, frames, ch, rate);
    LOGS("AIL_open_stream(%s): %u frames, %d ch, %d Hz", win, frames, ch, rate);
    return h;
}
AIL(close_stream, 4) { Obj *o = obj_get(ARG(0), K_STREAM); if (o) release(o); return 0; }
AIL(start_stream, 4)
{
    Obj *o = obj_get(ARG(0), K_STREAM);
    if (o && o->voice >= 0) {
        voice_rewind(o->voice);
        apply_gain(o);
        voice_play(o->voice, o->loops);
        o->status = SMP_PLAYING;
        o->ended = 0;
    }
    return 0;
}
/* (stream, 1 pause / 0 resume) */
AIL(pause_stream, 8)
{
    Obj *o = obj_get(ARG(0), K_STREAM);
    if (!o || o->voice < 0)
        return 0;
    if (ARG(1))
        stop(o);
    else if (o->status == SMP_STOPPED) {
        voice_play(o->voice, o->loops);
        o->status = SMP_PLAYING;
    }
    return 0;
}
AIL(stream_status, 4) { Obj *o = obj_get(ARG(0), K_STREAM); return o ? (uint32_t)status_of(o) : SMP_FREE; }
AIL(set_stream_loop_count, 8) { Obj *o = obj_get(ARG(0), K_STREAM); if (o) o->loops = (int)ARG(1); return 0; }
AIL(stream_volume, 4) { Obj *o = obj_get(ARG(0), K_STREAM); return o ? (uint32_t)(o->volume * 127.0f + 0.5f) : 0; }
AIL(set_stream_volume, 8)
{
    Obj *o = obj_get(ARG(0), K_STREAM);
    if (o) { o->volume = (float)(int32_t)ARG(1) / 127.0f; apply_gain(o); }
    return 0;
}

/* ------------------------------------------------------------------ from li2-native's copy */
AIL(lock, 0) { return 0; }
AIL(unlock, 0) { return 0; }
AIL(midiOutOpen, 12)
{
    uint32_t h = obj_new(K_MIDI);
    if (ARG(0)) W32(ARG(0), h);
    if (ARG(1)) W32(ARG(1), 0);
    LOGS("AIL_midiOutOpen -> %08x", h);
    return 0;
}
AIL(midiOutClose, 4) { Obj *o = obj_get(ARG(0), K_MIDI); if (o) release(o); return 0; }
/* (sample, &IDirectSound, &IDirectSoundBuffer): none (ours mix in the runtime) */
AIL(get_DirectSound_info, 12)
{
    if (ARG(1)) W32(ARG(1), 0);
    if (ARG(2)) W32(ARG(2), 0);
    return 0;
}
AIL(sample_loop_count, 4) { Obj *o = obj_get(ARG(0), K_SAMPLE); return o ? (uint32_t)o->loops : 0; }
AIL(3D_sample_loop_count, 4) { Obj *o = obj_get(ARG(0), K_3DSAMPLE); return o ? (uint32_t)o->loops : 0; }
AIL(3D_sample_distances, 12)
{
    Obj *o = obj_get(ARG(0), K_3DSAMPLE);
    if (ARG(1)) W32(ARG(1), fu(o ? o->maxd : 0));
    if (ARG(2)) W32(ARG(2), fu(o ? o->mind : 0));
    return 0;
}
AIL(3D_position, 16)
{
    Obj *o = obj_get(ARG(0), 0);
    for (int k = 0; k < 3; k++)
        if (ARG(1 + k)) W32(ARG(1 + k), fu(o ? o->pos[k] : 0));
    return 0;
}
AIL(set_stream_pan, 8) { Obj *o = obj_get(ARG(0), K_STREAM); if (o) { o->pan = (float)(int32_t)ARG(1) / 127.0f; apply_gain(o); } return 0; }
/* (stream, callback): the old callback (never called: streams are decoded whole) */
AIL(register_stream_callback, 8)
{
    Obj *o = obj_get(ARG(0), K_STREAM);
    uint32_t old = o ? o->callback : 0;
    if (o) o->callback = ARG(1);
    return old;
}
AIL(auto_service_stream, 8) { return 0; }

/* ------------------------------------------------------------------ MIDI and DLS (midi.c) */
/* (weak: a port without midi.c, and so without the DirectMusic library, gets no MIDI) */
__attribute__((weak)) int midi_bank_load(const void *data, size_t len);
__attribute__((weak)) int midi_seq_new(const void *smf, size_t len, int bank);
__attribute__((weak)) void midi_seq_free(int id);
__attribute__((weak)) void midi_seq_play(int id, int loops);
__attribute__((weak)) void midi_seq_pause(int id, int pause);
__attribute__((weak)) void midi_seq_end(int id);
__attribute__((weak)) int midi_seq_state(int id);
__attribute__((weak)) void midi_seq_gain(int id, float g);
static int have_midi(void) { return midi_seq_new != NULL; }

/* a sequence's volume (0-127) scales its channels' volume controllers: their DLS curve, squared */
static float seq_gain(float v) { float g = v < 0 ? 0 : v > 1 ? 1 : v; return g * g; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* a standard MIDI file's length from its own chunks (MThd, MTrk...); 0 if it is not one */
static uint32_t smf_length(uint32_t m)
{
    const uint8_t *p = (const uint8_t *)gptr(m);
    if (memcmp(p, "MThd", 4))
        return 0;
    uint32_t o = 0, tracks = (uint32_t)(p[10] << 8 | p[11]);
    for (uint32_t k = 0; k <= tracks && k < 1024; k++) {
        uint32_t len = be32(p + o + 4);
        if (len > (16u << 20))
            return 0;
        o += 8 + len;
    }
    return o;
}

/* (MIDI, size, &XMIDI, &XMIDI size, flags): the data kept as it is (we read standard MIDI); the
   size can be wrong (LEGO Island 2 passes -1): Miles reads the file's own chunk lengths */
AIL(MIDI_to_XMI, 20)
{
    if (!ARG(0))
        return 0;
    uint32_t n = ARG(1), real = smf_length(ARG(0));
    if (real && (n > real || n == 0))
        n = real;
    if (n > (16u << 20))
        return 0;
    uint32_t d = galloc(n ? n : 1);
    memcpy(gptr(d), gptr(ARG(0)), n);
    if (ARG(2)) W32(ARG(2), d);
    if (ARG(3)) W32(ARG(3), n);
    return 1;
}

static int load_file(const char *win, uint8_t **data, uint32_t *size)
{
    char host[1200];
    if (!win || !path_to_host(win, host, sizeof host, 0))
        return 0;
    FILE *f = fopen(host, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = malloc(n > 0 ? (size_t)n : 1);
    *size = fread(*data, 1, n > 0 ? (size_t)n : 0, f) == (size_t)n ? (uint32_t)n : 0;
    fclose(f);
    if (!*size) { free(*data); return 0; }
    return 1;
}

/* (midi driver, digital driver, library, flags, rate, bits, channels) */
AIL(DLS_open, 28) { LOGS("AIL_DLS_open(%u Hz)", ARG(4)); return obj_new(K_DLS); }
AIL(DLS_close, 8) { Obj *o = obj_get(ARG(0), K_DLS); if (o) release(o); return 0; }
/* (dls, file name, flags): a file id, 0 on failure. Sequences prepared after it play through it;
   the same file loaded again is the same bank. */
static int dls_last = -1;
AIL(DLS_load_file, 12)
{
    static struct { char name[260]; int bank; } loaded[16];
    static int nloaded;
    const char *name = gstr(ARG(1));
    LOGS("AIL_DLS_load_file(%s)", name ? name : "NULL");
    if (!name) return 0;
    for (int i = 0; i < nloaded; i++)
        if (!strcasecmp(loaded[i].name, name)) {
            dls_last = loaded[i].bank;
            return (uint32_t)i + 1;
        }
    uint8_t *d;
    uint32_t n;
    if (!load_file(name, &d, &n)) {
        LOGS("AIL_DLS_load_file(%s): not found", name);
        return 0;
    }
    int b = have_midi() ? midi_bank_load(d, n) : -1;
    free(d);
    if (b < 0 || nloaded >= 16) {
        LOGS("AIL_DLS_load_file(%s): not a DLS collection", name);
        return 0;
    }
    snprintf(loaded[nloaded].name, sizeof loaded[nloaded].name, "%s", name);
    loaded[nloaded].bank = b;
    dls_last = b;
    return (uint32_t)++nloaded;
}
AIL(allocate_sequence_handle, 4) { return obj_new(K_SEQUENCE); }
AIL(release_sequence_handle, 4)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (o) {
        if (o->midi >= 0) midi_seq_free(o->midi);
        o->midi = -1;
        release(o);
    }
    return 0;
}
/* (sequence, XMIDI, sequence number): 1 = ready */
AIL(init_sequence, 12)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (!o) return 0;
    if (o->midi >= 0) midi_seq_free(o->midi);
    uint32_t n = ARG(1) ? smf_length(ARG(1)) : 0;
    o->midi = n && have_midi() ? midi_seq_new(gptr(ARG(1)), n, dls_last) : -1;
    o->status = SMP_DONE;
    LOGS("AIL_init_sequence: %u bytes, bank %d -> %d", n, dls_last, o->midi);
    if (o->midi >= 0) midi_seq_gain(o->midi, seq_gain(o->volume));
    return 1;
}
AIL(start_sequence, 4)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (o) {
        o->status = SMP_PLAYING;
        if (o->midi >= 0) midi_seq_play(o->midi, o->loops);
    }
    return 0;
}
AIL(stop_sequence, 4)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (o && o->status == SMP_PLAYING) {
        o->status = SMP_STOPPED;
        if (o->midi >= 0) midi_seq_pause(o->midi, 1);
    }
    return 0;
}
AIL(resume_sequence, 4)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (o && o->status == SMP_STOPPED) {
        o->status = SMP_PLAYING;
        if (o->midi >= 0) midi_seq_pause(o->midi, 0);
    }
    return 0;
}
AIL(end_sequence, 4)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (o) {
        o->status = SMP_DONE;
        if (o->midi >= 0) midi_seq_end(o->midi);
    }
    return 0;
}
/* a playing sequence is done once midi.c has played it through (with no sound output: never) */
AIL(sequence_status, 4)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (!o) return SMP_FREE;
    if (o->status == SMP_PLAYING && (o->midi < 0 || midi_seq_state(o->midi) < 0))
        o->status = SMP_DONE;
    return (uint32_t)o->status;
}
/* (sequence, volume 0-127, milliseconds) */
AIL(set_sequence_volume, 12)
{
    Obj *o = obj_get(ARG(0), K_SEQUENCE);
    if (o) {
        o->volume = (float)(int32_t)ARG(1) / 127.0f;
        if (o->midi >= 0) midi_seq_gain(o->midi, seq_gain(o->volume));
    }
    return 0;
}
AIL(sequence_volume, 4) { Obj *o = obj_get(ARG(0), K_SEQUENCE); return o ? (uint32_t)(o->volume * 127.0f + 0.5f) : 0; }
