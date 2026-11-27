/* THE REGISTER AUDIT (from one port's eax sweep, 2026-11-12; docs/REWRITE.md "The register
   audit"). Off unless <PORT_ENV>_AUDIT=1 or <PORT_ENV>_AUDIT_LOG=PREFIX: then rewrite_install puts
   every rewrite (DECOMP) behind a wrapper of its own. With AUDIT=1, on sampled calls during an
   ordinary run the wrapper forks; the child runs the translation from the same state and reports how
   it left the registers, the parent then runs the rewrite and compares: eax, ebx, esi, edi, ebp, esp,
   the return address, the direction flag and the x87 stack (depth and every value pushed); with
   <PORT_ENV>_AUDIT_SLOTS=1 also the call's own argument slots. ecx and edx are scratch and not
   compared.

   Why: a rewrite can return the right answer and still leave a register as the original did not
   (an fnstsw status word in eax, `mov al` over a stale eax, the last pointer or count through eax,
   the old fs:[0] from an exception prologue). No memory hash sees that until a later caller reads
   the register "by accident". In one port this found well over twice as many such rewrites as a
   static scan of the listings had, and no A/B route showed any of them [verified: that port's
   sweep, 2026-11-12/13]. The audit makes "is my rewrite register-exact" a route run, with no spec.

   <PORT_ENV>_AUDIT_OUT=FILE   the report (appended; default <user folder>/audit.txt), one line each:
       ADDR REACHED                         the first audited call of a rewrite
       ADDR DIFF n=.. ret=.. eax_in=.. ecx_in=.. eax T=.. R=.. ...   T the translation, R the rewrite
       ADDR NOCHILD ... why=..              the child ended without a result: why=thunk (a call through
                                            a pointer into the runtime's thunks: Direct3D, sound,
                                            network), why=sys:NAME (it touched a file or a descriptor
                                            it shares with the parent: isolation, below),
                                            why=blocked:CALL progress=N (asleep in the kernel, e.g.
                                            futex, with no call made for 0.2 s: waiting on a lock or
                                            event held by a thread the fork didn't copy),
                                            why=timeout progress=N (still working at 2 s; N counts its
                                            calls through wrappers and pointers), why=ended (it crashed
                                            or hit a fatal error): that rewrite is not audited again
   <PORT_ENV>_AUDIT_MEM=1            also the guest memory each side wrote (soft-dirty pages: the image,
                                     DLLs, regions, the calling thread's stack), " mem A..B T=.. R=.."
   <PORT_ENV>_AUDIT_TAINT=all|addr,addr   on each such function's first four audited calls, the
                                     translation and the rewrite each in a child of its own with NaN,
                                     -NaN, -0 or denormal inputs (every xmm lane and
                                     <PORT_ENV>_AUDIT_TAINT_ARGS "addr:a0,t8,p1+4;...": the N-th stack
                                     argument, the float at this+N, at [argument N]+M):
                                     "ADDR TAINT n=.. v=nan same|<differences>|nochild T|R why=.."
   (always compared too: xmm0-7, mm0-7 and MXCSR bit for bit, and the flags of a function whose callers
   read them, by the translation's flags_out_table)
   <PORT_ENV>_AUDIT_ONLY=addr,addr   only those (hex)
   <PORT_ENV>_AUDIT_SKIP=addr,addr   not those (with the port's PORT_AUDIT_SKIP "addr,addr" in port.h
                                     and the program's entry point, which never returns)
   <PORT_ENV>_AUDIT_SLOTS=1          also the argument slots: each slot the call popped (a callee-pop
                                     function's own, up to 12; a cdecl one's first 6) and the return
                                     address slot, as the call left them (MSVC uses argument slots as
                                     scratch; fncheck.c compares them too): " slotK T=.. R=.. (in ..)",
                                     " retslot ..."

   Sampling: each rewrite's first six calls, the first two from each return address, then calls
   1, 2, 4, 8... up to 60 audited a rewrite; never a call of a function already running further up
   the same thread's stack (a re-entered message handler, a recursion's inner calls: forking those
   reruns what the outer call is in the middle of). The child runs the translation through its
   F_<addr>_orig when the port links -rdynamic (found by name), else through F_<addr> with the
   rewrite answering "not done" (the same, except where INNER_HOOKS puts a port hook in front: that
   hook would run twice). Forked children crash under the NVIDIA driver: audit a game that renders
   under lavapipe (Mesa's software Vulkan). Not with --fncheck at once (it looks for the rewrite itself
   in the hooks, finds the wrapper, and says it could not switch the rewrite off). In --test runs only
   one guest thread runs at a time, so nothing moves while the parent waits for its child.

   ISOLATION (an audited run must stay the run it audits): the child writes nothing the parent can
   see. Its standard output and error and every regular file open for writing only (the log, hash
   files) become /dev/null in the child; then a seccomp filter (Linux, x86-64 and arm64) ends the
   child (NOCHILD why=sys:NAME) at the first system call that would change a file (an open for
   writing, unlink, rename, mkdir, truncate, a shared writable file mapping...), do I/O on a
   descriptor it shares with the parent (sockets, pipes, devices, eventfds: the window system's
   connection, sound) or a file open read-write, or signal another process. Regular files open
   read-only are read as before, their offsets put back after the child. <PORT_ENV>_AUDIT_ISOLATE=0
   switches the filter off (the old behaviour); where it can't be installed the report says so once.

   THE TWO-RUN LOG (<PORT_ENV>_AUDIT_LOG=PREFIX; no fork, so also the functions whose child can't
   finish): every call of every wrapped function is recorded as it returns. A run with
   <PORT_ENV>_DECOMP_OFF=all runs each function's translation through its wrapper and writes
   PREFIX.a.txt; a run of the rewrites writes PREFIX.b.txt (as tools/rewrite/ab.sh's A and B runs do
   with the variable set for both); tools/rewrite/auditlog.py PREFIX compares them. Per function: the
   call count, a digest of the inputs (eax, ecx, edx) and one of what each call left (eax; ebx, esi,
   edi, ebp as kept or their new value; esp; the return address as kept or not; df; the x87 values
   pushed; the argument slots as above, kept or their new value), the two digests at calls 16, 32,
   64..., and the first 16 calls in full. Stack addresses count as one value (the runs' frames
   differ). Needs F_<addr>_orig (-rdynamic): the wrapper runs the translation itself to see it
   return. Written at the program's exit (runtime_at_exit). AUDIT_ONLY and AUDIT_SKIP apply; AUDIT=1
   is ignored beside it. Recording reads only, so the two runs are the A/B pair they would be
   without it. */
#if defined(_WIN32)
/* (the Windows host has no fork and no lookup by name: no audit) */
#include "rewrite.h"
int (*rewrite_audit_wrap(const DecompEntry *d))(CPU *)
{
    const char *v = port_getenv("AUDIT"), *l = port_getenv("AUDIT_LOG");
    static int said;
    if (((v && *v && strcmp(v, "0")) || (l && *l)) && !said++)
        LOG(LOG_ALWAYS, "rewrite: the register audit needs Linux (fork, F_<addr>_orig by name)");
    return d->fn;
}
#else
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include "rewrite.h"

#ifndef PORT_AUDIT_SKIP
#define PORT_AUDIT_SKIP ""                /* (port.h: functions never to audit, "addr,addr" hex) */
#endif

