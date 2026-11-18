/* Files: Windows paths -> host paths, file handles, directory searches.

   The program sees its folder as PORT_VROOT (starting in PORT_START_DIR inside it). Reads look in the user folder first and then in
   the game folder (which is never written); anything opened for writing lives in the user
   folder, copied there first if it only exists in the game folder. Lookups ignore case. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include "options.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"

#define VROOT PORT_VROOT
/* the program's current directory (Windows form): its start folder (port.h) */
static char cwd[512] = VROOT;
static pthread_mutex_t flock_ = PTHREAD_MUTEX_INITIALIZER;

const char *virtual_game_root(void) { return VROOT; }
static char cd_drive_letter_(void);

/* Normalise to a path relative to the virtual root, '/' separated, no "." or "..";
   returns 0 if outside the root (then *rel holds a path under the user folder's drive_c). */
static int to_relative(const char *win, char *rel, size_t n, int *outside)
{
    char full[1024];
    *outside = 0;
    if (win[0] && win[1] == ':')
        snprintf(full, sizeof full, "%s", win);
    else if (win[0] == '\\' || win[0] == '/')
        snprintf(full, sizeof full, "C:%s", win);
    else
        snprintf(full, sizeof full, "%s\\%s", cwd, win);
    for (char *p = full; *p; p++)
        if (*p == '/') *p = '\\';
    /* split into components and resolve . and .. */
    char *comp[128];
    int nc = 0;
    char *save = NULL;
    char *drive = strtok_r(full, "\\", &save);
    for (char *t = strtok_r(NULL, "\\", &save); t; t = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (nc) nc--; continue; }
        if (nc < 128) comp[nc++] = t;
    }
    int start = 0;
    if (drive && !strcasecmp(drive, "C:") && nc >= 1 && !strcasecmp(comp[0], VROOT + 3))
        start = 1;
#ifdef PORT_CD_LABEL
    else if (drive && !strcasecmp(drive, PORT_CD_DRIVE))
        *outside = 2;                   /* the CD: relative to cd_dir */
#endif
    else
        *outside = 1;
    size_t k = 0;
    rel[0] = 0;
    if (*outside == 1) {
        k = snprintf(rel, n, "drive_%c", drive && drive[0] ? (char)tolower((unsigned char)drive[0]) : 'c');
    }
    for (int i = start; i < nc && k < n; i++)
        k += snprintf(rel + k, n - k, "%s%s", k ? "/" : "", comp[i]);
    return 1;
}

/* Find `rel` under `base` ignoring case; writes the real host path. Returns 1 if it exists.
   If only the parent exists, writes the path with the last component as given. */
static int resolve_ci(const char *base, const char *rel, char *out, size_t n)
{
#ifdef _WIN32
    /* Windows' folders already ignore case: a path that is not there is not there in any case,
       so one look at the whole path does (not a look at each part and a folder listing; the
       game opens thousands of files at start-up with fan content) */
    if (!*rel) {
        snprintf(out, n, "%s", base);
        return 1;
    }
    snprintf(out, n, "%s/%s", base, rel);
    struct stat st;
    return stat(out, &st) == 0;
#else
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", base);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", rel);
    char *save = NULL;
    int exists = 1;
    for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        char next[2048];
        snprintf(next, sizeof next, "%s/%s", cur, t);
        struct stat st;
        if (exists && stat(next, &st) != 0) {
            DIR *d = opendir(cur);
            int found = 0;
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (!strcasecmp(e->d_name, t)) {
                        snprintf(next, sizeof next, "%s/%s", cur, e->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found)
                exists = 0;
        }
        snprintf(cur, sizeof cur, "%s", next);
    }
    snprintf(out, n, "%s", cur);
    return exists;
#endif
}

static void mkdirs_for(const char *path)
{
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

static int copy_file(const char *from, const char *to)
{
    int a = open(from, O_RDONLY);
    if (a < 0) return 0;
    mkdirs_for(to);
    int b = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (b < 0) { close(a); return 0; }
    char buf[65536];
    ssize_t r;
    while ((r = read(a, buf, sizeof buf)) > 0)
        if (write(b, buf, r) != r) break;
    close(a);
    close(b);
    return 1;
}

/* XBOX DATA (option xbox_data = on, the default): files the Xbox version has better, prepared
   by tools/xbox_*.py in the user folder's xbox_data/ at the game's own paths, are read in place of
   the game's (writes still go to the user folder). */
static int xbox_data_file(const char *rel, char *out, size_t n)
{
    static int on = -1;
    static char dir[1100];
    if (on < 0) {
        on = strcasecmp(option_str("xbox_data", "off"), "off") != 0;   /* (LA Rush's; unused here) */
        snprintf(dir, sizeof dir, "%s/xbox_data", user_dir);
    }
    return on && resolve_ci(dir, rel, out, n);
}

/* MODS (option mods = on, the default): each folder in the user folder's mods/ holds files at the
   game's own paths, loose or whole .dir/.res packs, read in place of the game's and the Xbox data's
   (writes still go to the user folder). Taken in name order: the first holding a file wins. */
#define MAXMODS 32
static char mod_dirs[MAXMODS][1024];
static int nmods = -1;

static int cmp_str(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void mods_scan(void)
{
    if (nmods >= 0) return;
    nmods = 0;
#if defined(PORT_NO_BUILTIN_MODS)
    return;                             /* (the port has its own mods: 1NSANE's fan cars and maps) */
#endif
    if (!option_bool("mods", 1)) return;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/mods", user_dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (nmods == MAXMODS) {         /* (said, not dropped quietly: pcrecomp 08023da's lesson) */
            LOG(LOG_ALWAYS, "mods: more than %d folders in %s: \"%s\" and the rest left out", MAXMODS, dir, e->d_name);
            break;
        }
        char p[1100 + 256];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISDIR(st.st_mode) || strlen(p) >= sizeof mod_dirs[0]) continue;
        snprintf(mod_dirs[nmods++], sizeof mod_dirs[0], "%s", p);
    }
    closedir(d);
    qsort(mod_dirs, (size_t)nmods, sizeof mod_dirs[0], cmp_str);
    for (int i = 0; i < nmods; i++) LOG(LOG_ALWAYS, "mod: %s", mod_dirs[i]);
}

static int mod_file(const char *rel, char *out, size_t n)
{
    mods_scan();
    for (int i = 0; i < nmods; i++)
        if (resolve_ci(mod_dirs[i], rel, out, n)) return 1;
    return 0;
}

/* A port's own files where the game folder has none (1NSANE's fan cars and maps from its mods
   list): rel is the game-relative path, '/' separated, as the program spelled it; 1 with the host
   path in out. port_find_extra adds names to a folder's listing (FindFirstFile). */
__attribute__((weak)) int port_file_extra(const char *rel, char *out, size_t n)
{
    (void)rel; (void)out; (void)n;
    return 0;
}
__attribute__((weak)) int port_find_extra(const char *rel, char (*names)[64], int max)
{
    (void)rel; (void)names; (void)max;
    return 0;
}

/* A port's own place for a game file it reads, before every other (L.A. Rush's overlays): rel as for
   port_file_extra; 1 with the host path in out. Writes still go to the user folder. */
__attribute__((weak)) int port_file_redirect(const char *rel, char *out, size_t n)
{
    (void)rel; (void)out; (void)n;
    return 0;
}

