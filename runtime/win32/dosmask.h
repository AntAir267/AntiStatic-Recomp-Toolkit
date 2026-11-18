/* File-name masks as Windows matches them (FindFirstFile): `?`, `*` and `.` become NT's DOS_QM,
   DOS_STAR and DOS_DOT before matching, so "name.*" matches "name", "file?.txt" matches
   "file.txt", "*." matches a name without an extension, and `[` and `\` are literal. The two
   functions are SwARc's MatchFileMask / MatchDosExpression (https://github.com/JTM-rootstorm/SwARc
   3c396d3, MIT License, Copyright (c) 2026 JT; the licence text is runtime/win32/LICENSE.SwARc),
   written in C here; POSIX fnmatch got all of those wrong. A header (files.c and
   tests/dosmask include it), so no port's file list changes. */
#pragma once
#include <ctype.h>
#include <string.h>

/* '<' DOS_STAR, '>' DOS_QM, '"' DOS_DOT */
static int dos_match(const char *e, const char *n)
{
    for (;; e++) {
        char c = *e;
        if (!c) return !*n;
        if (c == '*' || c == '<') {
            const char *lim = n + strlen(n), *dot = c == '<' ? strrchr(n, '.') : NULL;
            if (dot) lim = dot;
            for (const char *k = n; k <= lim; k++)
                if (dos_match(e + 1, k)) return 1;
            return 0;
        }
        if (c == '>') { if (*n && *n != '.') n++; }
        else if (c == '"') { if (*n == '.') n++; else if (*n) return 0; }
        else if (*n && tolower((unsigned char)*n) == c) n++;
        else return 0;
    }
}

static int dos_mask_match(const char *mask, const char *name)
{
    char e[260];
    size_t len = strlen(mask);
    int had_dot = len && mask[len - 1] == '.';
    if (strspn(mask, ".") != len)                     /* "." and ".." stay as they are */
        while (len && (mask[len - 1] == '.' || mask[len - 1] == ' ')) len--;
    if (len >= sizeof e) len = sizeof e - 1;
    for (size_t i = 0; i < len; i++) e[i] = (char)tolower((unsigned char)mask[i]);
    e[len] = 0;
    if (!strcmp(e, "*.*")) { e[1] = 0; len = 1; }
    else {
        for (size_t i = 1; i < len; i++) {
            if (e[i] == '.' && e[i - 1] == '*') e[i - 1] = '<';
            if ((e[i] == '?' || e[i] == '*') && e[i - 1] == '.') e[i - 1] = '"';
        }
        for (size_t i = 0; i < len; i++) if (e[i] == '?') e[i] = '>';
    }
    if (had_dot && len && e[len - 1] == '*') e[len - 1] = '<';
    return dos_match(e, name);
}