extern int fncheck_solo;                /* sync.c: a forked check child has one thread; no turns */
/* the translation's table (table.c, from AntiStatic v0.2.60): {function, flags its callers read after
   it (CF 1, PF 2, AF 4, ZF 8, SF 16, OF 32)} pairs, ending {0, 0}; absent before: flags not compared */
extern const uint32_t flags_out_table[] __attribute__((weak));

#define AUDIT_MAX 8192
#define NSLOT 13                        /* the return address slot and 12 argument slots */
typedef struct Log Log;
typedef struct {
    const DecompEntry *d;
    void (*orig)(CPU *);                /* F_<addr>_orig when it can be found by name */
    int n, audited, dead, timeouts, logged;
    uint32_t rets[32];
    int retn[32], nrets;
    Log *log;                           /* (the two-run log's record) */
    uint32_t fmask;                     /* EFLAGS bits its callers read after it (the translation's table) */
    int taint, tainted, nti;            /* AUDIT_TAINT: tainted, how many taint runs so far, items */
    struct { char kind; int n; uint32_t off; } ti[16];  /* a: stack argument n; t: float at this+off; p: at [argument n]+off */
} Ent;
static Ent ents[AUDIT_MAX];
static int nents, in_child, child_idx = -1;
/* progress: the child's calls through a wrapper or an indirect call, so the parent can tell a child
   still working from one waiting on something no thread of the child will ever release */
typedef struct {
    uint32_t r[8], ret, ftop, df, slot[NSLOT], mxcsr, eflags, nregions;
    uint8_t st[8][10], xmm[8][16];
    uint64_t mm[8];
    volatile int done;
    int sys, noiso;
    volatile uint32_t progress;
} Out;
static Out *shm, *shm_base;             /* shm_base[0] the audit's child, [1] and [2] a taint run's two */
static FILE *outf;
static int state = -1;                  /* -1 not read yet, 0 off, 1 on */
static int slots_on, isolate = 1;
/* AUDIT_MEM=1: the memory each side wrote, compared too (the kernel's soft-dirty bits say which pages) */
#define PAGE 4096u
#define MAXDIRTY (1u << 20)
static int mem_on, mem_busy, child_mem, pm_fd = -1;
static uint32_t *dirty[3], *dirty_mine; /* [0] count, [1] overflow, then the pages' addresses */
static int taint_all;                   /* AUDIT_TAINT=all */
static int logmode, log_translation;    /* the two-run log: 1 the translation's run, 2 the rewrites' */
static char logpath[1100];

/* the argument slots a call owns, by what it popped: a callee-pop function's own (up to 12), a cdecl
   one's first 6 */
static int nslots(uint32_t entry, uint32_t esp_after)
{
    int popped = (int32_t)(esp_after - entry) / 4 - 1;
    return popped > 0 ? (popped > NSLOT - 1 ? NSLOT - 1 : popped) : 6;
}

/* regular files' offsets, put back after the child (a forked child shares them with its parent) */
typedef struct { int n; struct { int fd; off_t off; } f[256]; } FdSet;
static void fds_save(FdSet *s)
{
    s->n = 0;
    for (int fd = 3; fd < 1024 && s->n < 256; fd++) {
        int fl = fcntl(fd, F_GETFL);
        struct stat st;
        if (fl < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) continue;
        s->f[s->n].fd = fd;
        s->f[s->n++].off = lseek(fd, 0, SEEK_CUR);
    }
}
static void fds_restore(const FdSet *s)
{
    for (int i = 0; i < s->n; i++) lseek(s->f[i].fd, s->f[i].off, SEEK_SET);
}

/* ------------------------------------------------------------------ the child's isolation */
/* what ends the child: t always; o / O an open with write flags (in args[1] / args[2]); f I/O on a
   descriptor shared with the parent (args[0]); m a shared writable file mapping; k a signal to
   another process */
typedef struct { int nr; const char *name; char kind; } Sys;
#define SY(n, k) {__NR_##n, #n, k},
static const Sys sys_list[] = {
#ifdef __NR_open
    SY(open, 'o')
#endif
    SY(openat, 'O')
#ifdef __NR_openat2
    SY(openat2, 't')
#endif
#ifdef __NR_creat
    SY(creat, 't')
#endif
#ifdef __NR_unlink
    SY(unlink, 't')
#endif
    SY(unlinkat, 't')
#ifdef __NR_rename
    SY(rename, 't')
#endif
#ifdef __NR_renameat
    SY(renameat, 't')
#endif
#ifdef __NR_renameat2
    SY(renameat2, 't')
#endif
#ifdef __NR_mkdir
    SY(mkdir, 't')
#endif
    SY(mkdirat, 't')
#ifdef __NR_rmdir
    SY(rmdir, 't')
#endif
#ifdef __NR_link
    SY(link, 't')
#endif
    SY(linkat, 't')
#ifdef __NR_symlink
    SY(symlink, 't')
#endif
    SY(symlinkat, 't')
#ifdef __NR_mknod
    SY(mknod, 't')
#endif
    SY(mknodat, 't')
    SY(truncate, 't')
    SY(ftruncate, 't')
    SY(fallocate, 't')
#ifdef __NR_chmod
    SY(chmod, 't')
#endif
    SY(fchmod, 't')
    SY(fchmodat, 't')
#ifdef __NR_chown
    SY(chown, 't')
#endif
#ifdef __NR_lchown
    SY(lchown, 't')
#endif
    SY(fchown, 't')
    SY(fchownat, 't')
#ifdef __NR_utime
    SY(utime, 't')
#endif
#ifdef __NR_utimes
    SY(utimes, 't')
#endif
#ifdef __NR_futimesat
    SY(futimesat, 't')
#endif
    SY(utimensat, 't')
#ifdef __NR_sendfile
    SY(sendfile, 't')
#endif
    SY(splice, 't')
    SY(tee, 't')
    SY(vmsplice, 't')
#ifdef __NR_copy_file_range
    SY(copy_file_range, 't')
#endif
    SY(process_vm_writev, 't')
    SY(ptrace, 't')
    SY(epoll_ctl, 't')
    SY(mmap, 'm')
    SY(kill, 'k')
    SY(tkill, 'k')
    SY(tgkill, 'k')
    SY(read, 'f')
    SY(write, 'f')
    SY(readv, 'f')
    SY(writev, 'f')
    SY(pread64, 'f')
    SY(pwrite64, 'f')
    SY(preadv, 'f')
    SY(pwritev, 'f')
#ifdef __NR_preadv2
    SY(preadv2, 'f')
    SY(pwritev2, 'f')
#endif
    SY(sendto, 'f')
    SY(recvfrom, 'f')
    SY(sendmsg, 'f')
    SY(recvmsg, 'f')
    SY(sendmmsg, 'f')
    SY(recvmmsg, 'f')
    SY(ioctl, 'f')
    SY(connect, 'f')
    SY(shutdown, 'f')
    SY(accept4, 'f')
#ifdef __NR_accept
    SY(accept, 'f')
#endif
#ifdef __NR_epoll_wait
    SY(epoll_wait, 'f')
#endif
    SY(epoll_pwait, 'f')
#ifdef __NR_epoll_pwait2
    SY(epoll_pwait2, 'f')
#endif
};
#undef SY
/* (names only, for a blocked child's report: not part of the filter) */
static const struct { int nr; const char *name; } wait_names[] = {
    {__NR_futex, "futex"}, {__NR_nanosleep, "nanosleep"}, {__NR_clock_nanosleep, "clock_nanosleep"},
    {__NR_ppoll, "ppoll"}, {__NR_wait4, "wait4"}, {__NR_epoll_pwait, "epoll_pwait"},
};
static const char *sys_name(int nr)
{
    for (size_t i = 0; i < sizeof sys_list / sizeof *sys_list; i++)
        if (sys_list[i].nr == nr) return sys_list[i].name;
    for (size_t i = 0; i < sizeof wait_names / sizeof *wait_names; i++)
        if (wait_names[i].nr == nr) return wait_names[i].name;
    return "?";
}