int path_to_host(const char *win, char *out, size_t n, int for_write)
{
    char rel[1024];
    int outside;
    to_relative(win, rel, sizeof rel, &outside);
    if (!outside && !for_write && port_file_redirect(rel, out, n))
        return 1;
    char user_path[2048], game_path[2048];
    if (outside == 2 && !for_write)
        return resolve_ci(cd_dir, rel, out, n);      /* the CD */
    if (outside == 2) {                               /* written to the CD: the user folder's drive_d */
        char r2[1100];
        snprintf(r2, sizeof r2, "drive_%c/%s", (char)tolower((unsigned char)cd_drive_letter_()), rel);
        snprintf(rel, sizeof rel, "%s", r2);
        outside = 1;
    }
    int in_user = resolve_ci(user_dir, rel, user_path, sizeof user_path);
    if (outside) {
        snprintf(out, n, "%s", user_path);
        return in_user;
    }
    int in_game = resolve_ci(game_dir, rel, game_path, sizeof game_path);
    if (for_write) {
        /* the result: whether the file was there before, as the program sees it (the user folder,
           a mod, the game folder), for CreateFile's ERROR_ALREADY_EXISTS and the others' checks.
           It was 1 for every write, so a new file looked old: LA Rush's save never wrote its new
           file's header and LOAD PROFILE deleted it (larush-native 7a87933, 2026-11-06). */
        char seen[2048];
        int existed = in_user || in_game || mod_file(rel, seen, sizeof seen) || xbox_data_file(rel, seen, sizeof seen);
        if (!in_user && in_game && for_write == 2)
            copy_file(game_path, user_path);      /* opened for update: keep its contents */
        mkdirs_for(user_path);
        snprintf(out, n, "%s", user_path);
        return existed;
    }
    if (in_user) {
        snprintf(out, n, "%s", user_path);
        return 1;
    }
    if (mod_file(rel, out, n) || xbox_data_file(rel, out, n))
        return 1;
    if (!in_game && port_file_extra(rel, out, n))
        return 1;
    snprintf(out, n, "%s", game_path);
    return in_game;
}

/* ------------------------------------------------------------------ handles */
typedef struct { int fd; char path[1024]; int is_std; } File;
static uint32_t std_handles[3];

static File *file_of(uint32_t h)
{
    return handle_get(h, H_FILE);
}

void files_init(void)
{
    if (*PORT_START_DIR)
        snprintf(cwd, sizeof cwd, "%s\\%s", VROOT, PORT_START_DIR);
    for (int i = 0; i < 3; i++) {
        File *f = calloc(1, sizeof *f);
        f->fd = i;
        f->is_std = 1;
        snprintf(f->path, sizeof f->path, "<std%d>", i);
        std_handles[i] = handle_new(H_FILE, f);
    }
}

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u

/* Files the port hides from the program (port_file_hidden, e.g. intro movies an option skips):
   CreateFile doesn't find them. */
__attribute__((weak)) int port_file_hidden(const char *winpath)
{
    (void)winpath;
    return 0;
}

static uint32_t create_file(const char *name, uint32_t access, uint32_t disp, const char *fn)
{
    if (name && port_file_hidden(name)) {
        LOG(LOG_FILE, "%s(\"%s\"): hidden by the port", fn, name);
        set_last_error(2);
        return 0xffffffffu;
    }
    int writing = (access & GENERIC_WRITE) != 0;
    char host[2048];
    if (disp < 1 || disp > 5) {
        LOG(LOG_FILE, "%s(\"%s\", %x, disp %u) -> failed (no such disposition)", fn, name, access, disp);
        set_last_error(87);                     /* ERROR_INVALID_PARAMETER */
        return 0xffffffffu;
    }
    int keep = disp == 3 /* OPEN_EXISTING */ || disp == 4 /* OPEN_ALWAYS */;
    /* CREATE_NEW / CREATE_ALWAYS / TRUNCATE_EXISTING make or empty the file whatever the access, and
       OPEN_ALWAYS makes one that is nowhere: in the user folder, never the game folder's */
    int makes = writing || disp == 1 || disp == 2 || disp == 5;
    int exists = path_to_host(name, host, sizeof host, makes ? (keep ? 2 : 1) : 0);
    if (!makes && disp == 4 && !exists)
        path_to_host(name, host, sizeof host, 1);
    /* Windows' answers by whether the file was there before (CreateFile's documentation; from
       larush-native): CREATE_NEW fails with ERROR_FILE_EXISTS if it was; OPEN_EXISTING and
       TRUNCATE_EXISTING fail with ERROR_FILE_NOT_FOUND if it wasn't; CREATE_ALWAYS and OPEN_ALWAYS
       say ERROR_ALREADY_EXISTS if it was, 0 if it is new */
    if (disp == 1 && exists) {
        LOG(LOG_FILE, "%s(\"%s\", CREATE_NEW) -> failed (it exists)", fn, name);
        set_last_error(80);
        return 0xffffffffu;
    }
    if ((disp == 3 || disp == 5) && !exists) {
        LOG(LOG_FILE, "%s(\"%s\", disp %u) -> failed (no such file)", fn, name, disp);
        set_last_error(2);
        return 0xffffffffu;
    }
    int fl = writing ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;      /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;     /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;               /* OPEN_ALWAYS */
    case 5: fl |= O_CREAT | O_TRUNC; break;     /* TRUNCATE_EXISTING (there: perhaps only in the game folder) */
    }
    int fd = open(host, fl | O_CLOEXEC, 0644);
    if (fd < 0) {
        int e = errno;
        if (e == EMFILE || e == ENFILE)
            LOG(LOG_ALWAYS, "%s(\"%s\") -> failed: too many files open (the host's open-file limit)", fn, name);
        LOG(LOG_FILE, "%s(\"%s\", %x, disp %u) -> failed (%s)", fn, name, access, disp, host);
        set_last_error(e == ENOENT ? 2 : e == EEXIST ? 80 : e == EMFILE || e == ENFILE ? 4 : 5);
        return 0xffffffffu;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        set_last_error(5);
        return 0xffffffffu;
    }
    File *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    uint32_t h = handle_new(H_FILE, f);
    LOG(LOG_FILE, "%s(\"%s\", %s%s, disp %u) -> %x (%s)", fn, name,
        access & GENERIC_READ ? "R" : "", writing ? "W" : "", disp, h, host);
    set_last_error((disp == 2 || disp == 4) && exists ? 183 : 0);
    return h;
}

API(KERNEL32, CreateFileA, 7) { return create_file(gstr(ARG(0)), ARG(1), ARG(4), "CreateFileA"); }
API(KERNEL32, CreateFileW, 7) { char b[1024]; return create_file(gwstr(ARG(0), b, sizeof b), ARG(1), ARG(4), "CreateFileW"); }

/* OpenFile (16-bit style): OF_EXIST asks only whether the file is there; the OFSTRUCT gets
   the full path. Returns the handle, 1 for OF_EXIST, or HFILE_ERROR. */
