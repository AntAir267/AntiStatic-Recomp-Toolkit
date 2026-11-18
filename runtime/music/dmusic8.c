/* DirectMusic 8 stand-in (created through CoCreateInstance) for LEGO Island Xtreme Stunts'
   DirectMusic sounds: the ambiences and building sounds of the zones (36 segments in the zones'
   Sounds folders and music\, MSSnd_Midi.cpp). Every other sound and the music are .ogg/.wav
   through DirectSound. The objects are ours; the notes are played by GothicKit's dmusic engine
   (third_party/dmusic, li2-native's patched copy: styles, bands, DLS instruments through
   TinySoundFont), mixed on the mixing thread as one streaming source (sound.h). Forked from
   li2-native's DirectX 6/7 stand-in, 2026-11-06.

   What the game does: work/dmusic-inventory.md (every call, with addresses). In short:
   - MSMusic (0x42f2d0): Performance8 by COM, InitAudio (its own DirectSound8 handed in),
     16 x CreateStandardAudioPath(DYNAMIC_STEREO), AddNotificationType(SEGMENT); a 20 ms timer
     polls GetNotificationPMsg.
   - Its own loader makes each object by CoCreateInstance(class, IID_IDirectMusicObject),
     IPersistStream::Load(its own IStream over the whole file). So each object reads its file
     at Load; GetDescriptor gives the file's own GUID so the loader caches it (else the 4.3 MB
     DLS is read again for every reference).
   - A sound instance (0x42dff0): an audio path (stereo, or 3D with an IDirectSound3DBuffer
     whose SetAllParameters places it), SetVolume, Segment SetRepeats (infinite or 0),
     PlaySegmentEx(segment, flags 0x80 = secondary, &state, pFrom, path); Stop(NULL, state).
   - SEGEND for a one-shot instance: the game finds the instance by comparing the notification's
     punkUser (QI'd to SegmentState8) with the pointer it holds: it must be the same object,
     and only for states the game still holds (it dereferences NULL otherwise).

   Here: one voice per segment state, each with its own engine performance; a voice ends after
   the segment's length on the game's clock (one-shots: a SEGEND then), loops by playing again.
   A segment's references (styles, DLS by file name) resolve in the folder its file is in on the
   disc (found by its bytes): file names repeat across zones with different contents.
   Methods the game never calls answer E_NOTIMPL and log once (LOG_ALWAYS). */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dmusic.h>
#include "runtime.h"
#include "sound.h"

#define LOGS(...) LOG(LOG_SOUND, __VA_ARGS__)
#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define DMUS_E_NOT_FOUND 0x88781193u
#define DMUS_E_TYPE_UNSUPPORTED 0x88781111u
#define DMUS_E_TRACK_NOT_FOUND 0x88781166u

/* ------------------------------------------------------------------ GUIDs */
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } G;
#define DMG(x) {x, 0xb39b, 0x11d1, {0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd}}
static const G CLSID_DirectMusicPerformance = DMG(0xd2ac2881);
static const G CLSID_DirectMusicSegment = DMG(0xd2ac2882);
static const G CLSID_DirectMusicStyle = DMG(0xd2ac288a);
static const G CLSID_DirectMusicChordMap = DMG(0xd2ac288f);
static const G CLSID_DirectMusicBand = {0x79ba9e00, 0xb6ee, 0x11d1, {0x86, 0xbe, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef}};
static const G CLSID_DirectMusicCollection = {0x480ff4b0, 0x28b2, 0x11d1, {0xbe, 0xf7, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef}};
static const G IID_IDirectMusicObject = DMG(0xd2ac28b5);
static const G IID_IPersistStream = {0x00000109, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const G IID_IPersist = {0x0000010c, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const G IID_IDirectSound3DBuffer = {0x279afa86, 0x4981, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}};
static const G GUID_PerfMasterVolume = DMG(0xd2ac28b1);
static const G GUID_PerfMasterTempo = DMG(0xd2ac28b0);
static const G GUID_NOTIFICATION_SEGMENT = DMG(0xd2ac2899);

static int is(uint32_t a, const G *g) { return a && guid_eq(a, g->d1, g->d2, g->d3, g->d4); }

/* ------------------------------------------------------------------ files and folders */
static char cd_root[1100];              /* host D:\LIXS\CD */

static void *read_host(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *b = malloc(n > 0 ? (size_t)n : 1);
    *len = fread(b, 1, n > 0 ? (size_t)n : 0, f);
    fclose(f);
    return b;
}

static int find_in(const char *dir, const char *base, char *out, size_t n)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)))
        if (!strcasecmp(e->d_name, base)) {
            snprintf(out, n, "%s/%s", dir, e->d_name);
            found = 1;
        }
    closedir(d);
    return found;
}

/* every .sgt on the disc (path, size), to find which folder a loaded segment came from */
typedef struct { char path[1200]; long size; } SgtFile;
static SgtFile *sgts;
static int nsgts;
static void index_sgts(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st))
            continue;
        size_t n = strlen(e->d_name);
        if (S_ISDIR(st.st_mode) && depth < 4)
            index_sgts(p, depth + 1);
        else if (n > 4 && !strcasecmp(e->d_name + n - 4, ".sgt")) {
            sgts = realloc(sgts, sizeof *sgts * (size_t)(nsgts + 1));
            snprintf(sgts[nsgts].path, sizeof sgts[nsgts].path, "%s", p);
            sgts[nsgts++].size = (long)st.st_size;
        }
    }
    if (d) closedir(d);
}

/* one engine loader per folder (its references resolve there, then in music\) */
typedef struct { char dir[1200]; DmLoader *loader; } Folder;
static Folder folders[32];
static int nfolders;

/* the segments' file images, by our names "mem:<n>" */
typedef struct { uint8_t *data; size_t len; } Blob;
static Blob blobs[128];
static int nblobs;

static void *resolve(void *ctx, char const *name, size_t *len)
{
    Folder *fo = ctx;
    if (!strncmp(name, "mem:", 4)) {
        int i = atoi(name + 4);
        if (i < 0 || i >= nblobs)
            return NULL;
        void *b = malloc(blobs[i].len);
        memcpy(b, blobs[i].data, blobs[i].len);
        *len = blobs[i].len;
        return b;
    }
    const char *base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    char path[1400], music[1200];
    snprintf(music, sizeof music, "%s/music", cd_root);
    if ((fo && find_in(fo->dir, base, path, sizeof path)) || find_in(music, base, path, sizeof path))
        return read_host(path, len);
    LOG(LOG_ALWAYS, "DirectMusic: no file %s (folder %s)", name, fo ? fo->dir : "?");
    return NULL;
}

