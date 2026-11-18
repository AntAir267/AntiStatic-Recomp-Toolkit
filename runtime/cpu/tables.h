/* The tables the translator writes into the generated table.c (translator/recomp.py), as the
   runtime reads them. */
#pragma once
#include "cpu.h"

/* every translated function, by its original address (sorted) */
typedef struct { uint32_t addr; void (*fn)(CPU *c); } FuncEntry;
extern const FuncEntry func_table[];
extern const int func_count;
/* the hooked functions (their wrappers), so a reference build runs the same hooks */
extern const FuncEntry hooked_table[];
extern const int hooked_count;

/* an import the runtime provides: the slot it fills and the C function behind it */
/* fn: the runtime's (or the port's) function, NULL if neither defines one (imports are weak
   references: a port's strong definition beats the runtime's weak default); missing: says so */
typedef struct { uint32_t iat; const char *dll; const char *name; void (*fn)(CPU *c); void (*missing)(CPU *c); } ImportEntry;
extern const ImportEntry import_table[];
extern const int import_count;

/* an import slot another translated module provides: the address written into it */
typedef struct { uint32_t iat, target; } LinkEntry;
extern const LinkEntry link_table[];
extern const int link_count;

/* a fixed-size table the translated code reads through *reloc and checks against *limit
   (build config TABLES), so the runtime can move it and make it bigger */
typedef struct { const char *name; uint32_t *reloc, *limit; uint32_t base, entry, count; } TableEntry;
extern const TableEntry table_list[];

/* the program and the DLLs translated with it (build config MODULES), each at its own base:
   its code sections, and the base it was linked for (relocations apply when they differ) */
typedef struct {
    const char *name, *file, *sha256;
    uint32_t base, size, entry;
    int ntext;
    uint32_t text[8][2];
    uint32_t pref_base;
    int is_dll, at_start;
} ModuleEntry;
extern const ModuleEntry module_table[];
extern const int module_count;

/* instructions a reference run (Unicorn) answers on the host as the translated code does */
extern const uint32_t x87_special_addr[];
extern const uint8_t x87_special_op[];
extern const int x87_special_count;
extern const uint32_t cpu_special_addr[];
extern const uint8_t cpu_special_op[];
extern const int cpu_special_count;
extern const uint32_t sse_special_addr[];
extern const uint8_t sse_special_len[];
extern const int sse_special_count;