static void full_path(const char *p, char *out, size_t n);
API(KERNEL32, OpenFile, 3)
{
    const char *name = gstr(ARG(0));
    uint32_t of = ARG(1), style = ARG(2);
    char full[1024], host[2048];
    full_path(name, full, sizeof full);
    memset(gptr(of), 0, 136);
    W8(of, 136);
    W8(of + 1, 1);                                      /* fFixedDisk */
    gstrncpy(of + 8, full, 128);
    if (style & 0x4000) {                               /* OF_EXIST */
        int ok = path_to_host(name, host, sizeof host, 0) && access(host, F_OK) == 0;
        LOG(LOG_FILE, "OpenFile(\"%s\", OF_EXIST) -> %d", name, ok);
        if (!ok) { W16(of + 2, 2); set_last_error(2); return 0xffffffffu; }
        return 1;
    }
    if (style & 0x200) {                                /* OF_DELETE */
        LOG(LOG_FILE, "OpenFile(\"%s\", OF_DELETE) refused", name);
        W16(of + 2, 5);
        set_last_error(5);
        return 0xffffffffu;
    }
    uint32_t access_ = (style & 3) == 1 ? GENERIC_WRITE : (style & 3) == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    uint32_t h = create_file(name, access_, (style & 0x1000) ? 2 : 3, "OpenFile");   /* OF_CREATE */
    if (h == 0xffffffffu) W16(of + 2, (uint16_t)get_last_error());
    return h;
}

/* no devices to talk to */
API(KERNEL32, DeviceIoControl, 8)
{
    LOG(LOG_FILE, "DeviceIoControl(%x, code %x) -> not supported", ARG(0), ARG(1));
    set_last_error(1);                                  /* ERROR_INVALID_FUNCTION */
    return 0;
}

/* An OVERLAPPED read or write completes at once: its result goes into the OVERLAPPED record
   (Internal = STATUS_SUCCESS, InternalHigh = bytes) and its event is set, which is what Windows
   does when a request completes synchronously. The offset comes from the record. */
extern int event_set(uint32_t h);

static void overlapped_done(uint32_t ov, uint32_t n, uint32_t status)
{
    W32(ov, status);
    W32(ov + 4, n);
    if (R32(ov + 16))
        event_set(R32(ov + 16));
}

API(KERNEL32, ReadFile, 5)
{
    File *f = file_of(ARG(0));
    uint32_t ov = ARG(4);
    if (ARG(3)) W32(ARG(3), 0);
    if (!f) { set_last_error(6); return 0; }
    uint32_t want = ARG(2), got = 0;
    if (ov) {
        off_t at = (off_t)(R32(ov + 8) | ((uint64_t)R32(ov + 12) << 32));
        while (got < want) {
            ssize_t r = pread(f->fd, gptr(ARG(1) + got), want - got, at + got);
            if (r <= 0) break;
            got += r;
        }
        overlapped_done(ov, got, got || !want ? 0 : 0xc0000011u);        /* STATUS_END_OF_FILE */
        if (ARG(3)) W32(ARG(3), got);
        LOG(LOG_FILE, "ReadFile(%x, %u at %lld, overlapped) -> %u", ARG(0), want, (long long)at, got);
        if (!got && want) { set_last_error(38); return 0; }               /* ERROR_HANDLE_EOF */
        return 1;
    }
    while (got < want) {
        ssize_t r = read(f->fd, gptr(ARG(1) + got), want - got);
        if (r <= 0) break;
        got += r;
    }
    if (ARG(3)) W32(ARG(3), got);
    LOG(LOG_FILE, "ReadFile(%x, %u) -> %u", ARG(0), want, got);
    return 1;
}

API(KERNEL32, GetOverlappedResult, 4)
{
    uint32_t ov = ARG(1);
    if (ARG(2)) W32(ARG(2), R32(ov + 4));
    if (R32(ov) == 0x103u) { set_last_error(996); return 0; }            /* still pending: never */
    if (R32(ov)) { set_last_error(R32(ov) == 0xc0000011u ? 38 : 31); return 0; }
    return 1;
}

API(KERNEL32, WriteFile, 5)
{
    File *f = file_of(ARG(0));
    if (ARG(3)) W32(ARG(3), 0);
    if (!f) { set_last_error(6); return 0; }
    uint32_t n = ARG(2);
    if (f->is_std) {
        LOG(LOG_ALWAYS, "program wrote to std%d: %.*s", f->fd, (int)n, gptr(ARG(1)));
        if (ARG(3)) W32(ARG(3), n);
        return 1;
    }
    ssize_t r;
    if (ARG(4)) {
        uint32_t ov = ARG(4);
        r = pwrite(f->fd, gptr(ARG(1)), n, (off_t)(R32(ov + 8) | ((uint64_t)R32(ov + 12) << 32)));
        overlapped_done(ov, r > 0 ? (uint32_t)r : 0, r < 0 ? 0xc0000001u : 0);
    } else
        r = write(f->fd, gptr(ARG(1)), n);
    if (r < 0) { set_last_error(5); return 0; }
    if (ARG(3)) W32(ARG(3), (uint32_t)r);
    LOG(LOG_FILE, "WriteFile(%x, %u) -> %zd", ARG(0), n, r);
    return 1;
}

API(KERNEL32, SetFilePointer, 4)
{
    File *f = file_of(ARG(0));
    if (!f) { set_last_error(6); return 0xffffffffu; }
    int64_t dist = (int32_t)ARG(1);
    if (ARG(2))
        dist = (int64_t)(((uint64_t)R32(ARG(2)) << 32) | ARG(1));
    int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r = lseek(f->fd, dist, whence);
    if (r < 0) { set_last_error(131); return 0xffffffffu; }
    if (ARG(2)) W32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    set_last_error(0);
    return (uint32_t)r;
}

API(KERNEL32, SetEndOfFile, 1)
{
    File *f = file_of(ARG(0));
    if (!f) return 0;
    off_t p = lseek(f->fd, 0, SEEK_CUR);
    return ftruncate(f->fd, p) == 0;
}

API(KERNEL32, FlushFileBuffers, 1)
{
    return file_of(ARG(0)) != NULL;
}

API(KERNEL32, GetFileType, 1)
{
    File *f = file_of(ARG(0));
    if (!f) { set_last_error(6); return 0; }
    return f->is_std ? 2 : 1;                   /* FILE_TYPE_CHAR / FILE_TYPE_DISK */
}

API(KERNEL32, GetStdHandle, 1)
{
    int32_t n = (int32_t)ARG(0);
    if (n >= -12 && n <= -10)
        return std_handles[-10 - n];
    return 0xffffffffu;
}

API(KERNEL32, SetStdHandle, 2)
{
    int32_t n = (int32_t)ARG(0);
    if (n >= -12 && n <= -10)
        std_handles[-10 - n] = ARG(1);
    return 1;
}

API(KERNEL32, SetHandleCount, 1) { return ARG(0); }

int close_file_handle(uint32_t h)
{
    File *f = file_of(h);
    if (!f) return 0;
    if (!f->is_std)
        close(f->fd);
    free(f);
    handle_close(h);
    return 1;
}

static int delete_file(const char *name)
{
    char host[2048];
    path_to_host(name, host, sizeof host, 0);
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = 0;
    if (!strncmp(host, user, strlen(user)))
        ok = unlink(host) == 0;
    LOG(LOG_FILE, "DeleteFile(\"%s\") -> %d", name, ok);
    if (!ok) set_last_error(2);
    return ok;
}
API(KERNEL32, DeleteFileA, 1) { return delete_file(gstr(ARG(0))); }
API(KERNEL32, DeleteFileW, 1) { char b[1024]; return delete_file(gwstr(ARG(0), b, sizeof b)); }

