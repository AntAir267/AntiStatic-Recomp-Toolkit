/* FMOD 3 stand-in (from penguins-native): the 17 FSOUND_* functions Penguins! imports from fmod.dll, over the
 * runtime's mixer voices (sound.h). Our own code; no FMOD code or headers.
 *
 * What the game does with them [verified: call sites in the game's disassembly]:
 *   FSOUND_SetBufferSize, then FSOUND_Init(44100, 16, flags) (0x427cfa)
 *   FSOUND_Sample_Load(FSOUND_FREE, path, 0x131, 0, 0)   effects: 16BITS|SIGNED|MONO|LOOP_OFF,
 *                                                        loaded by file name (0x429edb)
 *   FSOUND_PlaySound(FSOUND_FREE, sample), then FSOUND_SetLoopMode(channel, mode) (0x4674b6)
 *   FSOUND_Stream_Open(path, 0x2132, 0, 0)               music: ...|2D|LOOP_NORMAL (0x42839f)
 *   FSOUND_Stream_Play(FSOUND_FREE, stream) (0x4283c1)
 * The files [verified: headers of every file in Resources]: every .wav is Microsoft ADPCM,
 * mono, 22050 Hz; the music is Ogg Vorbis (stb_vorbis, third_party/stb_vorbis).
 *
 * FMOD 3's constants as the game uses them: FSOUND_FREE = -1, FSOUND_ALL = -3 [guess: from
 * FMOD 3's documentation as remembered; the game passes -1 for FREE, -3 not seen yet];
 * LOOP_OFF 1, LOOP_NORMAL 2, LOOP_BIDI 4 [verified: 0x131 / 0x2132 decode consistently].
 * Volumes are 0..255. Sample and stream handles are small opaque numbers (the game only passes
 * them back) [guess: no dereference seen].
 */
#include "runtime.h"
#include "sound.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "../../third_party/stb_vorbis/stb_vorbis.c"

#define FSOUND_FREE (-1)
#define FSOUND_ALL (-3)
#define FS_LOOP_OFF 1
#define FS_LOOP_NORMAL 2
#define FS_LOOP_BIDI 4

#define MAX_SAMPLES 512
#define MAX_STREAMS 32
#define MAX_CHANNELS 64
#define HANDLE_SAMPLE 0x7e000000u
#define HANDLE_STREAM 0x7f000000u

typedef struct { int16_t *pcm; uint32_t frames; int channels, rate; unsigned mode; } Pcm;
typedef struct {
    int used, voice;       /* voice = mixer id or -1 */
    unsigned serial;
    int volume, freq, base_rate;
    int stream;            /* stream index this channel plays, or -1 */
} Chan;

static pthread_mutex_t fm_lock = PTHREAD_MUTEX_INITIALIZER;
static Pcm samples[MAX_SAMPLES];
static Pcm streams[MAX_STREAMS];
static int stream_chan[MAX_STREAMS];
static Chan chans[MAX_CHANNELS];
static unsigned next_serial = 1;
static int sfx_master = 255;