static void engine_init(void)
{
    static int done;
    if (done++)
        return;
    Dm_setLoggerDefault(DmLogLevel_ERROR);
    char p[1200];
#ifdef PORT_DMUSIC_SEARCH_DIR
    /* a folder whose segment files are found by name wherever they sit (port.h; Xtreme Stunts:
       its disc's "D:\\LIXS\\CD") */
    if (path_to_host(PORT_DMUSIC_SEARCH_DIR, p, sizeof p, 0)) {
        snprintf(cd_root, sizeof cd_root, "%s", p);
        index_sgts(cd_root, 0);
    }
#else
    (void)p;
#endif
    LOGS("DirectMusic: %d segment files on the disc", nsgts);
}

static Folder *folder_of(const char *dir)
{
    for (int i = 0; i < nfolders; i++)
        if (!strcmp(folders[i].dir, dir))
            return &folders[i];
    if (nfolders == 32)
        return &folders[0];
    Folder *fo = &folders[nfolders++];
    snprintf(fo->dir, sizeof fo->dir, "%s", dir);
    DmLoader_create(&fo->loader, DmLoader_DOWNLOAD);
    DmLoader_addResolver(fo->loader, resolve, fo);
    /* the folder's DLS collections first: DirectMusic's loader finds instruments a band names
       only by GUID by scanning (li2-native) */
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcasecmp(e->d_name + n - 4, ".dls"))
            DmLoader_preloadDls(fo->loader, e->d_name);
    }
    if (d) closedir(d);
    return fo;
}

/* the disc folder holding a file with these bytes (music\ if none) */
static Folder *folder_for(const uint8_t *data, size_t len)
{
    for (int i = 0; i < nsgts; i++) {
        if (sgts[i].size != (long)len)
            continue;
        size_t n;
        uint8_t *b = read_host(sgts[i].path, &n);
        int same = b && n == len && !memcmp(b, data, len);
        free(b);
        if (same) {
            char dir[1200];
            snprintf(dir, sizeof dir, "%s", sgts[i].path);
            *strrchr(dir, '/') = 0;
            return folder_of(dir);
        }
    }
    char music[1200];
    snprintf(music, sizeof music, "%s/music", cd_root);
    LOG(LOG_ALWAYS, "DirectMusic: a segment (%zu bytes) matches no file on the disc", len);
    return folder_of(music);
}

