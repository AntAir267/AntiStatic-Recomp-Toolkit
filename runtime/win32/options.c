/* Enhanced options: <user>/options.txt, "key = value" lines, # comments.
   Missing keys keep the faithful defaults. --option key=value on the command line overrides
   the file for one run. The in-game Enhanced page (enhanced.c) edits the same file. */
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"

typedef struct { char key[48], value[208]; int dirty; } Opt;
static Opt opts[64];
static int nopts;

static void set(const char *k, const char *v)
{
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, k)) {
            snprintf(opts[i].value, sizeof opts[i].value, "%s", v);
            return;
        }
    if (nopts < 64) {
        snprintf(opts[nopts].key, sizeof opts[nopts].key, "%s", k);
        snprintf(opts[nopts].value, sizeof opts[nopts].value, "%s", v);
        nopts++;
    }
}

/* "key = value" -> set; returns 0 if the line has no '=' */
int option_parse(const char *line)
{
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    char k[64], v[256];
    const char *a = line, *b = eq;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
    a = eq + 1;
    while (*a && isspace((unsigned char)*a)) a++;
    b = a + strlen(a);
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(v, sizeof v, "%.*s", (int)(b - a), a);
    set(k, v);
    return 1;
}

static void write_template(const char *path);

void options_load(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (!deterministic)
            write_template(path);       /* the first start: every option listed (if the port lists them) */
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s && !option_parse(s))
            LOG(LOG_ALWAYS, "options.txt: \"%s\" is not key = value", s);
    }
    fclose(f);
}

/* Where a key was found, by the address of the caller's key string (some are asked many times a
   frame): a hint, checked; each thread its own. A key not there stays not there until one is added. */
static __thread struct { const char *key; int idx, n; } hint[32];

const char *option_str(const char *key, const char *def)
{
    unsigned h = (unsigned)((uintptr_t)key >> 3) % 32;
    if (hint[h].key == key) {
        int i = hint[h].idx;
        if (i < 0 ? hint[h].n == nopts : !strcasecmp(opts[i].key, key))
            return i < 0 ? def : opts[i].value;
    }
    int found = -1;
    for (int i = 0; i < nopts && found < 0; i++)
        if (!strcasecmp(opts[i].key, key))
            found = i;
    hint[h].key = key;
    hint[h].idx = found;
    hint[h].n = nopts;
    return found < 0 ? def : opts[found].value;
}

int option_bool(const char *key, int def)
{
    const char *v = option_str(key, NULL);
    if (!v) return def;
    return !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

/* every option in force, for the offline checker's recordings (devrec.c) */
int option_count(void) { return nopts; }
const char *option_key(int i) { return i < nopts ? opts[i].key : ""; }
const char *option_value(int i) { return i < nopts ? opts[i].value : ""; }

void option_set(const char *key, const char *value)
{
    set(key, value);
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, key))
            opts[i].dirty = 1;
}

/* a port's own help lines for its keys (a strong port_option_help returns the comment for key,
   or NULL to fall back on the toolkit's): written above a key the first time it is saved
   (Stranglehold's controller_layout) */
__attribute__((weak)) const char *port_option_help(const char *key) { (void)key; return NULL; }

/* comments for keys written to the file the first time */
static const char *const help[][2] = {
    {"display", "display: window or fullscreen (Alt+Enter switches too)"},
    {"rumble", "rumble: on or off (controller vibration)"},
    {"frame_limit", "frame_limit: off, screen (3 under the screen's refresh, for G-Sync / FreeSync) or a rate"},
    {NULL, NULL}};

/* A port's options at their defaults, for the file the first start writes: a strong
   port_option_default gives option i's key and default value and returns 1, or 0 past the last */
__attribute__((weak)) int port_option_default(int i, const char **key, const char **value)
{
    (void)i; (void)key; (void)value;
    return 0;
}

static const char *help_of(const char *key)
{
    const char *ph = port_option_help(key);
    if (ph)
        return ph;
    for (int h = 0; help[h][0]; h++)
        if (!strcasecmp(help[h][0], key))
            return help[h][1];
    return NULL;
}

/* The first start (no options.txt yet, outside test runs): a file listing every option the port
   lists, commented out at its default (Off-Road) */
static void write_template(const char *path)
{
    const char *k, *v;
    if (!port_option_default(0, &k, &v))
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fputs("# " PORT_TITLE ": options (key = value).\n"
          "# Every option is listed at its default, commented out: take away the # to change one.\n", f);
    for (int i = 0; port_option_default(i, &k, &v); i++) {
        const char *h = help_of(k);
        if (h)
            fprintf(f, "\n# %s\n", h);
        fprintf(f, "#%s = %s\n", k, v);
    }
    fclose(f);
}

/* Rewrite options.txt keeping its comments and order: a changed key's line is replaced, keys
   the file does not have yet are appended. */
int options_save(void)
{
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        LOG(LOG_ALWAYS, "options: cannot write %s", tmp);
        return 0;
    }
    int written[64] = {0};
    char line[512];
    if (!in)
        fputs("# " PORT_TITLE ": Enhanced options (key = value), also set from the game's Options menu.\n"
              "# Delete a line to get the original behaviour.\n", out);
    while (in && fgets(line, sizeof line, in)) {
        char k[64] = "";
        const char *eq = strchr(line, '=');
        const char *hash = strchr(line, '#');
        if (eq && (!hash || hash > eq)) {
            const char *a = line, *b = eq;
            while (a < b && isspace((unsigned char)*a)) a++;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
        }
        int done = 0;
        for (int i = 0; i < nopts && k[0]; i++)
            if (!strcasecmp(opts[i].key, k) && opts[i].dirty) {
                if (!written[i]) fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
                written[i] = done = 1;
            }
        if (!done)
            fputs(line, out);
    }
    for (int i = 0; i < nopts; i++)
        if (opts[i].dirty && !written[i]) {
            const char *ph = help_of(opts[i].key);
            if (ph)
                fprintf(out, "\n# %s\n", ph);
            fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
        }
    if (in) fclose(in);
    int ok = fclose(out) == 0 && rename(tmp, path) == 0;
    if (!ok)
        LOG(LOG_ALWAYS, "options: cannot save %s", path);
    return ok;
}

/* The in-game options page changed something: apply it now (the runtime's own options, then the
   port's: its renderer, widescreen, game; from offroad-native's fan-out) */
__attribute__((weak)) void port_options_changed(void) {}
void options_changed(void)
{
    extern void frame_limit_options(void), pad_options(void);
    extern void win_options(void) __attribute__((weak));
    frame_limit_options();
    pad_options();
    if (win_options)
        win_options();
    port_options_changed();
}