static int move_file(const char *src, const char *dst)
{
    char from[2048], to[2048];
    int exists = path_to_host(src, from, sizeof from, 0);
    if (exists && path_to_host(dst, to, sizeof to, 1)) {
        LOG(LOG_FILE, "MoveFile(\"%s\", \"%s\") -> 0 (the target exists)", src, dst);
        set_last_error(183);                    /* (Windows: ERROR_ALREADY_EXISTS, the target kept) */
        return 0;
    }
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = 0;
    if (exists) {
        if (!strncmp(from, user, strlen(user)))
            ok = rename(from, to) == 0;
        else
            ok = copy_file(from, to);           /* never remove from the game folder */
    }
    LOG(LOG_FILE, "MoveFile(\"%s\", \"%s\") -> %d", src, dst, ok);
    if (!ok) set_last_error(2);
    return ok;
}
API(KERNEL32, MoveFileA, 2) { return move_file(gstr(ARG(0)), gstr(ARG(1))); }
API(KERNEL32, MoveFileW, 2)
{
    char a[1024], b[1024];
    return move_file(gwstr(ARG(0), a, sizeof a), gwstr(ARG(1), b, sizeof b));
}

static int set_cwd(const char *p)
{
    char host[2048];
    int exists = path_to_host(p, host, sizeof host, 0);
    LOG(LOG_FILE, "SetCurrentDirectory(\"%s\") -> %d", p, exists);
    if (!exists) { set_last_error(3); return 0; }
    pthread_mutex_lock(&flock_);
    if (p[0] && p[1] == ':')
        snprintf(cwd, sizeof cwd, "%s", p);
    else {
        char t[512];
        snprintf(t, sizeof t, "%s\\%s", cwd, p);
        snprintf(cwd, sizeof cwd, "%s", t);
    }
    size_t k = strlen(cwd);
    while (k > 3 && (cwd[k - 1] == '\\' || cwd[k - 1] == '/'))
        cwd[--k] = 0;
    pthread_mutex_unlock(&flock_);
    return 1;
}
API(KERNEL32, SetCurrentDirectoryA, 1) { return set_cwd(gstr(ARG(0))); }
API(KERNEL32, SetCurrentDirectoryW, 1) { char b[1024]; return set_cwd(gwstr(ARG(0), b, sizeof b)); }

API(KERNEL32, GetTempFileNameA, 4)
{
    static unsigned counter = 1;
    const char *dir = gstr(ARG(0)), *pre = gstr(ARG(1));
    unsigned u = ARG(2) & 0xffff;
    char name[600];
    snprintf(name, sizeof name, "%s\\%.3s%X.TMP", dir, pre ? pre : "", u);
    if (!ARG(2)) {
        /* Windows takes the next number whose file is nowhere and makes it (an empty file) */
        char host[2048];
        int tries = 0;
        do {
            u = counter++ & 0xffff;
            if (!u)
                u = counter++ & 0xffff;
            snprintf(name, sizeof name, "%s\\%.3s%X.TMP", dir, pre ? pre : "", u);
        } while (path_to_host(name, host, sizeof host, 1) && ++tries < 0xffff);
        int fd = open(host, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) close(fd);
    }
    gstrncpy(ARG(3), name, 260);
    LOG(LOG_FILE, "GetTempFileNameA -> \"%s\"", name);
    return u;
}

/* ------------------------------------------------------------------ directory search */
typedef struct {
    char names[4096][64];
    char from[4096];                    /* where each name was listed: FROM_USER, FROM_GAME, FROM_XBOX, FROM_MOD + i */
    int n, pos;
    char dir_user[2048], dir_game[2048], dir_xbox[2048], dir_mod[MAXMODS][1024];
    char rel[1024];                     /* (the folder, for the port's own names: FROM_EXTRA) */
} Find;

static void unix_time_to_filetime(time_t t, uint32_t at)
{
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    W32(at, (uint32_t)ft);
    W32(at + 4, (uint32_t)(ft >> 32));
}

enum { FROM_USER, FROM_GAME, FROM_XBOX, FROM_MOD, FROM_EXTRA = 100 };

/* WIN32_FIND_DATAA (320 bytes) or, wide, WIN32_FIND_DATAW (592: the names in UTF-16) */
static int fill_find_data(Find *f, uint32_t data, int wide)
{
    while (f->pos < f->n) {
        int from = f->from[f->pos];
        const char *nm = f->names[f->pos++];
        char p[2200];
        struct stat st;
        /* looked for where it was listed only (a name in the user folder hides the game's) */
        if (from == FROM_EXTRA) {
            char r[1100];
            snprintf(r, sizeof r, "%s%s%s", f->rel, *f->rel ? "/" : "", nm);
            if (!port_file_extra(r, p, sizeof p) || stat(p, &st) != 0)
                continue;
        } else {
            snprintf(p, sizeof p, "%s/%s", from == FROM_USER ? f->dir_user : from == FROM_XBOX ? f->dir_xbox :
                     from >= FROM_MOD ? f->dir_mod[from - FROM_MOD] : f->dir_game, nm);
            if (stat(p, &st) != 0)
                continue;
        }
        memset(gptr(data), 0, wide ? 592 : 320);
        W32(data, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
        unix_time_to_filetime(st.st_mtime, data + 4);
        unix_time_to_filetime(st.st_atime, data + 12);
        unix_time_to_filetime(st.st_mtime, data + 20);
        W32(data + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        W32(data + 32, (uint32_t)st.st_size);
        if (wide) gwput(data + 44, nm, 260);
        else gstrncpy(data + 44, nm, 260);
        return 1;
    }
    return 0;
}

#include "dosmask.h"               /* (Windows' masks) */

static void add_matches(Find *f, const char *dir, const char *pat, int from)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && f->n < 4096) {
        if (!dos_mask_match(pat, e->d_name))   /* (Windows' masks: dosmask.h) */
            continue;
        if (strlen(e->d_name) >= 64)
            continue;
        int dup = 0;
        for (int i = 0; i < f->n && !dup; i++)
            dup = !strcasecmp(f->names[i], e->d_name);
        if (!dup) {
            f->from[f->n] = (char)from;
            snprintf(f->names[f->n++], 64, "%s", e->d_name);
        }
    }
    closedir(d);
}

/* sorted by name, case ignored (the names are unique so), each keeping where it was listed */
static Find *sorting;
static int cmp_idx(const void *a, const void *b)
{
    return strcasecmp(sorting->names[*(const int *)a], sorting->names[*(const int *)b]);
}

static void sort_names(Find *f)
{
    static int idx[4096];
    static char names[4096][64], from[4096];
    for (int i = 0; i < f->n; i++)
        idx[i] = i;
    sorting = f;
    qsort(idx, f->n, sizeof idx[0], cmp_idx);
    for (int i = 0; i < f->n; i++) {
        memcpy(names[i], f->names[idx[i]], 64);
        from[i] = f->from[idx[i]];
    }
    memcpy(f->names, names, (size_t)f->n * 64);
    memcpy(f->from, from, (size_t)f->n);
}