/* the file's own GUID: the top-level "guid" chunk (segments, styles, bands) or "dlid" (DLS) */
static int riff_guid(const uint8_t *d, size_t len, G *out)
{
    if (len < 12 || memcmp(d, "RIFF", 4))
        return 0;
    size_t end = 8 + (size_t)(d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24);
    if (end > len) end = len;
    for (size_t p = 12; p + 8 <= end;) {
        uint32_t n = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
        if ((!memcmp(d + p, "guid", 4) || !memcmp(d + p, "dlid", 4)) && n >= 16 && p + 24 <= end) {
            memcpy(out, d + p + 8, 16);
            return 1;
        }
        p += 8 + n + (n & 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ objects
   One host record per object; each interface the game can ask for is its own program object
   (vtable) pointing at the record. */
enum { K_PERF = 1, K_SEG, K_STATE, K_STYLE, K_BAND, K_COLL, K_CHORDMAP, K_PATH, K_DM, K_BUF3D };
static const char *const kind_names[] = {"?", "Performance", "Segment", "SegmentState", "Style",
    "Band", "Collection", "ChordMap", "AudioPath", "DirectMusic", "3DBuffer"};
typedef struct Host {
    int kind, refs;
    uint32_t obj_main, obj_object, obj_persist;   /* the interface, IDirectMusicObject, IPersistStream */
    G clsid, guid;
    int has_guid;
    DmSegment *seg;                     /* segments: the engine's */
    Folder *folder;                     /* segments: where its references resolve */
    uint32_t repeats;                   /* segments: SetRepeats (0xffffffff: for ever) */
    int voice;                          /* states: their voice (-1 when gone) */
    uint32_t seg_obj;                   /* states: their segment */
    /* audio paths */
    int vol_mb, active, is3d;
    uint32_t buf3d;                     /* 3D paths: the IDirectSound3DBuffer */
    float pos[3], mind, maxd;
    uint32_t mode;
    struct Host *path;                  /* 3D buffers: their path */
} Host;
#define HOST(o) ((Host *)com_host(o))
static ComClass perf_cls, seg_cls, state_cls, generic_cls, object_cls, persist_cls, path_cls, dm_cls, buf3d_cls;

static Host *host_new(int kind)
{
    Host *h = calloc(1, sizeof *h);
    h->kind = kind;
    h->refs = 1;
    h->voice = -1;
    return h;
}

/* ------------------------------------------------------------------ voices: segment states playing */
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static float master_gain = 1.0f;
typedef struct {
    int used;
    uint32_t state_obj;                 /* the SegmentState the game holds */
    DmSegment *seg;
    Host *path;                         /* its audio path (NULL: full volume, centred) */
    int primary;
    int loop;                           /* repeats for ever */
    double start_ms, len_ms;            /* on the game's clock */
    int ended;                          /* a one-shot past its end: SEGEND sent */
    int stop;                           /* the mixer lets it go */
    DmPerformance *perf;                /* the mixer's, at its rate */
    uint64_t frames;                    /* rendered since (re)started */
} Voice;
#define MAXVOICES 64
static Voice voices[MAXVOICES];

static void voice_drop_locked(int i)
{
    Voice *v = &voices[i];
    if (v->perf) DmPerformance_release(v->perf);
    if (v->seg) DmSegment_release(v->seg);
    if (v->state_obj) HOST(v->state_obj)->voice = -1;
    memset(v, 0, sizeof *v);
}

/* the mixing thread: every voice rendered into the mix at its path's volume and place */
static void music_source(float *out, int frames, int rate, void *ctx)
{
    (void)ctx;
    static float *buf;
    static int cap;
    if (cap < frames) {
        cap = frames;
        buf = realloc(buf, sizeof(float) * 2 * (size_t)cap);
    }
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < MAXVOICES; i++) {
        Voice *v = &voices[i];
        if (!v->used || v->stop || !v->seg)
            continue;
        uint64_t len = (uint64_t)(v->len_ms * rate / 1000.0);
        if (!v->perf) {
            if (DmPerformance_create(&v->perf, (uint32_t)rate) != DmResult_SUCCESS) { v->perf = NULL; continue; }
            DmPerformance_playSegment(v->perf, v->seg, DmTiming_INSTANT);
            v->frames = 0;
        } else if (len && v->frames >= len) {
            if (!v->loop)
                continue;                   /* (a one-shot is over: silent until the game lets it go) */
            DmPerformance_playSegment(v->perf, v->seg, DmTiming_INSTANT);
            v->frames = 0;
        }
        /* gain: master x path volume (hundredths of a dB) x distance (3D paths: inverse distance
           from min distance, as DirectSound's default rolloff), pan from x */
        float g = master_gain * 0.5f, l = 1, r = 1;  /* (0.5: TinySoundFont plays DLS ~6 dB hot, li2-native) */
        Host *p = v->path;
        if (p) {
            g *= p->vol_mb <= -9600 ? 0.0f : powf(10.0f, (float)p->vol_mb / 2000.0f);
            if (p->is3d) {
                float dx = p->pos[0], dy = p->pos[1], dz = p->pos[2];
                float dist = sqrtf(dx * dx + dy * dy + dz * dz), mn = p->mind > 0 ? p->mind : 1;
                if (p->maxd > 0 && dist > p->maxd) g = 0;
                else if (dist > mn) g *= mn / dist;
                float pan = dist > 1e-3f ? dx / dist : 0;   /* -1 left .. 1 right */
                l = pan > 0 ? 1 - pan : 1;
                r = pan < 0 ? 1 + pan : 1;
            }
        }
        DmPerformance_renderPcm(v->perf, buf, (size_t)frames * 2, DmRender_FLOAT | DmRender_STEREO);
        v->frames += (uint64_t)frames;
        if (g > 0)
            for (int k = 0; k < frames; k++) {
                out[2 * k] += buf[2 * k] * g * l;
                out[2 * k + 1] += buf[2 * k + 1] * g * r;
            }
    }
    /* stopped voices go here, on the mixing thread that owns their performances */
    for (int i = 0; i < MAXVOICES; i++)
        if (voices[i].used && voices[i].stop)
            voice_drop_locked(i);
    pthread_mutex_unlock(&mlock);
}

static int mixing;                      /* sound_add_source done (a real output: the mixer drops voices) */
static void voice_stop_locked(int i)
{
    if (!voices[i].used) return;
    if (voices[i].state_obj) HOST(voices[i].state_obj)->voice = -1;
    voices[i].state_obj = 0;
    if (mixing && voices[i].perf) voices[i].stop = 1;   /* (the mixer releases it) */
    else voice_drop_locked(i);
}

/* ------------------------------------------------------------------ IUnknown */
METHOD(m_QueryInterface, 3)
{
    uint32_t riid = ARG(1), ppv = ARG(2);
    Host *h = HOST(ARG(0));
    if (!ppv)
        return E_POINTER;
    uint32_t o = 0;
    if (is(riid, &IID_IPersistStream) || is(riid, &IID_IPersist))
        o = h->obj_persist;
    else if (is(riid, &IID_IDirectMusicObject))
        o = h->obj_object;
    else
        o = h->obj_main;                /* the object's own interface (any version: IUnknown, ...8) */
    W32(ppv, o);
    if (!o) {
        LOG(LOG_ALWAYS, "DirectMusic %s: QueryInterface {" GUID_FMT "}: not provided",
            kind_names[h->kind], GUID_ARGS(riid));
        return E_NOINTERFACE;
    }
    h->refs++;
    return S_OK;
}
METHOD(m_AddRef, 1) { return (uint32_t)++HOST(ARG(0))->refs; }
METHOD(m_Release, 1)
{
    Host *h = HOST(ARG(0));
    int r = --h->refs;
    return (uint32_t)(r < 0 ? 0 : r);
}

#define NOP(name, nargs) METHOD(name, nargs) \
    { static int once; if (!once++) LOGS("DirectMusic " #name ": nothing to do"); return S_OK; }
#define NI(name, nargs) METHOD(name, nargs) \
    { static int once; if (!once++) LOG(LOG_ALWAYS, "DirectMusic " #name ": not implemented"); return E_NOTIMPL; }

/* ------------------------------------------------------------------ IDirectMusicObject */
/* DMUS_OBJECTDESC: dwSize, dwValidData (+4), guidObject (+8), guidClass (+24) */
METHOD(ob_GetDescriptor, 2)
{
    Host *h = HOST(ARG(0));
    uint32_t d = ARG(1);
    if (!d) return E_POINTER;
    uint32_t size = R32(d);
    if (size > 4) memset(gptr(d + 4), 0, size - 4);
    W32(d + 4, 0x2 | (h->has_guid ? 0x1 : 0));   /* DMUS_OBJ_CLASS, DMUS_OBJ_OBJECT */
    if (h->has_guid) memcpy(gptr(d + 8), &h->guid, 16);
    memcpy(gptr(d + 24), &h->clsid, 16);
    return S_OK;
}
NOP(ob_SetDescriptor, 2)
METHOD(ob_ParseDescriptor, 3) { return DMUS_E_TYPE_UNSUPPORTED; }

/* ------------------------------------------------------------------ IPersistStream */
METHOD(ps_GetClassID, 2) { Host *h = HOST(ARG(0)); if (ARG(1)) memcpy(gptr(ARG(1)), &h->clsid, 16); return S_OK; }
METHOD(ps_IsDirty, 1) { return S_FALSE; }
/* Load(stream): the whole file through IStream::Read (vtable +12: Read(this, buf, n, &got));
   the game's Read fails (E_FAIL) on a short read, with the count filled in */
METHOD(ps_Load, 2)
{
    Host *h = HOST(ARG(0));
    uint32_t stm = ARG(1);
    if (!stm) return E_POINTER;
    engine_init();
    uint32_t vt = R32(stm);
    uint32_t buf = galloc(65536), got = galloc(4);
    uint8_t *data = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        W32(got, 0);
        uint32_t hr = guest_call(c, R32(vt + 12), 4, stm, buf, 65536, got);
        uint32_t n = R32(got);
        if (n) {
            if (len + n > cap) {
                cap = (len + n) * 2;
                data = realloc(data, cap);
            }
            memcpy(data + len, gptr(buf), n);
            len += n;
        }
        if ((int32_t)hr < 0 || n < 65536)
            break;
    }
    gfree(buf);
    gfree(got);
    h->has_guid = riff_guid(data, len, &h->guid);
    LOGS("DirectMusic %s: loaded %zu bytes", kind_names[h->kind], len);
    if (h->kind == K_SEG && len) {
        pthread_mutex_lock(&mlock);
        h->folder = folder_for(data, len);
        if (nblobs < 128) {
            char nm[16];
            blobs[nblobs] = (Blob){data, len};
            snprintf(nm, sizeof nm, "mem:%d", nblobs++);
            data = NULL;                 /* (the blob keeps it) */
            if (DmLoader_getSegment(h->folder->loader, nm, &h->seg) != DmResult_SUCCESS) {
                LOG(LOG_ALWAYS, "DirectMusic: a segment in %s did not load", h->folder->dir);
                h->seg = NULL;
            } else
                LOGS("DirectMusic: segment %s (%s), %.2f s", DmSegment_getName(h->seg),
                     h->folder->dir, DmSegment_getLength(h->seg));
        }
        pthread_mutex_unlock(&mlock);
    }
    free(data);                          /* (other objects: the engine reads its own files) */
    return S_OK;
}
METHOD(ps_Save, 3) { return E_NOTIMPL; }
METHOD(ps_GetSizeMax, 2) { return E_NOTIMPL; }

static void (*const object_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release,
    ob_GetDescriptor, ob_SetDescriptor, ob_ParseDescriptor};
static const char *const object_names[] = {"QueryInterface", "AddRef", "Release", "GetDescriptor",
    "SetDescriptor", "ParseDescriptor"};
static void (*const persist_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release,
    ps_GetClassID, ps_IsDirty, ps_Load, ps_Save, ps_GetSizeMax};
static const char *const persist_names[] = {"QueryInterface", "AddRef", "Release", "GetClassID",
    "IsDirty", "Load", "Save", "GetSizeMax"};

/* ------------------------------------------------------------------ IDirectMusic8
   InitAudio hands one back; the game asks it for IDirectMusic8 and keeps it, no calls
   [verified: disassembly 0x42f418-0x42f438, inventory 2.6] */
NI(dm_EnumPort, 3)
NI(dm_CreateMusicBuffer, 4)
NI(dm_CreatePort, 5)
METHOD(dm_EnumMasterClock, 3) { return S_FALSE; }
NI(dm_GetMasterClock, 3)
NOP(dm_SetMasterClock, 2)
NOP(dm_Activate, 2)
NI(dm_GetDefaultPort, 2)
NOP(dm_SetDirectSound, 3)
NOP(dm_SetExternalMasterClock, 2)
static void (*const dm_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release, dm_EnumPort,
    dm_CreateMusicBuffer, dm_CreatePort, dm_EnumMasterClock, dm_GetMasterClock, dm_SetMasterClock,
    dm_Activate, dm_GetDefaultPort, dm_SetDirectSound, dm_SetExternalMasterClock};
static const char *const dm_names[] = {"QueryInterface", "AddRef", "Release", "EnumPort",
    "CreateMusicBuffer", "CreatePort", "EnumMasterClock", "GetMasterClock", "SetMasterClock",
    "Activate", "GetDefaultPort", "SetDirectSound", "SetExternalMasterClock"};

/* ------------------------------------------------------------------ IDirectMusicPerformance8 */
static uint32_t the_perf, the_dm;

static uint32_t new_path(int is3d)
{
    Host *h = host_new(K_PATH);
    h->obj_main = com_new(&path_cls, h);
    h->is3d = is3d;
    h->mind = 1.0f;
    h->maxd = 1e9f;
    return h->obj_main;
}

/* PlaySegmentEx(source, name, transition, flags, i64 start, &state, from, audio path) */
METHOD(perf_PlaySegmentEx, 10)
{
    uint32_t seg_obj = ARG(1), flags = ARG(4), pp_state = ARG(7), from = ARG(8), path = ARG(9);
    Host *s = seg_obj ? HOST(seg_obj) : NULL;
    if (pp_state) W32(pp_state, 0);
    if (!s || s->kind != K_SEG)
        return E_POINTER;
    /* (the state first: the game QIs it without a NULL check) */
    Host *st = host_new(K_STATE);
    st->obj_main = com_new(&state_cls, st);
    st->seg_obj = seg_obj;
    s->refs++;
    pthread_mutex_lock(&mlock);
    if (from && HOST(from)->voice >= 0)                 /* the state this one replaces */
        voice_stop_locked(HOST(from)->voice);
    /* TRACKTYPE 2 passes leftover bits as flags (inventory 2.1): only 0x80 SECONDARY counts */
    int primary = !(flags & 0x80);
    if (primary)
        for (int i = 0; i < MAXVOICES; i++)
            if (voices[i].used && voices[i].primary) voice_stop_locked(i);
    int vi = -1;
    for (int i = 0; i < MAXVOICES && vi < 0; i++)
        if (!voices[i].used) vi = i;
    if (vi >= 0 && s->seg) {
        Voice *v = &voices[vi];
        memset(v, 0, sizeof *v);
        v->used = 1;
        v->state_obj = st->obj_main;
        v->seg = DmSegment_retain(s->seg);
        v->path = path ? HOST(path) : NULL;
        v->primary = primary;
        v->loop = s->repeats == 0xffffffffu;
        v->start_ms = now_ms();
        v->len_ms = DmSegment_getLength(s->seg) * 1000.0 * (v->loop ? 1 : 1 + (s->repeats ? s->repeats : 0));
        st->voice = vi;
    } else if (vi < 0)
        LOG(LOG_ALWAYS, "DirectMusic: more than %d sounds at once", MAXVOICES);
    pthread_mutex_unlock(&mlock);
    LOGS("DirectMusic PlaySegmentEx(%s, flags %#x, path %08x) -> state %08x voice %d%s",
         s->seg ? DmSegment_getName(s->seg) : "?", flags, path, st->obj_main, st->voice,
         s->repeats == 0xffffffffu ? " (loops)" : "");
    if (pp_state) W32(pp_state, st->obj_main);
    return S_OK;
}
/* Stop(segment, state, mt, flags): that state, every state of that segment, or everything */
METHOD(perf_Stop, 5)
{
    uint32_t seg = ARG(1), state = ARG(2);
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < MAXVOICES; i++) {
        Voice *v = &voices[i];
        if (!v->used) continue;
        if ((state && v->state_obj == state) || (!state && seg && v->state_obj && HOST(v->state_obj)->seg_obj == seg) ||
            (!state && !seg))
            voice_stop_locked(i);
    }
    pthread_mutex_unlock(&mlock);
    LOGS("DirectMusic Stop(segment %08x, state %08x)", seg, state);
    return S_OK;
}
/* StopEx(object, i64 time, flags) */
METHOD(perf_StopEx, 5)
{
    uint32_t o = ARG(1);
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < MAXVOICES; i++)
        if (voices[i].used && (!o || voices[i].state_obj == o)) voice_stop_locked(i);
    pthread_mutex_unlock(&mlock);
    return S_OK;
}

/* GetNotificationPMsg(&msg): a SEGEND for the first one-shot voice past its end, on the game's
   clock. DMUS_NOTIFICATION_PMSG as the game reads it (8-byte packing): dwSize, rtTime at +8,
   punkUser at +0x34, guidNotificationType at +0x38, dwNotificationOption at +0x48 [verified:
   inventory 2.1]. The state stays the game's to release. */
METHOD(perf_GetNotificationPMsg, 2)
{
    uint32_t pp = ARG(1);
    if (pp) W32(pp, 0);
    double now = now_ms();
    uint32_t state = 0;
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < MAXVOICES && !state; i++) {
        Voice *v = &voices[i];
        if (v->used && !v->loop && !v->ended && v->state_obj && now - v->start_ms >= v->len_ms) {
            v->ended = 1;
            state = v->state_obj;
            voice_stop_locked(i);
        }
    }
    pthread_mutex_unlock(&mlock);
    if (!state || !pp)
        return S_FALSE;
    uint32_t m = galloc(0x58);
    memset(gptr(m), 0, 0x58);
    W32(m, 0x54);
    uint64_t rt = (uint64_t)(now * 10000.0);
    W32(m + 8, (uint32_t)rt);
    W32(m + 12, (uint32_t)(rt >> 32));
    W32(m + 0x20, 9);                   /* dwType: DMUS_PMSGT_NOTIFICATION */
    W32(m + 0x34, state);
    memcpy(gptr(m + 0x38), &GUID_NOTIFICATION_SEGMENT, 16);
    W32(m + 0x48, 1);                   /* DMUS_NOTIFICATION_SEGEND */
    HOST(state)->refs++;                /* (the message's reference) */
    W32(pp, m);
    LOGS("DirectMusic SEGEND for state %08x", state);
    return S_OK;
}
METHOD(perf_FreePMsg, 2) { if (ARG(1)) gfree(ARG(1)); return S_OK; }

METHOD(perf_Init, 4) { engine_init(); if (ARG(1)) W32(ARG(1), 0); return S_OK; }
METHOD(perf_PlaySegment, 6) { LOG(LOG_ALWAYS, "DirectMusic PlaySegment: not implemented (PlaySegmentEx is)"); return E_NOTIMPL; }
METHOD(perf_GetSegmentState, 3) { if (ARG(1)) W32(ARG(1), 0); return DMUS_E_NOT_FOUND; }
NOP(perf_SetPrepareTime, 2)
METHOD(perf_GetPrepareTime, 2) { if (ARG(1)) W32(ARG(1), 1000); return S_OK; }
NOP(perf_SetBumperLength, 2)
METHOD(perf_GetBumperLength, 2) { if (ARG(1)) W32(ARG(1), 50); return S_OK; }
NI(perf_SendPMsg, 2)
NI(perf_MusicToReferenceTime, 3)
NI(perf_ReferenceToMusicTime, 4)
METHOD(perf_IsPlaying, 3)
{
    uint32_t state = ARG(2);
    return state && HOST(state)->voice >= 0 ? S_OK : S_FALSE;
}
/* GetTime(&rt, &mt): reference time in 100 ns; music time [guess: 768 ticks a beat at 120 bpm] */
METHOD(perf_GetTime, 3)
{
    double ms = now_ms();
    if (ARG(1)) { uint64_t rt = (uint64_t)(ms * 10000.0); W32(ARG(1), (uint32_t)rt); W32(ARG(1) + 4, (uint32_t)(rt >> 32)); }
    if (ARG(2)) W32(ARG(2), (uint32_t)(ms * 768.0 * 2.0 / 1000.0));
    return S_OK;
}
METHOD(perf_AllocPMsg, 3) { if (ARG(2)) W32(ARG(2), 0); return E_FAIL; }
METHOD(perf_GetGraph, 2) { if (ARG(1)) W32(ARG(1), 0); return DMUS_E_NOT_FOUND; }
NOP(perf_SetGraph, 2)
NOP(perf_SetNotificationHandle, 4)
NOP(perf_AddNotificationType, 2)
NOP(perf_RemoveNotificationType, 2)
NOP(perf_AddPort, 2)
NOP(perf_RemovePort, 2)
NOP(perf_AssignPChannelBlock, 4)
NOP(perf_AssignPChannel, 5)
NI(perf_PChannelInfo, 5)
NI(perf_DownloadInstrument, 9)
NOP(perf_Invalidate, 3)
METHOD(perf_GetParam, 7) { return DMUS_E_NOT_FOUND; }
NOP(perf_SetParam, 6)
METHOD(perf_GetGlobalParam, 4)
{
    if (is(ARG(1), &GUID_PerfMasterVolume) && ARG(2) && ARG(3) >= 4) {
        W32(ARG(2), (uint32_t)(int32_t)lrintf(master_gain > 0 ? 2000.0f * log10f(master_gain) : -9600.0f));
        return S_OK;
    }
    if (is(ARG(1), &GUID_PerfMasterTempo) && ARG(2) && ARG(3) >= 4) {
        float one = 1.0f;
        memcpy(gptr(ARG(2)), &one, 4);
        return S_OK;
    }
    return E_NOTIMPL;
}
METHOD(perf_SetGlobalParam, 4)
{
    if (is(ARG(1), &GUID_PerfMasterVolume) && ARG(2) && ARG(3) >= 4) {
        int32_t v = (int32_t)R32(ARG(2));
        master_gain = v <= -9600 ? 0.0f : powf(10.0f, (float)v / 2000.0f);
    }
    return S_OK;
}
METHOD(perf_GetLatencyTime, 2) { if (ARG(1)) { W32(ARG(1), 0); W32(ARG(1) + 4, 0); } return S_OK; }
METHOD(perf_GetQueueTime, 2) { if (ARG(1)) { W32(ARG(1), 0); W32(ARG(1) + 4, 0); } return S_OK; }
NOP(perf_AdjustTime, 3)
METHOD(perf_CloseDown, 1)
{
    pthread_mutex_lock(&mlock);
    for (int i = 0; i < MAXVOICES; i++) voice_stop_locked(i);
    pthread_mutex_unlock(&mlock);
    return S_OK;
}
NI(perf_GetResolvedTime, 5)
NI(perf_MIDIToMusic, 6)
NI(perf_MusicToMIDI, 6)
NI(perf_TimeToRhythm, 7)
NI(perf_RhythmToTime, 7)
/* InitAudio(&dm, &ds, hwnd, default path type, pchannels, flags, params): the game hands in its
   own DirectSound8 (non-NULL) and wants an IDirectMusic back (0x42f418) */
METHOD(perf_InitAudio, 8)
{
    engine_init();
    if (!mixing++)
        sound_add_source(music_source, NULL);
    if (ARG(1)) {
        if (!the_dm) {
            Host *h = host_new(K_DM);
            h->obj_main = com_new(&dm_cls, h);
            the_dm = h->obj_main;
        } else
            HOST(the_dm)->refs++;
        W32(ARG(1), the_dm);
    }
    if (ARG(2) && R32(ARG(2)) == 0) {
        extern uint32_t dsound_new_for_dmusic(void);
        W32(ARG(2), dsound_new_for_dmusic());
    }
    LOGS("DirectMusic InitAudio(path type %u, %u pchannels, flags %#x)", ARG(4), ARG(5), ARG(6));
    return S_OK;
}
NI(perf_ClonePMsg, 3)
/* CreateAudioPath(config, activate, &path): a stereo path (the segments' own configs) */
METHOD(perf_CreateAudioPath, 4) { uint32_t p = new_path(0); HOST(p)->active = ARG(2) != 0; if (ARG(3)) W32(ARG(3), p); return S_OK; }
/* CreateStandardAudioPath(type, pchannels, activate, &path): 6 DYNAMIC_3D, 8 DYNAMIC_STEREO */
METHOD(perf_CreateStandardAudioPath, 5)
{
    uint32_t p = new_path(ARG(1) == 6);
    HOST(p)->active = ARG(3) != 0;
    if (ARG(4)) W32(ARG(4), p);
    return S_OK;
}
NOP(perf_SetDefaultAudioPath, 2)
METHOD(perf_GetDefaultAudioPath, 2) { if (ARG(1)) W32(ARG(1), 0); return DMUS_E_NOT_FOUND; }
METHOD(perf_GetParamEx, 8) { return DMUS_E_NOT_FOUND; }
NOP(perf_SetParamEx, 7)

static void (*const perf_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release, perf_Init,
    perf_PlaySegment, perf_Stop, perf_GetSegmentState, perf_SetPrepareTime, perf_GetPrepareTime,
    perf_SetBumperLength, perf_GetBumperLength, perf_SendPMsg, perf_MusicToReferenceTime,
    perf_ReferenceToMusicTime, perf_IsPlaying, perf_GetTime, perf_AllocPMsg, perf_FreePMsg,
    perf_GetGraph, perf_SetGraph, perf_SetNotificationHandle, perf_GetNotificationPMsg,
    perf_AddNotificationType, perf_RemoveNotificationType, perf_AddPort, perf_RemovePort,
    perf_AssignPChannelBlock, perf_AssignPChannel, perf_PChannelInfo, perf_DownloadInstrument,
    perf_Invalidate, perf_GetParam, perf_SetParam, perf_GetGlobalParam, perf_SetGlobalParam,
    perf_GetLatencyTime, perf_GetQueueTime, perf_AdjustTime, perf_CloseDown, perf_GetResolvedTime,
    perf_MIDIToMusic, perf_MusicToMIDI, perf_TimeToRhythm, perf_RhythmToTime, perf_InitAudio,
    perf_PlaySegmentEx, perf_StopEx, perf_ClonePMsg, perf_CreateAudioPath,
    perf_CreateStandardAudioPath, perf_SetDefaultAudioPath, perf_GetDefaultAudioPath,
    perf_GetParamEx, perf_SetParamEx};
static const char *const perf_names[] = {"QueryInterface", "AddRef", "Release", "Init",
    "PlaySegment", "Stop", "GetSegmentState", "SetPrepareTime", "GetPrepareTime",
    "SetBumperLength", "GetBumperLength", "SendPMsg", "MusicToReferenceTime",
    "ReferenceToMusicTime", "IsPlaying", "GetTime", "AllocPMsg", "FreePMsg", "GetGraph", "SetGraph",
    "SetNotificationHandle", "GetNotificationPMsg", "AddNotificationType", "RemoveNotificationType",
    "AddPort", "RemovePort", "AssignPChannelBlock", "AssignPChannel", "PChannelInfo",
    "DownloadInstrument", "Invalidate", "GetParam", "SetParam", "GetGlobalParam", "SetGlobalParam",
    "GetLatencyTime", "GetQueueTime", "AdjustTime", "CloseDown", "GetResolvedTime", "MIDIToMusic",
    "MusicToMIDI", "TimeToRhythm", "RhythmToTime", "InitAudio", "PlaySegmentEx", "StopEx",
    "ClonePMsg", "CreateAudioPath", "CreateStandardAudioPath", "SetDefaultAudioPath",
    "GetDefaultAudioPath", "GetParamEx", "SetParamEx"};
_Static_assert(sizeof perf_methods / sizeof *perf_methods == 54, "IDirectMusicPerformance8 has 54 methods");

/* ------------------------------------------------------------------ IDirectMusicSegment8 */
METHOD(seg_GetLength, 2)
{
    Host *s = HOST(ARG(0));
    double sec = s->seg ? DmSegment_getLength(s->seg) : 0;
    if (ARG(1)) W32(ARG(1), (uint32_t)(sec * 768.0 * 2.0));   /* [guess: 120 bpm] */
    return S_OK;
}
NOP(seg_SetLength, 2)
METHOD(seg_GetRepeats, 2) { if (ARG(1)) W32(ARG(1), HOST(ARG(0))->repeats); return S_OK; }
METHOD(seg_SetRepeats, 2) { HOST(ARG(0))->repeats = ARG(1); return S_OK; }
METHOD(seg_GetDefaultResolution, 2) { if (ARG(1)) W32(ARG(1), 0); return S_OK; }
NOP(seg_SetDefaultResolution, 2)
METHOD(seg_GetTrack, 5) { if (ARG(4)) W32(ARG(4), 0); return DMUS_E_NOT_FOUND; }
METHOD(seg_GetTrackGroup, 3) { if (ARG(2)) W32(ARG(2), 0); return DMUS_E_NOT_FOUND; }
NI(seg_InsertTrack, 3)
NI(seg_RemoveTrack, 2)
METHOD(seg_InitPlay, 4) { if (ARG(1)) W32(ARG(1), 0); return E_FAIL; }
METHOD(seg_GetGraph, 2) { if (ARG(1)) W32(ARG(1), 0); return DMUS_E_NOT_FOUND; }
NOP(seg_SetGraph, 2)
NOP(seg_AddNotificationType, 2)
NOP(seg_RemoveNotificationType, 2)
METHOD(seg_GetParam, 7) { return DMUS_E_NOT_FOUND; }
/* SetParam(GUID_CommandParam: the groove level): not in the engine [to do]; "no command
   track" makes the game skip its groove replays (0x42e460) */
METHOD(seg_SetParam, 6) { return DMUS_E_TRACK_NOT_FOUND; }
METHOD(seg_Clone, 4) { if (ARG(3)) W32(ARG(3), 0); return E_FAIL; }
NOP(seg_SetStartPoint, 2)
METHOD(seg_GetStartPoint, 2) { if (ARG(1)) W32(ARG(1), 0); return S_OK; }
NOP(seg_SetLoopPoints, 3)
METHOD(seg_GetLoopPoints, 3) { if (ARG(1)) W32(ARG(1), 0); if (ARG(2)) W32(ARG(2), 0); return S_OK; }
NOP(seg_SetPChannelsUsed, 3)
NOP(seg_SetTrackConfig, 6)
/* no config: the game then uses its standard paths [verified: inventory 2.2] */
METHOD(seg_GetAudioPathConfig, 2) { if (ARG(1)) W32(ARG(1), 0); return DMUS_E_NOT_FOUND; }
NI(seg_Compose, 5)
NOP(seg_Download, 2)
NOP(seg_Unload, 2)

static void (*const seg_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release, seg_GetLength,
    seg_SetLength, seg_GetRepeats, seg_SetRepeats, seg_GetDefaultResolution, seg_SetDefaultResolution,
    seg_GetTrack, seg_GetTrackGroup, seg_InsertTrack, seg_RemoveTrack, seg_InitPlay, seg_GetGraph,
    seg_SetGraph, seg_AddNotificationType, seg_RemoveNotificationType, seg_GetParam, seg_SetParam,
    seg_Clone, seg_SetStartPoint, seg_GetStartPoint, seg_SetLoopPoints, seg_GetLoopPoints,
    seg_SetPChannelsUsed, seg_SetTrackConfig, seg_GetAudioPathConfig, seg_Compose, seg_Download,
    seg_Unload};
static const char *const seg_names[] = {"QueryInterface", "AddRef", "Release", "GetLength",
    "SetLength", "GetRepeats", "SetRepeats", "GetDefaultResolution", "SetDefaultResolution",
    "GetTrack", "GetTrackGroup", "InsertTrack", "RemoveTrack", "InitPlay", "GetGraph", "SetGraph",
    "AddNotificationType", "RemoveNotificationType", "GetParam", "SetParam", "Clone",
    "SetStartPoint", "GetStartPoint", "SetLoopPoints", "GetLoopPoints", "SetPChannelsUsed",
    "SetTrackConfig", "GetAudioPathConfig", "Compose", "Download", "Unload"};
_Static_assert(sizeof seg_methods / sizeof *seg_methods == 31, "IDirectMusicSegment8 has 31 methods");

/* ------------------------------------------------------------------ IDirectMusicSegmentState8 */
METHOD(st_GetRepeats, 2) { if (ARG(1)) W32(ARG(1), 0); return S_OK; }
METHOD(st_GetSegment, 2)
{
    Host *st = HOST(ARG(0));
    if (ARG(1)) W32(ARG(1), st->seg_obj);
    if (st->seg_obj) HOST(st->seg_obj)->refs++;
    return st->seg_obj ? S_OK : DMUS_E_NOT_FOUND;
}
METHOD(st_GetStartTime, 2) { if (ARG(1)) W32(ARG(1), 0); return S_OK; }
METHOD(st_GetSeek, 2) { if (ARG(1)) W32(ARG(1), 0); return S_OK; }
METHOD(st_GetStartPoint, 2) { if (ARG(1)) W32(ARG(1), 0); return S_OK; }
NOP(st_SetTrackConfig, 6)
METHOD(st_GetObjectInPath, 8) { if (ARG(7)) W32(ARG(7), 0); return DMUS_E_NOT_FOUND; }

static void (*const st_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release, st_GetRepeats,
    st_GetSegment, st_GetStartTime, st_GetSeek, st_GetStartPoint, st_SetTrackConfig, st_GetObjectInPath};
static const char *const st_names[] = {"QueryInterface", "AddRef", "Release", "GetRepeats",
    "GetSegment", "GetStartTime", "GetSeek", "GetStartPoint", "SetTrackConfig", "GetObjectInPath"};

/* ------------------------------------------------------------------ IDirectMusicAudioPath8 */
/* GetObjectInPath(pchannel, stage, buffer, guid, index, iid, &obj): a 3D path's 3D buffer */
METHOD(ap_GetObjectInPath, 8)
{
    Host *p = HOST(ARG(0));
    uint32_t pp = ARG(7);
    if (pp) W32(pp, 0);
    if (!p->is3d || !is(ARG(6), &IID_IDirectSound3DBuffer)) {
        LOG(LOG_ALWAYS, "DirectMusic GetObjectInPath(stage %#x, {" GUID_FMT "}): not provided", ARG(2), GUID_ARGS(ARG(6)));
        return DMUS_E_NOT_FOUND;
    }
    if (!p->buf3d) {
        Host *b = host_new(K_BUF3D);
        b->obj_main = com_new(&buf3d_cls, b);
        b->path = p;
        p->buf3d = b->obj_main;
    } else
        HOST(p->buf3d)->refs++;
    if (pp) W32(pp, p->buf3d);
    return S_OK;
}
METHOD(ap_Activate, 2) { HOST(ARG(0))->active = ARG(1) != 0; return S_OK; }
/* SetVolume(hundredths of a dB, duration) */
METHOD(ap_SetVolume, 3) { HOST(ARG(0))->vol_mb = (int32_t)ARG(1); return S_OK; }
METHOD(ap_ConvertPChannel, 3) { if (ARG(2)) W32(ARG(2), ARG(1)); return S_OK; }
static void (*const path_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release,
    ap_GetObjectInPath, ap_Activate, ap_SetVolume, ap_ConvertPChannel};
static const char *const path_names[] = {"QueryInterface", "AddRef", "Release", "GetObjectInPath",
    "Activate", "SetVolume", "ConvertPChannel"};

/* ------------------------------------------------------------------ IDirectSound3DBuffer
   The game sets only SetAllParameters (DS3DBUFFER: dwSize, vPosition +4, vVelocity +16, cone
   angles +28/+32, orientation +36, outside volume +48, flMinDistance +52, flMaxDistance +56,
   dwMode +60) [verified: inventory 2.5]; mode 1 (head relative) as the game uses. */
static float rf(uint32_t a) { float f; uint32_t v = R32(a); memcpy(&f, &v, 4); return f; }
METHOD(b3_SetAllParameters, 3)
{
    Host *p = HOST(ARG(0))->path;
    uint32_t d = ARG(1);
    if (!d) return E_POINTER;
    pthread_mutex_lock(&mlock);
    p->pos[0] = rf(d + 4); p->pos[1] = rf(d + 8); p->pos[2] = rf(d + 12);
    p->mind = rf(d + 52); p->maxd = rf(d + 56); p->mode = R32(d + 60);
    pthread_mutex_unlock(&mlock);
    return S_OK;
}
METHOD(b3_GetAllParameters, 2) { return E_NOTIMPL; }
NI(b3_GetConeAngles, 3) NI(b3_GetConeOrientation, 2) NI(b3_GetConeOutsideVolume, 2)
NI(b3_GetMaxDistance, 2) NI(b3_GetMinDistance, 2) NI(b3_GetMode, 2) NI(b3_GetPosition, 2)
NI(b3_GetVelocity, 2)
NOP(b3_SetConeAngles, 4) NOP(b3_SetConeOrientation, 5) NOP(b3_SetConeOutsideVolume, 3)
METHOD(b3_SetMaxDistance, 3) { Host *p = HOST(ARG(0))->path; memcpy(&p->maxd, &(uint32_t){ARG(1)}, 4); return S_OK; }
METHOD(b3_SetMinDistance, 3) { Host *p = HOST(ARG(0))->path; memcpy(&p->mind, &(uint32_t){ARG(1)}, 4); return S_OK; }
METHOD(b3_SetMode, 3) { HOST(ARG(0))->path->mode = ARG(1); return S_OK; }
METHOD(b3_SetPosition, 5)
{
    Host *p = HOST(ARG(0))->path;
    memcpy(&p->pos[0], &(uint32_t){ARG(1)}, 4);
    memcpy(&p->pos[1], &(uint32_t){ARG(2)}, 4);
    memcpy(&p->pos[2], &(uint32_t){ARG(3)}, 4);
    return S_OK;
}
NOP(b3_SetVelocity, 5)
static void (*const buf3d_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release,
    b3_GetAllParameters, b3_GetConeAngles, b3_GetConeOrientation, b3_GetConeOutsideVolume,
    b3_GetMaxDistance, b3_GetMinDistance, b3_GetMode, b3_GetPosition, b3_GetVelocity,
    b3_SetAllParameters, b3_SetConeAngles, b3_SetConeOrientation, b3_SetConeOutsideVolume,
    b3_SetMaxDistance, b3_SetMinDistance, b3_SetMode, b3_SetPosition, b3_SetVelocity};
static const char *const buf3d_names[] = {"QueryInterface", "AddRef", "Release",
    "GetAllParameters", "GetConeAngles", "GetConeOrientation", "GetConeOutsideVolume",
    "GetMaxDistance", "GetMinDistance", "GetMode", "GetPosition", "GetVelocity",
    "SetAllParameters", "SetConeAngles", "SetConeOrientation", "SetConeOutsideVolume",
    "SetMaxDistance", "SetMinDistance", "SetMode", "SetPosition", "SetVelocity"};
_Static_assert(sizeof buf3d_methods / sizeof *buf3d_methods == 21, "IDirectSound3DBuffer has 21 methods");

/* ------------------------------------------------------------------ other loaded objects
   Styles, bands, collections, chord maps: only loaded through the game's loader (the engine
   reads them itself when a segment refers to them); their own interfaces are never called
   [verified: inventory 2.8] */
static void (*const generic_methods[])(CPU *) = {m_QueryInterface, m_AddRef, m_Release};
static const char *const generic_names[] = {"QueryInterface", "AddRef", "Release"};

static void classes(void)
{
    static int done;
    if (done++)
        return;
#define CLS(cls, iface, m, n) com_class_init(&cls, iface, (int)(sizeof m / sizeof *m), m, n)
    CLS(dm_cls, "IDirectMusic8", dm_methods, dm_names);
    CLS(perf_cls, "IDirectMusicPerformance8", perf_methods, perf_names);
    CLS(seg_cls, "IDirectMusicSegment8", seg_methods, seg_names);
    CLS(state_cls, "IDirectMusicSegmentState8", st_methods, st_names);
    CLS(path_cls, "IDirectMusicAudioPath8", path_methods, path_names);
    CLS(buf3d_cls, "IDirectSound3DBuffer(DirectMusic)", buf3d_methods, buf3d_names);
    CLS(generic_cls, "IDirectMusic(loaded object)", generic_methods, generic_names);
    CLS(object_cls, "IDirectMusicObject", object_methods, object_names);
    CLS(persist_cls, "IPersistStream", persist_methods, persist_names);
#undef CLS
}

static uint32_t new_loaded(int kind, const G *clsid, ComClass *main)
{
    Host *h = host_new(kind);
    h->clsid = *clsid;
    h->obj_main = com_new(main, h);
    h->obj_object = com_new(&object_cls, h);
    h->obj_persist = com_new(&persist_cls, h);
    return h->obj_main;
}

/* CoCreateInstance: the classes above; REGDB_E_CLASSNOTREG otherwise */
uint32_t dmusic_cocreate(uint32_t clsid, uint32_t iid, uint32_t ppv)
{
    struct { const G *g; int kind; } loaded[] = {
        {&CLSID_DirectMusicSegment, K_SEG}, {&CLSID_DirectMusicStyle, K_STYLE},
        {&CLSID_DirectMusicBand, K_BAND}, {&CLSID_DirectMusicCollection, K_COLL},
        {&CLSID_DirectMusicChordMap, K_CHORDMAP},
    };
    uint32_t o = 0;
    if (is(clsid, &CLSID_DirectMusicPerformance)) {
        classes();
        if (!the_perf) {
            Host *h = host_new(K_PERF);
            h->obj_main = com_new(&perf_cls, h);
            the_perf = h->obj_main;
        } else
            HOST(the_perf)->refs++;
        o = the_perf;
    }
    for (int i = 0; !o && i < (int)(sizeof loaded / sizeof *loaded); i++)
        if (is(clsid, loaded[i].g)) {
            classes();
            o = new_loaded(loaded[i].kind, loaded[i].g, loaded[i].kind == K_SEG ? &seg_cls : &generic_cls);
            Host *h = HOST(o);
            if (is(iid, &IID_IDirectMusicObject)) o = h->obj_object;
            else if (is(iid, &IID_IPersistStream)) o = h->obj_persist;
        }
    if (!o)
        return 0x80040154u;
    LOGS("DirectMusic CoCreateInstance({" GUID_FMT "}) -> %08x", GUID_ARGS(clsid), o);
    if (ppv) W32(ppv, o);
    return S_OK;
}
