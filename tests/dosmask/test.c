/* tests/dosmask: runtime/win32/dosmask.h against Windows' answers (SwARc's table, MIT License,
   Copyright (c) 2026 JT: runtime/win32/LICENSE.SwARc) */
#include <stdio.h>
#include "dosmask.h"
int main(void)
{
    static const struct { const char *mask, *name; int match; } t[] = {
        {"*", "readme", 1}, {"*", ".", 1}, {"*.*", "readme", 1}, {"*.*", "a.b", 1},
        {"name.*", "name", 1}, {"name.*", "name.txt", 1}, {"name.*", "names", 0}, {"*.txt", "a.TXT", 1},
        {"*.txt", "a.b.txt", 1}, {"*.txt", "a.txt.bak", 0}, {"*.txt", "atxt", 0}, {"a?c", "abc", 1},
        {"a?c", "ac", 0}, {"file?.txt", "file.txt", 1}, {"file?.txt", "file1.txt", 1}, {"file?.txt", "file12.txt", 0},
        {"a.?", "a", 1}, {"a.?", "a.b", 1}, {"*.", "readme", 1}, {"*.", "a.txt", 0},
        {"readme.", "readme", 1}, {"[ab]*", "[ab]c", 1}, {"[ab]*", "a", 0}, {"*\\*", "a\\b", 1},
        {"*\\*", "ab", 0}, {".", ".", 1}, {"..", "..", 1}, {"FOO", "foo", 1},
        /* the masks games use */
        {"*.sav", "SLOT1.SAV", 1}, {"*.dat", "car.dat.bak", 0}, {"save?.dat", "save3.dat", 1}, {"*", "..", 1},
    };
    int fails = 0, n = (int)(sizeof t / sizeof *t);
    for (int i = 0; i < n; i++)
        if (dos_mask_match(t[i].mask, t[i].name) != t[i].match) {
            printf("FAILED: \"%s\" against \"%s\": want %d\n", t[i].mask, t[i].name, t[i].match);
            fails++;
        }
    printf(fails ? "dosmask: %d of %d FAILED\n" : "dosmask: %d of %d ok\n", fails ? fails : n, n);
    return fails != 0;
}