static uint32_t find_first(const char *spec, uint32_t data, int wide)
{
    char dirpart[600], pat[260];
    const char *slash = strrchr(spec, '\\');
    if (!slash) slash = strrchr(spec, '/');
    if (slash) {
        snprintf(dirpart, sizeof dirpart, "%.*s", (int)(slash - spec), spec);
        snprintf(pat, sizeof pat, "%s", slash + 1);
    } else {
        snprintf(dirpart, sizeof dirpart, ".");
        snprintf(pat, sizeof pat, "%s", spec);
    }
    if (!strcmp(pat, "*.*"))
        snprintf(pat, sizeof pat, "*");
    Find *f = calloc(1, sizeof *f);
    char rel[1024];
    int outside;
    to_relative(dirpart, rel, sizeof rel, &outside);
    resolve_ci(user_dir, rel, f->dir_user, sizeof f->dir_user);
    if (outside == 2) {                                 /* the CD: its files only */
        f->dir_user[0] = 0;
        resolve_ci(cd_dir, rel, f->dir_game, sizeof f->dir_game);
    } else if (outside)
        snprintf(f->dir_game, sizeof f->dir_game, "%s", f->dir_user);
    else if (!resolve_ci(game_dir, rel, f->dir_game, sizeof f->dir_game))
        port_file_extra(rel, f->dir_game, sizeof f->dir_game);   /* (a folder only the port has: a fan car's) */
    snprintf(f->rel, sizeof f->rel, "%s", rel);
    add_matches(f, f->dir_user, pat, FROM_USER);
    if (!outside) {                                     /* (each before the game's: it hides them) */
        mods_scan();
        for (int i = 0; i < nmods; i++)
            if (resolve_ci(mod_dirs[i], rel, f->dir_mod[i], sizeof f->dir_mod[i]))
                add_matches(f, f->dir_mod[i], pat, FROM_MOD + i);
        if (xbox_data_file(rel, f->dir_xbox, sizeof f->dir_xbox))
            add_matches(f, f->dir_xbox, pat, FROM_XBOX);
    }
    add_matches(f, f->dir_game, pat, FROM_GAME);
    if (!outside) {                                     /* the port's own names, after (the first spelling wins) */
        static char extra[4096][64];
        int k = port_find_extra(rel, extra, 4096);
        for (int i = 0; i < k && f->n < 4096; i++) {
            if (!dos_mask_match(pat, extra[i]))
                continue;
            int dup = 0;
            for (int j = 0; j < f->n && !dup; j++)
                dup = !strcasecmp(f->names[j], extra[i]);
            if (!dup) {
                f->from[f->n] = FROM_EXTRA;
                snprintf(f->names[f->n++], 64, "%s", extra[i]);
            }
        }
    }
    sort_names(f);
    LOG(LOG_FILE, "FindFirstFile(\"%s\") -> %d matches", spec, f->n);
    if (!fill_find_data(f, data, wide)) {
        free(f);
        set_last_error(2);
        return 0xffffffffu;
    }
    return handle_new(H_FIND, f);
}

API(KERNEL32, FindFirstFileA, 2) { return find_first(gstr(ARG(0)), ARG(1), 0); }
API(KERNEL32, FindFirstFileW, 2) { char b[1024]; return find_first(gwstr(ARG(0), b, sizeof b), ARG(1), 1); }

static int find_next(uint32_t h, uint32_t data, int wide)
{
    Find *f = handle_get(h, H_FIND);
    if (!f || !fill_find_data(f, data, wide)) {
        set_last_error(18);                     /* ERROR_NO_MORE_FILES */
        return 0;
    }
    return 1;
}

API(KERNEL32, FindNextFileA, 2) { return find_next(ARG(0), ARG(1), 0); }
API(KERNEL32, FindNextFileW, 2) { return find_next(ARG(0), ARG(1), 1); }

API(KERNEL32, FindClose, 1)
{
    Find *f = handle_get(ARG(0), H_FIND);
    if (!f) return 0;
    free(f);
    handle_close(ARG(0));
    return 1;
}

/* ------------------------------------------------------------------ more file functions (LA Rush) */
static uint32_t attrs_of(const char *host)
{
    struct stat st;
    if (stat(host, &st) != 0)
        return 0xffffffffu;
    uint32_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;             /* DIRECTORY / ARCHIVE */
    char user[2048];
    snprintf(user, sizeof user, "%s/", user_dir);
    if (!S_ISDIR(st.st_mode) && strncmp(host, user, strlen(user)))
        a |= 0x01;                                              /* the game folder is read-only */
    return a;
}

static uint32_t get_attrs(const char *win)
{
    char host[2048];
    if (!path_to_host(win, host, sizeof host, 0)) {
        set_last_error(2);
        return 0xffffffffu;
    }
    uint32_t a = attrs_of(host);
    if (a == 0xffffffffu)
        set_last_error(2);
    else if (cd_dir[0] && strstr(host, cd_dir) == host)
        a |= 1;                                         /* the CD is read only */
    LOG(LOG_FILE, "GetFileAttributes(\"%s\") -> %x", win, a);
    return a;
}

API(KERNEL32, GetFileAttributesA, 1) { return get_attrs(gstr(ARG(0))); }
API(KERNEL32, GetFileAttributesW, 1) { char b[1024]; return get_attrs(gwstr(ARG(0), b, sizeof b)); }
API(KERNEL32, SetFileAttributesA, 2) { return get_attrs(gstr(ARG(0))) != 0xffffffffu; }
API(KERNEL32, SetFileAttributesW, 2) { char b[1024]; return get_attrs(gwstr(ARG(0), b, sizeof b)) != 0xffffffffu; }

static int make_dir(const char *win)
{
    char host[2048];
    int existed = path_to_host(win, host, sizeof host, 1);    /* (the user folder's, or a game folder's) */
    int ok = !existed && mkdir(host, 0755) == 0;
    if (!ok) set_last_error(existed || errno == EEXIST ? 183 : 3);
    LOG(LOG_FILE, "CreateDirectory(\"%s\") -> %d", win, ok);
    return ok;
}
API(KERNEL32, CreateDirectoryA, 2) { return make_dir(gstr(ARG(0))); }
API(KERNEL32, CreateDirectoryW, 2) { char b[1024]; return make_dir(gwstr(ARG(0), b, sizeof b)); }

static int remove_dir(const char *win)
{
    char host[2048], user[2048];
    path_to_host(win, host, sizeof host, 0);
    snprintf(user, sizeof user, "%s/", user_dir);
    int ok = !strncmp(host, user, strlen(user)) && rmdir(host) == 0;
    if (!ok) set_last_error(5);
    return ok;
}
API(KERNEL32, RemoveDirectoryA, 1) { return remove_dir(gstr(ARG(0))); }
API(KERNEL32, RemoveDirectoryW, 1) { char b[1024]; return remove_dir(gwstr(ARG(0), b, sizeof b)); }

/* the full path of a name against the program's current directory */
static void full_path(const char *p, char *out, size_t n)
{
    pthread_mutex_lock(&flock_);
    if (p[0] && p[1] == ':')
        snprintf(out, n, "%s", p);
    else if (p[0] == '\\' || p[0] == '/')
        snprintf(out, n, "%.2s%s", cwd, p);
    else
        snprintf(out, n, "%s\\%s", cwd, p);
    pthread_mutex_unlock(&flock_);
    for (char *q = out; *q; q++)
        if (*q == '/') *q = '\\';
}

API(KERNEL32, GetFullPathNameA, 4)
{
    char b[1024];
    full_path(gstr(ARG(0)), b, sizeof b);
    uint32_t len = strlen(b);
    if (len + 1 > ARG(1)) return len + 1;
    gstrncpy(ARG(2), b, ARG(1));
    if (ARG(3)) {
        const char *s = strrchr(b, '\\');
        W32(ARG(3), s ? ARG(2) + (uint32_t)(s + 1 - b) : 0);
    }
    return len;
}