/* ------------------------------------------------------------------ decoders */
static uint8_t *read_host_file(const char *win, size_t *len)
{
    char host[1024];
    if (!path_to_host(win, host, sizeof host, 0))
        return NULL;
    FILE *f = fopen(host, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *len = d ? (size_t)n : 0;
    return d;
}

static int16_t clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* Microsoft ADPCM (WAVE_FORMAT_ADPCM = 2): blocks of a preamble per channel (predictor index,
   delta, two history samples) then 4-bit nibbles, high nibble first. */
static int decode_msadpcm(const uint8_t *fmt, uint32_t fmt_len, const uint8_t *d, uint32_t n, Pcm *out)
{
    static const int adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614,
                                   768, 614, 512, 409, 307, 230, 230, 230 };
    int ch = fmt[2] | fmt[3] << 8;
    int rate = (int)(fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24);
    int align = fmt[12] | fmt[13] << 8;
    if (ch < 1 || ch > 2 || align < 7 * ch || fmt_len < 20)
        return 0;
    int ncoef = fmt[20] | fmt[21] << 8;
    int coef1[32], coef2[32];
    if (ncoef < 1 || ncoef > 32 || fmt_len < 22u + 4u * (uint32_t)ncoef)
        return 0;
    for (int i = 0; i < ncoef; i++) {
        coef1[i] = (int16_t)(fmt[22 + 4 * i] | fmt[23 + 4 * i] << 8);
        coef2[i] = (int16_t)(fmt[24 + 4 * i] | fmt[25 + 4 * i] << 8);
    }
    uint32_t per_block = (uint32_t)(2 + (align - 7 * ch) * 2 / ch);
    uint32_t blocks = (n + (uint32_t)align - 1) / (uint32_t)align;
    int16_t *pcm = malloc((size_t)blocks * per_block * (size_t)ch * 2);
    if (!pcm)
        return 0;
    uint32_t frames = 0;
    for (uint32_t off = 0; off + 7u * (uint32_t)ch <= n; off += (uint32_t)align) {
        const uint8_t *b = d + off;
        uint32_t blen = n - off < (uint32_t)align ? n - off : (uint32_t)align;
        int pred[2], delta[2], s1[2], s2[2];
        for (int c = 0; c < ch; c++) {
            pred[c] = b[c] < ncoef ? b[c] : 0;
            delta[c] = (int16_t)(b[ch + 2 * c] | b[ch + 2 * c + 1] << 8);
            s1[c] = (int16_t)(b[3 * ch + 2 * c] | b[3 * ch + 2 * c + 1] << 8);
            s2[c] = (int16_t)(b[5 * ch + 2 * c] | b[5 * ch + 2 * c + 1] << 8);
        }
        int16_t *o = pcm + (size_t)frames * ch;
        for (int c = 0; c < ch; c++) o[c] = (int16_t)s2[c];
        for (int c = 0; c < ch; c++) o[ch + c] = (int16_t)s1[c];
        uint32_t got = 2;
        int c = 0;
        for (uint32_t i = 7u * (uint32_t)ch; i < blen; i++) {
            for (int half = 0; half < 2; half++) {
                int nib = half ? b[i] & 15 : b[i] >> 4;
                int sn = nib & 8 ? nib - 16 : nib;
                int p = (s1[c] * coef1[pred[c]] + s2[c] * coef2[pred[c]]) / 256 + sn * delta[c];
                int16_t v = clamp16(p);
                s2[c] = s1[c];
                s1[c] = v;
                delta[c] = delta[c] * adapt[nib] / 256;
                if (delta[c] < 16) delta[c] = 16;
                o[got * (uint32_t)ch + (uint32_t)c] = v;
                if (++c == ch) { c = 0; got++; }
            }
        }
        frames += got;
    }
    out->pcm = pcm;
    out->frames = frames;
    out->channels = ch;
    out->rate = rate;
    return 1;
}

static int decode_wav(const uint8_t *d, size_t n, Pcm *out)
{
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4))
        return 0;
    const uint8_t *fmt = NULL, *data = NULL;
    uint32_t fmt_len = 0, data_len = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len = (uint32_t)(d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24);
        if (len > n - p - 8) len = (uint32_t)(n - p - 8);
        if (!memcmp(d + p, "fmt ", 4)) { fmt = d + p + 8; fmt_len = len; }
        if (!memcmp(d + p, "data", 4)) { data = d + p + 8; data_len = len; }
        p += 8 + len + (len & 1);
    }
    if (!fmt || !data || fmt_len < 16)
        return 0;
    int tag = fmt[0] | fmt[1] << 8, ch = fmt[2] | fmt[3] << 8, bits = fmt[14] | fmt[15] << 8;
    int rate = (int)(fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24);
    if (tag == 2)
        return decode_msadpcm(fmt, fmt_len, data, data_len, out);
    if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2)
        return 0;
    uint32_t frames = data_len / (uint32_t)(ch * bits / 8);
    int16_t *pcm = malloc((size_t)frames * ch * 2 + 2);
    if (!pcm)
        return 0;
    for (uint32_t i = 0; i < frames * (uint32_t)ch; i++)
        pcm[i] = bits == 8 ? (int16_t)((data[i] - 128) << 8) : (int16_t)(data[2 * i] | data[2 * i + 1] << 8);
    out->pcm = pcm;
    out->frames = frames;
    out->channels = ch;
    out->rate = rate;
    return 1;
}