static void child_sigsys(int sig, siginfo_t *si, void *u)
{
    (void)sig; (void)u;
    shm->done = 3;
    shm->sys = si->si_syscall;
    _exit(0);
}

static struct sock_filter flt[2048];
static int nflt;
static void em(uint16_t code, uint32_t k, uint8_t jt, uint8_t jf)
{
    flt[nflt++] = (struct sock_filter){code, jt, jf, k};
}
#define LDW(off) em(BPF_LD | BPF_W | BPF_ABS, (uint32_t)(off), 0, 0)
#define ARGW(n) (offsetof(struct seccomp_data, args) + 8u * (n))     /* (an argument's low half) */
#define JEQ(k, t, f) em(BPF_JMP | BPF_JEQ | BPF_K, (k), (t), (f))
#define JSET(k, t, f) em(BPF_JMP | BPF_JSET | BPF_K, (k), (t), (f))
#define RET(k) em(BPF_RET | BPF_K, (k), 0, 0)
#define TRAP SECCOMP_RET_TRAP
#define ALLOW SECCOMP_RET_ALLOW

/* in the child, before the call: 0 isolated */
static int child_isolate(void)
{
#if defined(__x86_64__)
    const uint32_t arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    const uint32_t arch = AUDIT_ARCH_AARCH64;
#else
    return -1;
#endif
    int nul = open("/dev/null", O_RDWR);
    struct stat ns;
    if (nul < 0 || fstat(nul, &ns)) return -1;
    dup2(nul, 1);
    dup2(nul, 2);
    int shared[400], nsh = 0;
    for (int fd = 0; fd < 1024; fd++) {
        int fl = fcntl(fd, F_GETFL);
        struct stat st;
        if (fd == 1 || fd == 2 || fd == nul || fl < 0 || fstat(fd, &st)) continue;
        if (S_ISREG(st.st_mode) && (fl & O_ACCMODE) == O_RDONLY) continue;   /* (offsets put back after) */
        if (S_ISREG(st.st_mode) && (fl & O_ACCMODE) == O_WRONLY) { dup2(nul, fd); continue; }
        if (S_ISDIR(st.st_mode) || (S_ISCHR(st.st_mode) && st.st_rdev == ns.st_rdev)) continue;
        if (nsh == 400) return -1;
        shared[nsh++] = fd;             /* sockets, pipes, devices, eventfds, files open read-write */
    }
    const uint32_t wmask = O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND;
    nflt = 0;
    LDW(offsetof(struct seccomp_data, arch));
    JEQ(arch, 1, 0);
    RET(ALLOW);
    LDW(offsetof(struct seccomp_data, nr));
    int ja[64], nja = 0;
    char jk[64];
    for (size_t i = 0; i < sizeof sys_list / sizeof *sys_list; i++) {
        JEQ((uint32_t)sys_list[i].nr, 0, 1);
        if (sys_list[i].kind == 't')
            RET(TRAP);
        else {
            jk[nja] = sys_list[i].kind;
            ja[nja++] = nflt;
            em(BPF_JMP | BPF_JA, 0, 0, 0);
        }
    }
    RET(ALLOW);
    int lab[128] = {0};
    lab['o'] = nflt; LDW(ARGW(1)); JSET(wmask, 0, 1); RET(TRAP); RET(ALLOW);
    lab['O'] = nflt; LDW(ARGW(2)); JSET(wmask, 0, 1); RET(TRAP); RET(ALLOW);
    lab['m'] = nflt; LDW(ARGW(3)); JSET(MAP_SHARED, 0, 5); LDW(ARGW(2)); JSET(PROT_WRITE, 0, 3);
    LDW(ARGW(4)); JEQ(0xffffffffu, 1, 0); RET(TRAP); RET(ALLOW);
    lab['k'] = nflt; LDW(ARGW(0)); JEQ((uint32_t)getpid(), 1, 0); RET(TRAP); RET(ALLOW);
    lab['f'] = nflt; LDW(ARGW(0));
    for (int i = 0; i < nsh; i++) { JEQ((uint32_t)shared[i], 0, 1); RET(TRAP); }
    RET(ALLOW);
    for (int i = 0; i < nja; i++) flt[ja[i]].k = (uint32_t)(lab[(int)jk[i]] - (ja[i] + 1));
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = child_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    struct sock_fprog prog = {(unsigned short)nflt, flt};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ the forked audit */
/* in the child: a call through a pointer into the runtime's thunks (COM methods: Direct3D,
   DirectSound, DirectPlay) ends it without a result (the GPU, sound and network are shared with
   the parent) */
static void child_icall(CPU *c, uint32_t t)
{
    shm->progress++;
    if (t >= THUNK_BASE && t < THUNK_BASE + 16u * THUNK_MAX) {
        shm->done = 2;
        _exit(0);
    }
    icall_hook = NULL;
    icall(c, t);
    icall_hook = child_icall;
}

static int want(Ent *e, uint32_t ret)
{
    if (e->dead || e->audited >= 60) return 0;
    e->n++;
    int k;
    for (k = 0; k < e->nrets; k++) if (e->rets[k] == ret) break;
    if (k == e->nrets && e->nrets < 32) { e->rets[k] = ret; e->retn[k] = 0; e->nrets++; }
    int per = k < 32 ? ++e->retn[k] : 99;
    if (e->n <= 6) return 1;
    if (per <= 2) return 1;
    if ((e->n & (e->n - 1)) == 0) return 1;
    return 0;
}

/* the system call a sleeping child sits in (/proc/PID/stat state S or D, then the number from
   /proc/PID/syscall), or -1 while it runs */
static int child_blocked(pid_t pid)
{
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *p = strrchr(buf, ')');
    if (!p || (p[2] != 'S' && p[2] != 'D')) return -1;
    snprintf(path, sizeof path, "/proc/%d/syscall", (int)pid);
    int nr = -1;
    if ((f = fopen(path, "r"))) { if (fscanf(f, "%d", &nr) != 1) nr = -1; fclose(f); }
    return nr < 0 ? 999 : nr;
}

/* ------------------------------------------------------------------ what a call left */
static void out_fill(Out *o, CPU *c, uint32_t entry)
{
    uint32_t *rr = &c->eax;
    for (int k = 0; k < 8; k++) o->r[k] = rr[k];
    o->ret = c->ret; o->ftop = c->ftop; o->df = c->df;
    for (int k = 0; k < 8; k++) memcpy(o->st[k], &c->st[k], 10);
    for (int k = 0; k < NSLOT; k++) o->slot[k] = R32(entry + 4u * (uint32_t)k);
    memcpy(o->xmm, c->xmm, sizeof o->xmm);
    memcpy(o->mm, c->mm, sizeof o->mm);
    o->mxcsr = c->mxcsr;
    o->eflags = c->eflags;
}

#define ADD(...) do { if (len < cap - 96) len += snprintf(buf + len, (size_t)(cap - len), __VA_ARGS__); } while (0)
/* what t (the translation) and r (the rewrite) left that differs, appended to buf */
static int reg_diff(const Ent *e, const Out *t, const Out *r, uint32_t ftop_in, uint32_t entry, const uint32_t *pre,
                    char *buf, int len, int cap)
{
    static const char *nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (int k = 0; k < 8; k++)
        if (k != 1 && k != 2 && t->r[k] != r->r[k]) ADD(" %s T=%08x R=%08x", nm[k], t->r[k], r->r[k]);
    if (t->ret != r->ret) ADD(" ret T=%08x R=%08x", t->ret, r->ret);
    if (t->df != r->df) ADD(" df");
    if (t->ftop != r->ftop) ADD(" ftop T=%u R=%u", t->ftop, r->ftop);
    else
        for (uint32_t k = r->ftop; k != ftop_in; k = (k + 1) & 7)
            if (memcmp(t->st[k], r->st[k], 10)) { ADD(" st[%u]", k); break; }
    if (slots_on) {
        int ns = nslots(entry, r->r[4]);
        for (int k = 0; k <= ns; k++) {
            if (t->slot[k] == r->slot[k]) continue;
            if (k) ADD(" slot%d T=%08x R=%08x (in %08x)", k - 1, t->slot[k], r->slot[k], pre[k]);
            else ADD(" retslot T=%08x R=%08x (in %08x)", t->slot[k], r->slot[k], pre[k]);
        }
    }
    /* SSE and MMX: the translation keeps them in the CPU record at every call and return, so a caller
       reads what its callee left there (bit for bit: NaN payloads and signed zeros count) */
    for (int k = 0; k < 8; k++)
        if (memcmp(t->xmm[k], r->xmm[k], 16)) {
            uint32_t a[4], b[4];
            memcpy(a, t->xmm[k], 16);
            memcpy(b, r->xmm[k], 16);
            ADD(" xmm%d T=%08x:%08x:%08x:%08x R=%08x:%08x:%08x:%08x", k, a[3], a[2], a[1], a[0], b[3], b[2], b[1], b[0]);
        }
    for (int k = 0; k < 8; k++)
        if (t->mm[k] != r->mm[k]) ADD(" mm%d T=%016llx R=%016llx", k, (unsigned long long)t->mm[k], (unsigned long long)r->mm[k]);
    if (t->mxcsr != r->mxcsr) ADD(" mxcsr T=%08x R=%08x", t->mxcsr, r->mxcsr);
    if (e->fmask && ((t->eflags ^ r->eflags) & e->fmask))
        ADD(" flags T=%03x R=%03x (its callers read %03x)", t->eflags & e->fmask, r->eflags & e->fmask, e->fmask);
    return len;
}

/* ------------------------------------------------------------------ the memory written */
static int sd_clear(void)
{
    int fd = open("/proc/self/clear_refs", O_WRONLY);
    if (fd < 0) return -1;
    int ok = write(fd, "4", 1) == 1;
    close(fd);
    return ok ? 0 : -1;
}
typedef struct { int fd; uint32_t *list; uint32_t n; } SdWalk;
static void sd_range(SdWalk *w, uint32_t base, uint32_t size)
{
    uint64_t pm[512], end = (uint64_t)base + size;
    for (uint64_t a = base & ~(uint64_t)(PAGE - 1); a < end;) {
        uint64_t n = (end - a + PAGE - 1) / PAGE;
        if (n > 512) n = 512;
        ssize_t got = pread(w->fd, pm, (size_t)n * 8, (off_t)(GP((uint32_t)a) / PAGE) * 8);
        if (got <= 0) break;
        n = (uint64_t)got / 8;
        for (uint64_t k = 0; k < n; k++)
            if ((pm[k] >> 55) & 1) {
                if (w->list[0] < MAXDIRTY) w->list[2 + w->list[0]++] = (uint32_t)(a + k * PAGE);
                else w->list[1] = 1;
            }
        a += n * PAGE;
    }
    w->n++;
}
static void sd_cb(void *u, uint32_t base, uint32_t size, int kind)
{
    if (kind != 3) sd_range(u, base, size);   /* (thread stacks: the calling thread's, below) */
}
/* the guest pages this process wrote since its last sd_clear: the image, DLLs, regions, this
   thread's stack; returns the number of ranges */
static uint32_t sd_collect(int fd, uint32_t *list, const CPU *c)
{
    list[0] = list[1] = 0;
    SdWalk w = {fd, list, 0};
    mem_walk(sd_cb, &w);
    if (c->stack_hi > c->stack_lo) sd_range(&w, c->stack_lo, c->stack_hi - c->stack_lo);
    return w.n;
}
static int u32cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static int page_read(pid_t p, uint32_t a, uint8_t *out)
{
    struct iovec l = {out, PAGE}, r = {(void *)GP(a), PAGE};
    return process_vm_readv(p, &l, 1, &r, 1, 0) == (ssize_t)PAGE ? 0 : -1;
}
/* the pages either side wrote, A's copy (process pa) against B's (pb): differences outside the dead
   stack below the call appended to buf; *dead counts the dead stack's */
static int mem_diff(pid_t pa, pid_t pb, const uint32_t *la, const uint32_t *lb, uint32_t entry, uint32_t stack_lo,
                    char *buf, int len, int cap, int *dead)
{
    *dead = 0;
    if (la[1] || lb[1]) { ADD(" mem: more than %u pages written, not compared", MAXDIRTY); return len; }
    uint32_t n = la[0] + lb[0], m = 0, shown = 0, more = 0;
    uint32_t *u = malloc(((size_t)n + 1) * 4);
    if (!u) return len;
    memcpy(u, la + 2, (size_t)la[0] * 4);
    memcpy(u + la[0], lb + 2, (size_t)lb[0] * 4);
    qsort(u, n, 4, u32cmp);
    for (uint32_t k = 0; k < n; k++) if (!m || u[k] != u[m - 1]) u[m++] = u[k];
    static uint8_t pa_[PAGE], pb_[PAGE];
    for (uint32_t k = 0; k < m; k++) {
        uint32_t a = u[k];
        int ea = page_read(pa, a, pa_), eb = page_read(pb, a, pb_);
        if (ea || eb) {
            if (ea != eb) { if (shown < 8) { ADD(" mem %08x readable only in %s", a, ea ? "R" : "T"); shown++; } else more++; }
            continue;
        }
        if (!memcmp(pa_, pb_, PAGE)) continue;
        if (a + PAGE <= entry && a >= stack_lo) { (*dead)++; continue; }
        uint32_t i = 0, j = PAGE - 1;
        while (pa_[i] == pb_[i]) i++;
        while (pa_[j] == pb_[j]) j--;
        if (a < entry && a + PAGE > entry && a + j < entry) { (*dead)++; continue; }
        if (shown >= 8) { more++; continue; }
        uint32_t at = i & ~3u, x, y;
        memcpy(&x, pa_ + at, 4);
        memcpy(&y, pb_ + at, 4);
        ADD(" mem %08x..%08x T=%08x R=%08x", a + i, a + j, x, y);
        shown++;
    }
    if (more) ADD(" (+%u pages)", more);
    free(u);
    return len;
}

/* ------------------------------------------------------------------ the children */
static void run_orig(Ent *e, CPU *c)
{
    if (e->orig) e->orig(c);
    else icall(c, e->d->addr);
}

/* AUDIT_TAINT: the inputs a taint run sets, by variant: quiet NaNs with payloads all their own,
   negative ones, minus zero, the smallest denormals (positive and negative by turns) */
static const char *const tv_name[4] = {"nan", "-nan", "-0", "denormal"};
static uint32_t tv_value(int v, int idx)
{
    switch (v) {
    case 0: return 0x7fc00000u | (uint32_t)(idx + 1);
    case 1: return 0xffc00000u | (uint32_t)(0x100 + idx);
    case 2: return 0x80000000u;
    default: return (idx & 1 ? 0x80000000u : 0) | (uint32_t)(1 + (idx >> 1));
    }
}
static void taint_apply(const Ent *e, CPU *c, int v)
{
    uint32_t entry = c->esp;
    int idx = 0;
    for (int k = 0; k < 8; k++) {           /* every xmm lane: a leftover a function should not read */
        uint32_t q[4];
        for (int l = 0; l < 4; l++) q[l] = tv_value(v, idx++);
        memcpy(&c->xmm[k], q, 16);
    }
    for (int k = 0; k < e->nti; k++) {
        uint32_t at = 0;
        if (e->ti[k].kind == 'a') at = entry + 4u * (uint32_t)(e->ti[k].n + 1);
        else if (e->ti[k].kind == 't') at = c->ecx ? c->ecx + e->ti[k].off : 0;
        else { uint32_t p = R32(entry + 4u * (uint32_t)(e->ti[k].n + 1)); at = p ? p + e->ti[k].off : 0; }
        if (at >= 0x10000u) W32(at, tv_value(v, idx++));
    }
}

/* a forked child's whole life: role 0 the audit's translation, 1 a taint run's translation, 2 a taint
   run's rewrite (its own declining: the translation). With child_mem it lists the pages it wrote
   and waits to be killed, so the parent can read them. */
static __attribute__((noreturn)) void child_body(int i, CPU *c, int role, uint32_t *list, int tid0, int tv)
{
    Ent *e = &ents[i];
    sync_fork_child(tid0);              /* (a critical section this call holds: re-entered in the child) */
    in_child = 1; child_idx = role == 2 ? -1 : i; fncheck_solo = 1;
    int pm = -1;
    if (child_mem && sd_clear() == 0) pm = open("/proc/self/pagemap", O_RDONLY);
    if (isolate && child_isolate()) shm->noiso = 1;
    icall_hook = child_icall;           /* no Direct3D, sound or network in the child */
    uint32_t entry = c->esp;
    if (tv >= 0) taint_apply(e, c, tv);
    if (role == 2) { if (!e->d->fn(c)) run_orig(e, c); }
    else run_orig(e, c);
    out_fill(shm, c, entry);
    if (list) {
        if (pm >= 0) shm->nregions = sd_collect(pm, list, c);
        else list[0] = 0, list[1] = 1;
    }
    __atomic_store_n(&shm->done, 1, __ATOMIC_RELEASE);
    if (child_mem) for (;;) pause();
    _exit(0);
}
static void reap(pid_t pid)
{
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* the parent waits for the child at shm: 1 its result is there (with child_mem it is still alive,
   *alive set), 0 none (shm->done, *blocked and *timed_out say why) */
static int child_wait(pid_t pid, Ent *e, int *blocked, int *timed_out, int *alive)
{
    int ws = 0, ok = 0;
    *blocked = -1; *timed_out = 0; *alive = 0;
    struct timespec t0, t1, d = {0, 200000};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint32_t seen = 0;
    double seen_at = 0;
    for (;;) {
        if (child_mem && __atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 1) { *alive = 1; return 1; }
        pid_t w = waitpid(pid, &ws, WNOHANG);
        if (w == pid) { ok = shm->done == 1; if (shm->done >= 2) e->dead = 1; break; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= 2) { kill(pid, SIGKILL); waitpid(pid, &ws, 0); *timed_out = 1; break; }
        /* a child asleep in the kernel with no progress for 0.2 s waits on something only a thread
           the fork didn't copy would release (a lock, an event): ended now, with the call it is in */
        if (shm->progress != seen) { seen = shm->progress; seen_at = el; }
        else if (el - seen_at >= 0.2) {
            int nr = child_blocked(pid);
            if (nr >= 0) { *blocked = nr; kill(pid, SIGKILL); waitpid(pid, &ws, 0); *timed_out = 1; break; }
            seen_at = el;
        }
        nanosleep(&d, NULL);
        if (d.tv_nsec < 20000000) d.tv_nsec *= 2;
    }
    return ok;
}
static void why_of(char *why, size_t n, const Out *o, int blocked, int timed_out)
{
    if (o->done == 2) snprintf(why, n, "thunk");
    else if (o->done == 3) snprintf(why, n, "sys:%s", sys_name(o->sys));
    else if (blocked >= 0) snprintf(why, n, "blocked:%s progress=%u", sys_name(blocked), o->progress);
    else if (timed_out) snprintf(why, n, "timeout progress=%u", o->progress);
    else snprintf(why, n, "ended");
}

/* AUDIT_TAINT: the translation and the rewrite each in a child of its own, from the same state with
   the same tainted inputs (the parent runs neither: its run goes on untouched) */
static void taint_run(int i, CPU *c)
{
    Ent *e = &ents[i];
    int v = e->tainted++ & 3, tid0 = (int)gettid();
    uint32_t entry = c->esp, ftop_in = c->ftop, pre[NSLOT];
    for (int k = 0; k < NSLOT; k++) pre[k] = R32(entry + 4u * (uint32_t)k);
    FdSet fds;
    fds_save(&fds);
    pid_t pid[2] = {0, 0};
    int ok[2] = {0, 0}, alive[2] = {0, 0}, blocked[2], tmo[2];
    Out o[2];
    int was_mem = child_mem;
    child_mem = mem_on && !mem_busy;
    for (int w = 0; w < 2; w++) {
        shm = &shm_base[1 + w];
        memset(shm, 0, sizeof *shm);
        fflush(NULL);
        pid[w] = fork();
        if (pid[w] == 0) child_body(i, c, 1 + w, child_mem ? dirty[1 + w] : NULL, tid0, v);
        if (pid[w] < 0) break;
        ok[w] = child_wait(pid[w], e, &blocked[w], &tmo[w], &alive[w]);
        o[w] = *shm;
        fds_restore(&fds);
        if (!ok[w]) break;
    }
    shm = &shm_base[0];
    if (e->dead) e->dead = 0;           /* (a taint run's failure doesn't end the audit's) */
    char buf[2400];
    int len = 0, cap = (int)sizeof buf;
    if (ok[0] && ok[1]) {
        len = reg_diff(e, &o[0], &o[1], ftop_in, entry, pre, buf, len, cap);
        if (child_mem) {
            int dead = 0;
            len = mem_diff(pid[0], pid[1], dirty[1], dirty[2], entry, c->stack_lo, buf, len, cap, &dead);
            if (o[0].nregions != o[1].nregions) ADD(" regions T=%u R=%u", o[0].nregions, o[1].nregions);
            if (len && dead) ADD(" (and %d dead-stack pages)", dead);
        }
        if (outf) {
            fprintf(outf, "%08x TAINT n=%d v=%s%s\n", e->d->addr, e->n, tv_name[v], len ? buf : " same");
            fflush(outf);
        }
    } else if (outf) {
        int w = ok[0] ? 1 : 0;
        char why[80];
        if (pid[w] < 0) snprintf(why, sizeof why, "fork");
        else why_of(why, sizeof why, &o[w], blocked[w], tmo[w]);
        fprintf(outf, "%08x TAINT n=%d v=%s nochild %s why=%s\n", e->d->addr, e->n, tv_name[v], w ? "R" : "T", why);
        fflush(outf);
    }
    for (int w = 0; w < 2; w++) if (alive[w]) reap(pid[w]);
    child_mem = was_mem;
}

static int fork_audit(int i, CPU *c)
{
    Ent *e = &ents[i];
    uint32_t entry = c->esp, ret = R32(entry);
    if (!want(e, ret))
        return e->d->fn(c);
    if (e->audited++ == 0 && outf) { fprintf(outf, "%08x REACHED\n", e->d->addr); fflush(outf); }
    if (e->taint && e->tainted < 4) taint_run(i, c);
    uint32_t ftop_in = c->ftop, eax_in = c->eax, ecx_in = c->ecx;
    uint32_t pre[NSLOT];
    for (int k = 0; k < NSLOT; k++) pre[k] = R32(entry + 4u * (uint32_t)k);
    FdSet fds;
    fds_save(&fds);
    shm = &shm_base[0];
    memset(shm, 0, sizeof *shm);
    int memhere = mem_on && !mem_busy;  /* (an audit inside a rewrite being compared: registers only) */
    child_mem = memhere;
    fflush(NULL);                       /* (the child must not write the parent's buffers twice) */
    int tid0 = (int)gettid();
    pid_t pid = fork();
    if (pid < 0) { e->dead = 1; child_mem = 0; return e->d->fn(c); }
    if (pid == 0) child_body(i, c, 0, memhere ? dirty[0] : NULL, tid0, -1);
    int blocked, timed_out, alive;
    int ok = child_wait(pid, e, &blocked, &timed_out, &alive);
    child_mem = 0;
    fds_restore(&fds);
    Out o = *shm;                       /* (the rewrite's own nested audits reuse shm) */
    if (o.noiso && isolate) {
        isolate = 0;
        if (outf) fprintf(outf, "# the child could not be isolated (seccomp): children run as they are, "
                                "their file writes reach the user folder\n");
    }
    if (!ok && ++e->timeouts >= 1) e->dead = 1;
    uint32_t *theirs = NULL;            /* (the child's page list, copied: nested audits reuse dirty[0]) */
    if (memhere && ok && dirty[0][1] == 0) {
        theirs = malloc(((size_t)dirty[0][0] + 2) * 4);
        if (theirs) memcpy(theirs, dirty[0], ((size_t)dirty[0][0] + 2) * 4);
    }
    int compare_mem = memhere && ok && sd_clear() == 0;
    if (compare_mem) mem_busy++;
    int r = e->d->fn(c);
    if (compare_mem) mem_busy--;
    if (!r || !ok) {
        if (alive) reap(pid);
        free(theirs);
        if (!ok && outf && e->logged < 8) {
            e->logged++;
            char why[80];
            why_of(why, sizeof why, &o, blocked, timed_out);
            fprintf(outf, "%08x NOCHILD n=%d ret=%08x dead=%d why=%s%s\n", e->d->addr, e->n, ret, e->dead, why,
                    r ? "" : " (rewrite declined)");
            fflush(outf);
        }
        return r;
    }
    Out q;
    out_fill(&q, c, entry);
    char buf[2400];
    int len = 0, cap = (int)sizeof buf;
    len = reg_diff(e, &o, &q, ftop_in, entry, pre, buf, len, cap);
    if (compare_mem) {
        int dead = 0;
        if (!theirs) ADD(" mem: more than %u pages written, not compared", MAXDIRTY);
        else {
            q.nregions = sd_collect(pm_fd, dirty_mine, c);
            len = mem_diff(pid, getpid(), theirs, dirty_mine, entry, c->stack_lo, buf, len, cap, &dead);
            if (o.nregions != q.nregions) ADD(" regions T=%u R=%u", o.nregions, q.nregions);
            if (len && dead) ADD(" (and %d dead-stack pages)", dead);
        }
    }
    if (alive) reap(pid);
    free(theirs);
    if (len && outf && e->logged < 12) {
        e->logged++;
        fprintf(outf, "%08x DIFF n=%d ret=%08x eax_in=%08x ecx_in=%08x%s\n", e->d->addr, e->n, ret, eax_in, ecx_in, buf);
        fflush(outf);
    }
    return r;
}
#undef ADD

/* the calls of audited functions running on this thread, outermost first (frames left without
   coming back through the wrapper, unwound past or longjmp'd over, drop out by their esp) */
#define ACT_MAX 256
static __thread struct { int i; uint32_t esp; } act[ACT_MAX];
static __thread int nact;

/* ------------------------------------------------------------------ the two-run log */
#define NREC 16
#define NCP 28
#define STK 0x57ac57acu                 /* (a stack address) */
typedef struct { uint32_t in[3], out[9], slot[NSLOT], kept, ns; } Rec;
/* hout: everything a call left; hreg: the same without the argument slots (a slot-only difference is
   counted apart: a callee writing through a pointer into its caller's frame, which the two runs' callers
   lay out differently, is no leftover) */
struct Log { uint32_t n, declined, hin, hout, hreg; int ncp, nrec; uint32_t cpn[NCP], cpi[NCP], cpo[NCP], cpr[NCP]; Rec rec[NREC]; };
static __thread int pass_i = -1;        /* the call whose translation the wrapper runs: its own entry */
static __thread uint32_t pass_esp;      /* comes back to the wrapper first, which then answers "not done" */
static uint32_t mixh(uint32_t h, uint32_t v) { h ^= v; h *= 16777619u; return h ^ (h >> 15); }
static uint32_t norm(uint32_t v, uint32_t entry) { return v - entry + 0x100000u < 0x200000u ? STK : v; }

static int log_call(int i, CPU *c)
{
    Ent *e = &ents[i];
    if (pass_i == i && pass_esp == c->esp) { pass_i = -1; return 0; }
    Log *l = e->log ? e->log : (e->log = calloc(1, sizeof *e->log));
    if (!l) return logmode == 1 ? 0 : e->d->fn(c);
    uint32_t entry = c->esp, ftop0 = c->ftop, pre[NSLOT];
    uint32_t in[7] = {c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->ebp};
    for (int k = 0; k < NSLOT; k++) pre[k] = R32(entry + 4u * (uint32_t)k);
    int r = logmode == 2 ? e->d->fn(c) : 0;
    if (!r) {
        if (logmode == 2) l->declined++;
        pass_i = i; pass_esp = entry;
        e->orig(c);
        pass_i = -1;
    }
    int32_t d = (int32_t)(c->esp - entry);
    if (d < -0x10000 || d > 0x10000) return 1;   /* (did not come back as a call returns) */
    Rec q;
    memset(&q, 0, sizeof q);
    for (int k = 0; k < 3; k++) q.in[k] = norm(in[k], entry);
    q.out[0] = norm(c->eax, entry);
    const uint32_t now[4] = {c->ebx, c->esi, c->edi, c->ebp};
    for (int k = 0; k < 4; k++) {
        if (now[k] == in[3 + k]) q.kept |= 1u << (1 + k);
        else q.out[1 + k] = norm(now[k], entry);
    }
    q.out[5] = (uint32_t)d;
    if (c->ret == pre[0]) q.kept |= 1u << 6;
    else q.out[6] = norm(c->ret, entry);
    q.out[7] = c->df;
    uint32_t pushed = (ftop0 - c->ftop) & 7, fh = 0x811c9dc5u;
    for (uint32_t k = 0; k < pushed; k++) {
        const uint8_t *b = (const uint8_t *)&c->st[(c->ftop + k) & 7];
        for (int j = 0; j < 10; j++) fh = mixh(fh, b[j]);
    }
    q.out[8] = pushed << 24 | (pushed ? fh & 0xffffffu : 0);
    q.ns = (uint32_t)nslots(entry, c->esp);
    for (uint32_t k = 0; k <= q.ns; k++) {
        uint32_t v = R32(entry + 4u * k);
        if (v == pre[k]) q.kept |= 1u << (9 + k);
        else q.slot[k] = norm(v, entry);
    }
    uint32_t ih = 0, oh = q.ns, rh = 0;
    for (int k = 0; k < 3; k++) ih = mixh(ih, q.in[k]);
    for (int k = 0; k < 9; k++) oh = mixh(oh, (q.kept >> k) & 1 ? 0x9e3779b9u : q.out[k]);
    for (int k = 0; k < 9; k++) rh = mixh(rh, (q.kept >> k) & 1 ? 0x9e3779b9u : q.out[k]);
    for (uint32_t k = 0; k <= q.ns; k++) oh = mixh(oh, (q.kept >> (9 + k)) & 1 ? 0x9e3779b9u : q.slot[k]);
    l->n++;
    l->hin = mixh(l->hin, ih);
    l->hout = mixh(l->hout, oh);
    l->hreg = mixh(l->hreg, rh);
    if (l->nrec < NREC) l->rec[l->nrec++] = q;
    if (l->n >= 16 && (l->n & (l->n - 1)) == 0 && l->ncp < NCP) {
        l->cpn[l->ncp] = l->n; l->cpi[l->ncp] = l->hin; l->cpo[l->ncp] = l->hout; l->cpr[l->ncp++] = l->hreg;
    }
    return 1;
}

static const char *val(char *b, uint32_t v, int kept)
{
    if (kept) return "=";
    if (v == STK) return "stk";
    sprintf(b, "%x", v);
    return b;
}
static void log_write(void)
{
    if (in_child || !logmode) return;
    FILE *f = fopen(logpath, "w");
    if (!f) { LOG(LOG_ALWAYS, "rewrite: audit log: can't write %s: %s", logpath, strerror(errno)); return; }
    int nf = 0;
    for (int i = 0; i < nents && i < AUDIT_MAX; i++) nf += ents[i].log && ents[i].log->n;
    fprintf(f, "# audit log: the %s run, %d functions called\n", logmode == 1 ? "translation's" : "rewrites'", nf);
    char b[16];
    for (int i = 0; i < nents && i < AUDIT_MAX; i++) {
        Log *l = ents[i].log;
        if (!l || !l->n) continue;
        uint32_t a = ents[i].d->addr;
        fprintf(f, "%08x n=%u declined=%u in=%08x out=%08x reg=%08x cp=", a, l->n, l->declined, l->hin, l->hout, l->hreg);
        for (int k = 0; k < l->ncp; k++) fprintf(f, "%s%u:%08x:%08x:%08x", k ? "," : "", l->cpn[k], l->cpi[k], l->cpo[k], l->cpr[k]);
        fprintf(f, "%s\n", l->ncp ? "" : "-");
        for (int j = 0; j < l->nrec; j++) {
            Rec *q = &l->rec[j];
            fprintf(f, "%08x #%d in=", a, j);
            for (int k = 0; k < 3; k++) fprintf(f, "%s%s", k ? "," : "", val(b, q->in[k], 0));
            fprintf(f, " out=");
            for (int k = 0; k < 9; k++) fprintf(f, "%s%s", k ? "," : "", val(b, q->out[k], (q->kept >> k) & 1));
            fprintf(f, " slots=");
            for (uint32_t k = 0; k <= q->ns; k++) fprintf(f, "%s%s", k ? "," : "", val(b, q->slot[k], (q->kept >> (9 + k)) & 1));
            fprintf(f, "\n");
        }
    }
    fclose(f);
    LOG(LOG_ALWAYS, "rewrite: audit log: %d functions written to %s", nf, logpath);
}

/* ------------------------------------------------------------------ the wrapper */
static int audit(int i, CPU *c)
{
    Ent *e = &ents[i];
    if (in_child) {                     /* (the audited one answers "not done": its translation runs) */
        shm->progress++;
        return i == child_idx ? 0 : e->d->fn(c);
    }
    if (logmode)
        return log_call(i, c);
    uint32_t entry = c->esp;
    while (nact && act[nact - 1].esp <= entry) nact--;
    int running = 0;
    for (int k = nact - 1; k >= 0 && !running; k--) running = act[k].i == i;
    if (nact < ACT_MAX) { act[nact].i = i; act[nact].esp = entry; nact++; }
    else {                              /* (deeper than the list: recursion past it isn't seen) */
        static int said;
        if (!said++ && outf) fprintf(outf, "# audited calls nest deeper than %d: a recursion past that may be forked\n", ACT_MAX);
    }
    int r = running ? e->d->fn(c) : fork_audit(i, c);
    while (nact && act[nact - 1].esp <= entry) nact--;
    return r;
}

/* one wrapper per rewrite (a hook has no argument saying which function it is): at_0000 .. at_1fff
   (the macros' parameters are named so no hex digit a-f is one of them) */
#define AT4(w, x, y, z) static int at_##w##x##y##z(CPU *cpu) { return audit(0x##w##x##y##z, cpu); }
#define AT3(w, x, y) AT4(w, x, y, 0) AT4(w, x, y, 1) AT4(w, x, y, 2) AT4(w, x, y, 3) AT4(w, x, y, 4) \
    AT4(w, x, y, 5) AT4(w, x, y, 6) AT4(w, x, y, 7) AT4(w, x, y, 8) AT4(w, x, y, 9) AT4(w, x, y, a) \
    AT4(w, x, y, b) AT4(w, x, y, c) AT4(w, x, y, d) AT4(w, x, y, e) AT4(w, x, y, f)
#define AT2(w, x) AT3(w, x, 0) AT3(w, x, 1) AT3(w, x, 2) AT3(w, x, 3) AT3(w, x, 4) AT3(w, x, 5) AT3(w, x, 6) \
    AT3(w, x, 7) AT3(w, x, 8) AT3(w, x, 9) AT3(w, x, a) AT3(w, x, b) AT3(w, x, c) AT3(w, x, d) AT3(w, x, e) AT3(w, x, f)
#define AT1(w) AT2(w, 0) AT2(w, 1) AT2(w, 2) AT2(w, 3) AT2(w, 4) AT2(w, 5) AT2(w, 6) AT2(w, 7) AT2(w, 8) \
    AT2(w, 9) AT2(w, a) AT2(w, b) AT2(w, c) AT2(w, d) AT2(w, e) AT2(w, f)
AT1(0)
AT1(1)
#undef AT4
#define AT4(w, x, y, z) at_##w##x##y##z,
static int (*const tramps[AUDIT_MAX])(CPU *) = {AT1(0) AT1(1)};

static int in_only(const char *s, uint32_t a)
{
    while (s && *s) {
        char *q;
        unsigned long v = strtoul(s, &q, 16);
        if (q == s) break;
        if (v == a) return 1;
        while (*q == ',' || *q == ' ') q++;
        s = q;
    }
    return 0;
}

/* the two-run log's translation run: <PORT_ENV>_DECOMP_OFF=all would leave every rewrite out before
   rewrite_install asks for a wrapper; read first and cleared, so each wrapper is put in and runs its
   function's translation (rewrite.c reads the variable at install, after this) */
__attribute__((constructor)) static void audit_env(void)
{
    const char *l = port_getenv("AUDIT_LOG"), *off = port_getenv("DECOMP_OFF");
    if (l && *l && off && !strcasecmp(off, "all")) {
        log_translation = 1;
        setenv(PORT_ENV "_DECOMP_OFF", "", 1);
    }
}

/* AUDIT_MEM: the soft-dirty bits work here (a page written after a clear shows, one not written
   doesn't); the shared page lists made */
static int softdirty_init(void)
{
    uint8_t *pg = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pg == MAP_FAILED) return 0;
    pg[0] = pg[PAGE] = 1;
    int fd = open("/proc/self/pagemap", O_RDONLY);
    uint64_t e[2] = {0, 0};
    int ok = fd >= 0 && sd_clear() == 0;
    if (ok) {
        pg[0] = 2;
        ok = pread(fd, e, 16, (off_t)((uintptr_t)pg / PAGE) * 8) == 16 && ((e[0] >> 55) & 1) && !((e[1] >> 55) & 1);
    }
    munmap(pg, 2 * PAGE);
    if (!ok) { if (fd >= 0) close(fd); return 0; }
    pm_fd = fd;
    size_t sz = ((size_t)MAXDIRTY + 2) * 4;
    for (int k = 0; k < 3; k++) {
        dirty[k] = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (dirty[k] == MAP_FAILED) return 0;
    }
    dirty_mine = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return dirty_mine != MAP_FAILED;
}

/* AUDIT_TAINT_ARGS "addr:a0,t8,p1+4;addr:..." : a function's float inputs a taint run sets */
static void taint_items(Ent *e)
{
    const char *s = port_getenv("AUDIT_TAINT_ARGS");
    while (s && *s) {
        char *q;
        unsigned long a = strtoul(s, &q, 16);
        if (q == s) break;
        s = q;
        if (*s == ':') s++;
        while (*s && *s != ';') {
            char kind = *s++;
            unsigned long n = strtoul(s, &q, 10), off = 0;
            s = q;
            if (kind == 't') off = n, n = 0;
            if (kind == 'p' && *s == '+') off = strtoul(s + 1, &q, 0), s = q;
            if (a == e->d->addr && (kind == 'a' || kind == 't' || kind == 'p') && e->nti < 16) {
                e->ti[e->nti].kind = kind; e->ti[e->nti].n = (int)n; e->ti[e->nti].off = (uint32_t)off; e->nti++;
            }
            while (*s == ',' || *s == ' ') s++;
        }
        if (*s == ';') s++;
    }
}

static void audit_init(void)
{
    state = 0;
    const char *v = port_getenv("AUDIT"), *l = port_getenv("AUDIT_LOG");
    int forks = v && *v && strcmp(v, "0") != 0;
    if (l && *l) {
        logmode = log_translation ? 1 : 2;
        snprintf(logpath, sizeof logpath, "%s.%s.txt", l, logmode == 1 ? "a" : "b");
        state = 1;
        runtime_at_exit(log_write);
        LOG(LOG_ALWAYS, "rewrite: audit log on (the %s run): %s at exit%s", logmode == 1 ? "translation's" : "rewrites'",
            logpath, forks ? " (no forks: AUDIT=1 is ignored beside AUDIT_LOG)" : "");
        return;
    }
    if (!forks) return;
    state = 1;
    const char *o = port_getenv("AUDIT_OUT");
    char def[1100];
    snprintf(def, sizeof def, "%s/audit.txt", user_dir);
    outf = fopen(o && *o ? o : def, "a");
    shm_base = mmap(NULL, 3 * sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm_base == MAP_FAILED) fatal("audit: no shared memory: %s", strerror(errno));
    shm = shm_base;
    const char *mv = port_getenv("AUDIT_MEM");
    if (mv && *mv && strcmp(mv, "0")) mem_on = softdirty_init();
    const char *tv = port_getenv("AUDIT_TAINT");
    taint_all = tv && !strcasecmp(tv, "all");
    const char *s = port_getenv("AUDIT_SLOTS"), *iso = port_getenv("AUDIT_ISOLATE");
    slots_on = s && *s && strcmp(s, "0") != 0;
    if (iso && !strcmp(iso, "0")) isolate = 0;
    LOG(LOG_ALWAYS, "rewrite: register audit on%s%s%s%s, report in %s", slots_on ? " (argument slots too)" : "",
        mem_on ? " (memory written too)" : "", tv && *tv ? " (taint runs)" : "",
        isolate ? "" : " (children not isolated)", o && *o ? o : def);
    if (outf && !flags_out_table)
        fprintf(outf, "# flags not compared: the translation has no flags_out_table (translated before AntiStatic v0.2.60)\n");
    if (outf && mv && *mv && strcmp(mv, "0") && !mem_on)
        fprintf(outf, "# memory not compared: the kernel's soft-dirty page bits are not available here\n");
}

/* rewrite_install's choice for each rewrite: itself, or (<PORT_ENV>_AUDIT=1 / _AUDIT_LOG) its wrapper */
int (*rewrite_audit_wrap(const DecompEntry *d))(CPU *)
{
    if (state < 0) audit_init();
    if (!state)
        return d->fn;
    /* a function left alone: its rewrite, or in the log's translation run none (its translation) */
    int (*plain)(CPU *) = logmode == 1 ? NULL : d->fn;
    if (nents == AUDIT_MAX) {
        if (outf) fprintf(outf, "# more than %d rewrites: %08x and the rest are not audited\n", AUDIT_MAX, d->addr);
        if (logmode) LOG(LOG_ALWAYS, "rewrite: audit log: more than %d rewrites: %08x and the rest not recorded", AUDIT_MAX, d->addr);
        nents++;
    }
    if (nents > AUDIT_MAX)
        return plain;
    const char *only = port_getenv("AUDIT_ONLY"), *skip = port_getenv("AUDIT_SKIP");
    if (only && *only && !in_only(only, d->addr))
        return plain;
    int entry = d->addr == IMAGE_ENTRY;
    if (entry || in_only(skip, d->addr) || in_only(PORT_AUDIT_SKIP, d->addr)) {
        if (outf) fprintf(outf, "# %08x not audited (%s)\n", d->addr, entry ? "the program's entry point" : "AUDIT_SKIP");
        return plain;
    }
    Ent *e = &ents[nents];
    e->d = d;
    if (flags_out_table)
        for (const uint32_t *f = flags_out_table; f[0]; f += 2)
            if (f[0] == d->addr) {
                static const uint32_t bit[6] = {1u << 0, 1u << 2, 1u << 4, 1u << 6, 1u << 7, 1u << 11};
                for (int k = 0; k < 6; k++) if (f[1] & (1u << k)) e->fmask |= bit[k];
            }
    if (state == 1 && !logmode) {
        const char *tv = port_getenv("AUDIT_TAINT");
        e->taint = taint_all || (tv && *tv && in_only(tv, d->addr));
        if (e->taint) taint_items(e);
    }
    char name[32];
    snprintf(name, sizeof name, "F_%08x_orig", d->addr);
    e->orig = (void (*)(CPU *))dlsym(RTLD_DEFAULT, name);
    if (logmode && !e->orig) {
        static int said;
        if (!said++) LOG(LOG_ALWAYS, "rewrite: audit log: no F_%08x_orig by name (link -rdynamic): such functions are not recorded", d->addr);
        return plain;
    }
    if (outf && nents == 0)
        fprintf(outf, "# audit: %s\n", e->orig ? "the translation through F_<addr>_orig" :
                                                 "the translation through F_<addr> (no -rdynamic)");
    return tramps[nents++];
}
#endif