API(KERNEL32, GetFullPathNameW, 4)
{
    char in[1024], b[1024];
    full_path(gwstr(ARG(0), in, sizeof in), b, sizeof b);
    uint32_t len = strlen(b);
    if (len + 1 > ARG(1)) return len + 1;
    gwput(ARG(2), b, ARG(1));
    if (ARG(3)) {
        const char *s = strrchr(b, '\\');
        W32(ARG(3), s ? ARG(2) + 2 * (uint32_t)(s + 1 - b) : 0);
    }
    return len;
}

API(KERNEL32, GetCurrentDirectoryA, 2)
{
    pthread_mutex_lock(&flock_);
    uint32_t len = strlen(cwd);
    if (len + 1 <= ARG(0)) gstrncpy(ARG(1), cwd, ARG(0));
    pthread_mutex_unlock(&flock_);
    return len + 1 <= ARG(0) ? len : len + 1;
}

API(KERNEL32, GetCurrentDirectoryW, 2)
{
    pthread_mutex_lock(&flock_);
    uint32_t len = strlen(cwd);
    if (len + 1 <= ARG(0)) gwput(ARG(1), cwd, ARG(0));
    pthread_mutex_unlock(&flock_);
    return len + 1 <= ARG(0) ? len : len + 1;
}

API(KERNEL32, GetFileSize, 2)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(6); return 0xffffffffu; }
    if (ARG(1)) W32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    return (uint32_t)st.st_size;
}

API(KERNEL32, GetFileTime, 4)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(6); return 0; }
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) unix_time_to_filetime(st.st_mtime, ARG(i));
    return 1;
}

API(KERNEL32, SetFileTime, 4) { return file_of(ARG(0)) != NULL; }

API(KERNEL32, GetFileInformationByHandle, 2)
{
    File *f = file_of(ARG(0));
    struct stat st;
    if (!f || fstat(f->fd, &st)) { set_last_error(6); return 0; }
    uint32_t b = ARG(1);
    memset(gptr(b), 0, 52);
    W32(b, attrs_of(f->path));
    unix_time_to_filetime(st.st_mtime, b + 4);
    unix_time_to_filetime(st.st_atime, b + 12);
    unix_time_to_filetime(st.st_mtime, b + 20);
    W32(b + 28, 0x1234);                                        /* volume serial */
    W32(b + 32, (uint32_t)((uint64_t)st.st_size >> 32));
    W32(b + 36, (uint32_t)st.st_size);
    W32(b + 40, 1);
    W32(b + 44, (uint32_t)((uint64_t)st.st_ino >> 32));
    W32(b + 48, (uint32_t)st.st_ino);
    return 1;
}

static int copy_file_to(const char *src, const char *dst, int fail_if_exists)
{
    char from[2048], to[2048];
    if (!path_to_host(src, from, sizeof from, 0)) { set_last_error(2); return 0; }
    int exists = path_to_host(dst, to, sizeof to, 1);
    struct stat st;
    (void)st;
    if (fail_if_exists && exists) { set_last_error(80); return 0; }      /* (seen anywhere the program reads) */
    int ok = copy_file(from, to);
    LOG(LOG_FILE, "CopyFile(\"%s\", \"%s\") -> %d", src, dst, ok);
    return ok;
}
API(KERNEL32, CopyFileA, 3) { return copy_file_to(gstr(ARG(0)), gstr(ARG(1)), ARG(2) != 0); }
API(KERNEL32, CopyFileW, 3)
{
    char a[1024], b[1024];
    return copy_file_to(gwstr(ARG(0), a, sizeof a), gwstr(ARG(1), b, sizeof b), ARG(2) != 0);
}

API(KERNEL32, LockFile, 5) { return 1; }
API(KERNEL32, UnlockFile, 5) { return 1; }

/* Drives: C: (the game's folder) and, with PORT_CD_LABEL, the CD */
static char cd_drive_letter_(void)
{
#ifdef PORT_CD_LABEL
    return (char)toupper((unsigned char)PORT_CD_DRIVE[0]);
#else
    return 0;
#endif
}
static uint32_t drive_mask(void) { return (1u << 2) | (cd_drive_letter_() ? 1u << (cd_drive_letter_() - 'A') : 0); }
static int drive_of(const char *r)
{
    char d = r && r[0] && r[1] == ':' ? (char)toupper((unsigned char)r[0]) : 'C';
    if (r && (r[0] == '\\' || r[0] == '/') && !r[1]) d = 'C';
    return d;
}
static uint32_t drive_type(const char *r)
{
    int d = drive_of(r);
    return d == 'C' ? 3 : d == cd_drive_letter_() ? 5 : 1;       /* DRIVE_FIXED, DRIVE_CDROM, no root */
}
API(KERNEL32, GetDriveTypeA, 1) { return drive_type(gstr(ARG(0))); }
API(KERNEL32, GetDriveTypeW, 1)
{
    char b[8];
    return drive_type(ARG(0) ? gwstr(ARG(0), b, sizeof b) : NULL);
}
API(KERNEL32, GetLogicalDrives, 0) { return drive_mask(); }
API_CDECL(MSVCRT, _getdrives) { return drive_mask(); }

API(KERNEL32, GetDiskFreeSpaceA, 5)
{
    if (ARG(1)) W32(ARG(1), 8);
    if (ARG(2)) W32(ARG(2), 512);
    if (ARG(3)) W32(ARG(3), 0x100000);
    if (ARG(4)) W32(ARG(4), 0x200000);
    return 1;
}

API(KERNEL32, GetDiskFreeSpaceExA, 4)
{
    uint64_t v = 16ull << 30;
    for (int i = 1; i <= 3; i++)
        if (ARG(i)) { W32(ARG(i), (uint32_t)v); W32(ARG(i) + 4, (uint32_t)(v >> 32)); }
    return 1;
}

API(KERNEL32, GetVolumeInformationA, 8)
{
    int d = drive_of(gstr(ARG(0)));
    int cd = d == cd_drive_letter_();
#ifdef PORT_CD_LABEL
    const char *label = cd ? PORT_CD_LABEL : d == 'C' ? PORT_VOLUME_LABEL : NULL;
#else
    const char *label = d == 'C' ? PORT_VOLUME_LABEL : NULL;
#endif
    LOG(LOG_FILE, "GetVolumeInformationA(\"%s\") -> %s", gstr(ARG(0)) ? gstr(ARG(0)) : "", label ? label : "none");
    if (!label) { set_last_error(21); return 0; }   /* ERROR_NOT_READY */
    if (ARG(1) && ARG(2)) gstrncpy(ARG(1), label, ARG(2));
    if (ARG(3)) W32(ARG(3), cd ? 0x3a1f0c99u : 0x1234abcdu);
    if (ARG(4)) W32(ARG(4), cd ? 110 : 255);
    if (ARG(5)) W32(ARG(5), cd ? 0x1 : 0x3);
    if (ARG(6) && ARG(7)) gstrncpy(ARG(6), cd ? "CDFS" : "NTFS", ARG(7));
    return 1;
}

API(KERNEL32, GetTempPathA, 2)
{
    char b[600];
    snprintf(b, sizeof b, "%s\\TEMP\\", virtual_game_root());
    uint32_t len = strlen(b);
    if (len + 1 <= ARG(0)) gstrncpy(ARG(1), b, ARG(0));
    return len;
}