static int decode_file(const char *win, Pcm *out)
{
    size_t n;
    uint8_t *d = read_host_file(win, &n);
    if (!d)
        return 0;
    int ok = 0;
    if (n >= 4 && !memcmp(d, "OggS", 4)) {
        int ch = 0, rate = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_memory(d, (int)n, &ch, &rate, &pcm);
        if (frames > 0 && pcm) {
            out->pcm = pcm;
            out->frames = (uint32_t)frames;
            out->channels = ch;
            out->rate = rate;
            ok = 1;
        }
    } else
        ok = decode_wav(d, n, out);
    free(d);
    return ok;
}

/* ------------------------------------------------------------------ channels */
static void chan_release(Chan *ch)
{
    if (ch->voice >= 0)
        voice_free(ch->voice);
    if (ch->stream >= 0 && stream_chan[ch->stream] == (int)(ch - chans))
        stream_chan[ch->stream] = -1;
    memset(ch, 0, sizeof *ch);
    ch->voice = -1;
    ch->stream = -1;
}

static void reap_finished(void)
{
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].used && chans[i].voice >= 0 && !voice_playing(chans[i].voice, NULL))
            chan_release(&chans[i]);
}

static uint32_t chan_handle(int i) { return (uint32_t)i | chans[i].serial << 12; }

static Chan *chan_of(uint32_t h)
{
    int i = (int)(h & 0xfff);
    if (i >= MAX_CHANNELS || !chans[i].used || chans[i].serial != (h >> 12 & 0xfffff))
        return NULL;
    return &chans[i];
}

static void chan_gain(Chan *ch, int master)
{
    float g = (float)ch->volume / 255.0f * (float)master / 255.0f;
    if (ch->voice >= 0)
        voice_set_gain(ch->voice, g, g);
}

static int chan_start(int want, const Pcm *p, int stream)
{
    reap_finished();
    int i = -1;
    if (want >= 0 && want < MAX_CHANNELS) {
        i = want;
        if (chans[i].used) chan_release(&chans[i]);
    } else
        for (int k = 0; k < MAX_CHANNELS; k++)
            if (!chans[k].used) { i = k; break; }
    if (i < 0)
        return -1;
    int v = voice_new(p->pcm, p->frames, p->channels, p->rate);
    if (v < 0)
        return -1;
    Chan *ch = &chans[i];
    ch->used = 1;
    ch->voice = v;
    ch->serial = next_serial++ & 0xfffff;
    if (!ch->serial) ch->serial = next_serial++ & 0xfffff;
    ch->volume = 255;
    ch->freq = ch->base_rate = p->rate;
    ch->stream = stream;
    chan_gain(ch, stream >= 0 ? 255 : sfx_master);
    voice_play(v, p->mode & (FS_LOOP_NORMAL | FS_LOOP_BIDI) ? 0 : 1);
    return i;
}

/* Every channel a handle names: one, or all for FSOUND_ALL. */
#define FOR_CHANNELS(h, ch) \
    for (int _i = 0; _i < MAX_CHANNELS; _i++) \
        for (Chan *ch = (int32_t)(h) == FSOUND_ALL ? (chans[_i].used ? &chans[_i] : NULL) \
                                                   : (_i == 0 ? chan_of(h) : NULL); ch; ch = NULL)

/* ------------------------------------------------------------------ the API */
API(FMOD, _FSOUND_SetBufferSize_4, 1) { return 1; }

API(FMOD, _FSOUND_Init_12, 3)
{
    LOG(LOG_SOUND, "FSOUND_Init(%u, %u, 0x%x)", ARG(0), ARG(1), ARG(2));
    pthread_mutex_lock(&fm_lock);
    for (int i = 0; i < MAX_CHANNELS; i++) { chans[i].voice = -1; chans[i].stream = -1; }
    for (int i = 0; i < MAX_STREAMS; i++) stream_chan[i] = -1;
    pthread_mutex_unlock(&fm_lock);
    return 1;
}

API(FMOD, _FSOUND_Close_0, 0)
{
    pthread_mutex_lock(&fm_lock);
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].used) chan_release(&chans[i]);
    pthread_mutex_unlock(&fm_lock);
    return 0;
}