API(KERNEL32, GetWindowsDirectoryA, 2) { gstrncpy(ARG(0), "C:\\WINDOWS", ARG(1)); return 10; }
API(KERNEL32, GetSystemDirectoryA, 2) { gstrncpy(ARG(0), "C:\\WINDOWS\\system32", ARG(1)); return 19; }
API(KERNEL32, GetSystemDirectoryW, 2) { gwput(ARG(0), "C:\\WINDOWS\\system32", ARG(1)); return 19; }

API(KERNEL32, GetShortPathNameA, 3)
{
    const char *s = gstr(ARG(0));
    uint32_t len = strlen(s);
    if (len + 1 <= ARG(2)) gstrncpy(ARG(1), s, ARG(2));
    return len + 1 <= ARG(2) ? len : len + 1;
}

/* names are kept as given (no 8.3 names exist) */
API(KERNEL32, GetLongPathNameW, 3)
{
    char s[1024];
    gwstr(ARG(0), s, sizeof s);
    uint32_t len = strlen(s);
    if (len + 1 <= ARG(2)) gwput(ARG(1), s, ARG(2));
    return len + 1 <= ARG(2) ? len : len + 1;
}

/* every missing folder of a path (SHCreateDirectoryEx, SHGetFolderPath's CSIDL_FLAG_CREATE):
   0, or ERROR_ALREADY_EXISTS / ERROR_PATH_NOT_FOUND */
uint32_t create_dir_tree(const char *win)
{
    char host[2048];
    struct stat st;
    if (path_to_host(win, host, sizeof host, 0) && stat(host, &st) == 0)
        return S_ISDIR(st.st_mode) ? 183 : 80;
    path_to_host(win, host, sizeof host, 1);
    mkdirs_for(host);
    int ok = mkdir(host, 0755) == 0 || errno == EEXIST;
    LOG(LOG_FILE, "create folders \"%s\" -> %d", win, ok);
    return ok ? 0 : 3;
}

API(KERNEL32, SearchPathA, 6) { set_last_error(2); return 0; }

/* ------------------------------------------------------------------ INI files */
/* The program's .ini files: read from the game or user folder, written to the user folder. */
static int ini_read(const char *file, char **text)
{
    char host[2048];
    *text = NULL;
    char name[600];
    if (!strchr(file, '\\') && !strchr(file, '/'))
        snprintf(name, sizeof name, "C:\\WINDOWS\\%s", file);
    else
        snprintf(name, sizeof name, "%s", file);
    if (!path_to_host(name, host, sizeof host, 0))
        return 0;
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *text = malloc(n + 1);
    n = fread(*text, 1, n, f);
    (*text)[n] = 0;
    fclose(f);
    return 1;
}

/* the value of key in section (NULL if absent), into out */
static int ini_get(const char *text, const char *sect, const char *key, char *out, size_t n)
{
    int in = 0;
    for (const char *l = text; l && *l; ) {
        const char *e = strpbrk(l, "\r\n");
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(len < 1023 ? len : 1023), l);
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *q = strchr(p, ']');
            if (q) *q = 0;
            in = !strcasecmp(p + 1, sect);
        } else if (in && *p && *p != ';') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                char *k = p, *v = eq + 1;
                size_t kl = strlen(k);
                while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
                while (*v == ' ' || *v == '\t') v++;
                if (!strcasecmp(k, key)) {
                    snprintf(out, n, "%s", v);
                    return 1;
                }
            }
        }
        l = e ? e + 1 : NULL;
        while (l && (*l == '\r' || *l == '\n')) l++;
    }
    return 0;
}

/* (from hardtruck2-native's inifile.c: HT2 lists its control profiles' keys, kmjin.ini) */
/* the names of a section's keys (sect set) or of all sections (sect NULL), each ended by a NUL,
   into out; returns the length used */
static size_t ini_list(const char *text, const char *sect, char *out, size_t cap)
{
    size_t n = 0;
    int in = 0;
    for (const char *l = text; l && *l; ) {
        const char *e = strpbrk(l, "\r\n");
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(len < 1023 ? len : 1023), l);
        char *p = line, *name = NULL;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char *q = strchr(p, ']');
            if (q) *q = 0;
            in = sect && !strcasecmp(p + 1, sect);
            if (!sect) name = p + 1;
        } else if (in && *p && *p != ';') {
            char *eq = strchr(p, '=');
            if (eq) {
                *eq = 0;
                size_t kl = strlen(p);
                while (kl && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) p[--kl] = 0;
                name = p;
            }
        }
        if (name) {
            size_t k = strlen(name) + 1;
            if (n + k > cap) k = cap > n ? cap - n : 0;
            memcpy(out + n, name, k);
            n += k;
        }
        l = e ? e + 1 : NULL;
        while (l && (*l == '\r' || *l == '\n')) l++;
    }
    return n;
}

API_WEAK(KERNEL32, GetPrivateProfileStringA, 6)
{
    const char *sect = gstr(ARG(0)), *key = gstr(ARG(1)), *def = gstr(ARG(2)), *file = gstr(ARG(5));
    uint32_t out = ARG(3), cap = ARG(4);
    char *text, v[1024];
    const char *r = def ? def : "";
    if ((!sect || !key) && cap) {
        /* a list: names ended by NULs, one more NUL at the end; when it does not fit, cut to
           cap - 2 with the two NULs */
        char *list = malloc(cap + 2);
        size_t n = 0;
        if (ini_read(file, &text)) {
            n = ini_list(text, sect, list, cap + 2);
            free(text);
        }
        uint32_t ret;
        if (n + 1 > cap) {
            n = cap >= 2 ? cap - 2 : 0;
            ret = n;
            memcpy(gptr(out), list, n);
            if (cap >= 2) { W8(out + n, 0); W8(out + n + 1, 0); }
            else W8(out, 0);
        } else {
            memcpy(gptr(out), list, n);
            W8(out + n, 0);
            ret = n;                    /* the length without the last NUL */
        }
        free(list);
        LOG(LOG_FILE, "GetPrivateProfileStringA([%s] list, %s) -> %u bytes", sect ? sect : "sections", file, ret);
        return ret;
    }
    if (sect && key && ini_read(file, &text)) {
        if (ini_get(text, sect, key, v, sizeof v))
            r = v;
        free(text);
    }
    LOG(LOG_FILE, "GetPrivateProfileStringA([%s] %s, %s) -> \"%s\"", sect, key, file, r);
    if (!cap) return 0;
    gstrncpy(out, r, cap);
    return strlen(gptr(out));
}

API_WEAK(KERNEL32, GetPrivateProfileIntA, 4)
{
    const char *sect = gstr(ARG(0)), *key = gstr(ARG(1)), *file = gstr(ARG(3));
    char *text, v[1024];
    uint32_t r = ARG(2);
    if (ini_read(file, &text)) {
        if (ini_get(text, sect, key, v, sizeof v))
            r = (uint32_t)strtol(v, NULL, 10);
        free(text);
    }
    LOG(LOG_FILE, "GetPrivateProfileIntA([%s] %s, %s) -> %d", sect, key, file, (int)r);
    return r;
}

API_WEAK(KERNEL32, GetProfileIntA, 3) { return ARG(2); }

API_WEAK(KERNEL32, WritePrivateProfileStringA, 4)
{
    const char *sect = gstr(ARG(0)), *key = gstr(ARG(1)), *val = gstr(ARG(2)), *file = gstr(ARG(3));
    char *text = NULL;
    ini_read(file, &text);
    /* rebuild: copy lines, replacing or adding key in section */
    size_t cap = (text ? strlen(text) : 0) + 4096, n = 0;
    char *out = malloc(cap);
    int in = 0, done = 0, seen = 0;
    out[0] = 0;
#define PUT(...) n += snprintf(out + n, cap - n, __VA_ARGS__)
    for (const char *l = text; l && *l; ) {
        const char *e = strpbrk(l, "\r\n");
        size_t len = e ? (size_t)(e - l) : strlen(l);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(len < 1023 ? len : 1023), l);
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            if (in && !done && key && val) { PUT("%s=%s\r\n", key, val); done = 1; }
            char s[256];
            snprintf(s, sizeof s, "%s", p + 1);
            char *q = strchr(s, ']');
            if (q) *q = 0;
            in = !strcasecmp(s, sect);
            seen |= in;
            if (!(in && !key)) PUT("%s\r\n", line);
        } else if (in) {
            const char *eq = strchr(p, '=');
            if (!key)
                ;                                               /* the section is deleted */
            else if (eq && (size_t)(eq - p) == strlen(key) && !strncasecmp(p, key, eq - p)) {
                if (val && !done) PUT("%s=%s\r\n", key, val);
                done = 1;
            } else
                PUT("%s\r\n", line);
        } else if (*p)
            PUT("%s\r\n", line);
        l = e ? e + 1 : NULL;
        while (l && (*l == '\r' || *l == '\n')) l++;
    }
    if (key && val && !done) {
        if (!in) PUT("[%s]\r\n", sect);
        PUT("%s=%s\r\n", key, val);
    }
#undef PUT
    (void)seen;
    char name[600], host[2048];
    if (!strchr(file, '\\') && !strchr(file, '/'))
        snprintf(name, sizeof name, "C:\\WINDOWS\\%s", file);
    else
        snprintf(name, sizeof name, "%s", file);
    path_to_host(name, host, sizeof host, 1);
    FILE *f = fopen(host, "wb");
    if (f) { fwrite(out, 1, n, f); fclose(f); }
    LOG(LOG_FILE, "WritePrivateProfileStringA([%s] %s=%s, %s)", sect, key ? key : "-", val ? val : "-", file);
    free(out);
    free(text);
    return f != NULL;
}

/* ------------------------------------------------------------------ gathered from the other ports (AntiStatic) */
/* A file mapping (from offroad-native): a view is a copy of the file read into program memory;
   writes to it are not written back; a mapping of no file (INVALID_HANDLE_VALUE) is zeroed
   memory. */
typedef struct { int fd; uint32_t size; } Mapping;

/* (from etm-native) */
/* (file): nothing is ever in flight (ReadFileEx reads at once) */
API(KERNEL32, CancelIo, 1) { return 1; }

/* (from offroad-native) */
API(KERNEL32, CompareFileTime, 2)
{
    uint64_t a = (uint64_t)R32(ARG(0) + 4) << 32 | R32(ARG(0)), b = (uint64_t)R32(ARG(1) + 4) << 32 | R32(ARG(1));
    return a < b ? 0xffffffffu : a > b;
}

/* (from offroad-native) */
API(KERNEL32, CreateFileMappingA, 6)
{
    uint32_t hf = ARG(0), prot = ARG(2), size = ARG(4);   /* (the high half, ARG(3), is 0 here) */
    Mapping *m = calloc(1, sizeof *m);
    m->fd = -1;
    m->size = size;
    if (hf != 0xffffffffu) {
        File *f = file_of(hf);
        if (!f) { free(m); set_last_error(6); return 0; }
        m->fd = f->fd;
        if (!size) {
            struct stat st;
            if (fstat(f->fd, &st) == 0) m->size = (uint32_t)st.st_size;
        }
    }
    uint32_t h = handle_new(H_MISC, m);
    LOG(LOG_FILE, "CreateFileMappingA(%x, prot %x, %u bytes, \"%s\") -> %x", hf, prot, m->size,
        ARG(5) ? gstr(ARG(5)) : "", h);
    return h;
}

/* (from lixs-native) */
API(KERNEL32, FileTimeToDosDateTime, 3)
{
    uint64_t ft = R32(ARG(0)) | (uint64_t)R32(ARG(0) + 4) << 32;
    time_t t = (time_t)(ft / 10000000ull) - (time_t)11644473600ll;
    struct tm tm;
    gmtime_r(&t, &tm);
    if (ARG(1)) W16(ARG(1), (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    if (ARG(2)) W16(ARG(2), (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    return 1;
}

/* (from offroad-native) */
/* C:\ and, with PORT_CD_LABEL, the CD's root */
API(KERNEL32, GetLogicalDriveStringsA, 2)
{
    char list[8] = "C:\\";
    uint32_t need = 4;                                  /* "C:\\" and its NUL */
    if (cd_drive_letter_()) {
        list[4] = cd_drive_letter_(); list[5] = ':'; list[6] = '\\'; list[7] = 0;
        need = 8;
    }
    if (ARG(0) < need + 1)
        return need + 1;
    memcpy(gptr(ARG(1)), list, need);
    W8(ARG(1) + need, 0);
    return need;
}

/* (from offroad-native) */
API(KERNEL32, MapViewOfFile, 5)
{
    Mapping *m = handle_get(ARG(0), H_MISC);
    uint32_t access = ARG(1), off = ARG(3), n = ARG(4);
    if (!m || off > m->size) { set_last_error(6); return 0; }
    if (!n) n = m->size - off;
    uint32_t a = galloc(n ? n : 1);
    memset(gptr(a), 0, n);
    if (m->fd >= 0) {
        ssize_t r = pread(m->fd, gptr(a), n, off);
        (void)r;
    }
    if (access & 2)
        LOG(LOG_ALWAYS, "MapViewOfFile: writable view of %u bytes (writes are not saved to the file)", n);
    LOG(LOG_FILE, "MapViewOfFile(%x, access %x, at %u, %u bytes) -> %08x", ARG(0), access, off, n, a);
    return a;
}

/* (from etm-native) */
/* (file, buffer, bytes, overlapped, completion routine): read at once at the overlapped offset;
   the routine (error, bytes, overlapped) runs at the thread's next alertable wait (sync.c) */
API(KERNEL32, ReadFileEx, 5)
{
    File *f = file_of(ARG(0));
    uint32_t ov = ARG(3), want = ARG(2), got = 0;
    if (!f || !ov) { set_last_error(6); return 0; }
    off_t at = (off_t)(R32(ov + 8) | ((uint64_t)R32(ov + 12) << 32));
    while (got < want) {
        ssize_t r = pread(f->fd, gptr(ARG(1) + got), want - got, at + got);
        if (r <= 0) break;
        got += r;
    }
    LOG(LOG_FILE, "ReadFileEx(%x, %u at %lld) -> %u", ARG(0), want, (long long)at, got);
    if (!got && want) { set_last_error(38); return 0; }                   /* ERROR_HANDLE_EOF */
    overlapped_done(ov, got, 0);
    extern void apc_queue(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);
    apc_queue(ARG(4), 0, got, ov);
    set_last_error(0);
    return 1;
}

/* (from hardtruck2-native) */
API(KERNEL32, UnmapViewOfFile, 1) { gfree(ARG(0)); return 1; }

/* (from offroad-native) */
API(KERNEL32, _lclose, 1)
{
    if (!file_of(ARG(0)))
        return 0xffffffffu;
    handle_close(ARG(0));
    return 0;
}