API(FMOD, _FSOUND_SetSFXMasterVolume_4, 1)
{
    pthread_mutex_lock(&fm_lock);
    sfx_master = (int)ARG(0) > 255 ? 255 : (int)ARG(0) < 0 ? 0 : (int)ARG(0);
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (chans[i].used && chans[i].stream < 0) chan_gain(&chans[i], sfx_master);
    pthread_mutex_unlock(&fm_lock);
    return 0;
}

API(FMOD, _FSOUND_Sample_Load_20, 5)
{
    const char *name = gstr(ARG(1));
    unsigned mode = ARG(2);
    if (!name || mode & 0x8000) {        /* FSOUND_LOADMEMORY: not used by this game */
        LOG(LOG_ALWAYS, "FSOUND_Sample_Load: unsupported mode 0x%x", mode);
        return 0;
    }
    pthread_mutex_lock(&fm_lock);
    int i = 0;
    while (i < MAX_SAMPLES && samples[i].pcm) i++;
    if (i == MAX_SAMPLES)
        LOG(LOG_ALWAYS, "FSOUND_Sample_Load(%s): all %d sample slots taken, refused", name, MAX_SAMPLES);
    uint32_t r = 0;
    if (i < MAX_SAMPLES && decode_file(name, &samples[i])) {
        samples[i].mode = mode;
        r = HANDLE_SAMPLE + (uint32_t)i;
    }
    pthread_mutex_unlock(&fm_lock);
    LOG(LOG_SOUND, "FSOUND_Sample_Load(%s, 0x%x) -> 0x%x", name, mode, r);
    if (!r) LOG(LOG_ALWAYS, "FSOUND_Sample_Load: can't load %s", name);
    return r;
}

API(FMOD, _FSOUND_Sample_Free_4, 1)
{
    uint32_t h = ARG(0) - HANDLE_SAMPLE;
    pthread_mutex_lock(&fm_lock);
    if (h < MAX_SAMPLES && samples[h].pcm) {   /* channels hold their own copies (voice_new) */
        free(samples[h].pcm);
        memset(&samples[h], 0, sizeof samples[h]);
    }
    pthread_mutex_unlock(&fm_lock);
    return 0;
}

API(FMOD, _FSOUND_PlaySound_8, 2)
{
    uint32_t h = ARG(1) - HANDLE_SAMPLE;
    pthread_mutex_lock(&fm_lock);
    int i = h < MAX_SAMPLES && samples[h].pcm ? chan_start((int32_t)ARG(0), &samples[h], -1) : -1;
    uint32_t r = i < 0 ? (uint32_t)-1 : chan_handle(i);
    pthread_mutex_unlock(&fm_lock);
    return r;
}

API(FMOD, _FSOUND_StopSound_4, 1)
{
    pthread_mutex_lock(&fm_lock);
    int any = 0;
    FOR_CHANNELS(ARG(0), ch) { chan_release(ch); any = 1; }
    pthread_mutex_unlock(&fm_lock);
    return (uint32_t)any;
}

API(FMOD, _FSOUND_IsPlaying_4, 1)
{
    pthread_mutex_lock(&fm_lock);
    Chan *ch = chan_of(ARG(0));
    int r = ch && ch->voice >= 0 && voice_playing(ch->voice, NULL);
    pthread_mutex_unlock(&fm_lock);
    return (uint32_t)r;
}

API(FMOD, _FSOUND_SetVolume_8, 2)
{
    int v = (int)ARG(1) > 255 ? 255 : (int)ARG(1) < 0 ? 0 : (int)ARG(1);
    pthread_mutex_lock(&fm_lock);
    int any = 0;
    FOR_CHANNELS(ARG(0), ch) { ch->volume = v; chan_gain(ch, ch->stream >= 0 ? 255 : sfx_master); any = 1; }
    pthread_mutex_unlock(&fm_lock);
    return (uint32_t)any;
}

API(FMOD, _FSOUND_SetFrequency_8, 2)
{
    pthread_mutex_lock(&fm_lock);
    int any = 0;
    FOR_CHANNELS(ARG(0), ch) {
        ch->freq = (int)ARG(1);
        if (ch->voice >= 0 && ch->freq > 0) voice_set_rate(ch->voice, (uint32_t)ch->freq);
        any = 1;
    }
    pthread_mutex_unlock(&fm_lock);
    return (uint32_t)any;
}

API(FMOD, _FSOUND_GetFrequency_4, 1)
{
    pthread_mutex_lock(&fm_lock);
    Chan *ch = chan_of(ARG(0));
    uint32_t r = ch ? (uint32_t)ch->freq : 0;
    pthread_mutex_unlock(&fm_lock);
    return r;
}

API(FMOD, _FSOUND_SetLoopMode_8, 2)
{
    unsigned mode = ARG(1);
    pthread_mutex_lock(&fm_lock);
    int any = 0;
    FOR_CHANNELS(ARG(0), ch) {
        /* on a playing voice, voice_play only changes its looping */
        if (ch->voice >= 0 && voice_playing(ch->voice, NULL))
            voice_play(ch->voice, mode & (FS_LOOP_NORMAL | FS_LOOP_BIDI) ? 0 : 1);
        any = 1;
    }
    pthread_mutex_unlock(&fm_lock);
    return (uint32_t)any;
}

API(FMOD, _FSOUND_Stream_Open_16, 4)
{
    const char *name = gstr(ARG(0));
    unsigned mode = ARG(1);
    if (!name || mode & 0x8000) {
        LOG(LOG_ALWAYS, "FSOUND_Stream_Open: unsupported mode 0x%x", mode);
        return 0;
    }
    pthread_mutex_lock(&fm_lock);
    int i = 0;
    while (i < MAX_STREAMS && streams[i].pcm) i++;
    if (i == MAX_STREAMS)
        LOG(LOG_ALWAYS, "FSOUND_Stream_Open(%s): all %d stream slots taken, refused", name, MAX_STREAMS);
    uint32_t r = 0;
    /* decoded whole when opened [to do: decode as it plays if load time shows] */
    if (i < MAX_STREAMS && decode_file(name, &streams[i])) {
        streams[i].mode = mode;
        stream_chan[i] = -1;
        r = HANDLE_STREAM + (uint32_t)i;
    }
    pthread_mutex_unlock(&fm_lock);
    LOG(LOG_SOUND, "FSOUND_Stream_Open(%s, 0x%x) -> 0x%x", name, mode, r);
    if (!r) LOG(LOG_ALWAYS, "FSOUND_Stream_Open: can't open %s", name);
    return r;
}

API(FMOD, _FSOUND_Stream_Play_8, 2)
{
    uint32_t h = ARG(1) - HANDLE_STREAM;
    pthread_mutex_lock(&fm_lock);
    int i = -1;
    if (h < MAX_STREAMS && streams[h].pcm) {
        if (stream_chan[h] >= 0) chan_release(&chans[stream_chan[h]]);
        i = chan_start((int32_t)ARG(0), &streams[h], (int)h);
        if (i >= 0) stream_chan[h] = i;
    }
    uint32_t r = i < 0 ? (uint32_t)-1 : chan_handle(i);
    pthread_mutex_unlock(&fm_lock);
    return r;
}

API(FMOD, _FSOUND_Stream_GetTime_4, 1)
{
    uint32_t h = ARG(0) - HANDLE_STREAM;
    pthread_mutex_lock(&fm_lock);
    uint32_t ms = 0;
    if (h < MAX_STREAMS && streams[h].pcm && stream_chan[h] >= 0) {
        Chan *ch = &chans[stream_chan[h]];
        uint32_t pos = 0;
        if (ch->voice >= 0) voice_playing(ch->voice, &pos);
        ms = (uint32_t)((uint64_t)pos * 1000 / (uint32_t)(streams[h].rate > 0 ? streams[h].rate : 44100));
    }
    pthread_mutex_unlock(&fm_lock);
    return ms;
}

API(FMOD, _FSOUND_Stream_Close_4, 1)
{
    uint32_t h = ARG(0) - HANDLE_STREAM;
    pthread_mutex_lock(&fm_lock);
    int ok = 0;
    if (h < MAX_STREAMS && streams[h].pcm) {
        if (stream_chan[h] >= 0) chan_release(&chans[stream_chan[h]]);
        free(streams[h].pcm);
        memset(&streams[h], 0, sizeof streams[h]);
        ok = 1;
    }
    pthread_mutex_unlock(&fm_lock);
    return (uint32_t)ok;
}
