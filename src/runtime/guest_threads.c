/*
 * Guest threads as fibers.
 *
 * Lifted code keeps the CPU in process globals (g_eax..g_edi, the x87 stack,
 * g_fs_base), so two OS threads running it would trample each other. The
 * client does start threads -- its UDP transport's Init (ctor 0x623C1E60)
 * spawns a receive thread through _beginthreadex, and without one
 * CNetMgr::Init fails and InitClientDLL returns -7.
 *
 * So every guest thread is a fiber on the one host thread, with its own
 * simulated stack, TIB and saved copy of those globals. Switching is
 * cooperative: a fiber runs until it reaches a blocking call -- Sleep,
 * WaitForSingleObject, select -- and those shims yield round-robin instead of
 * blocking the OS thread. Code that never blocks never yields; that is the
 * same contract a game loop written for one core already honours.
 *
 * TLS moves to FLS (fiber-local storage, native) so each guest thread keeps
 * its own slots. Critical sections stay real: all fibers share one OS thread,
 * so they never contend -- mutual exclusion holds as long as nobody yields
 * while holding one (see the ponytail note on EnterCriticalSection).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

#define ARG(n)   MEM32(g_esp + 4u + 4u * (n))
#define ARGP(n)  ((void *)(uintptr_t)ARG(n))
#define RETP(p)  RET((uint32_t)(uintptr_t)(p))

#define GT_MAX          32
#define GT_GUEST_STACK  (1u << 20)      /* default when the client asks for 0 */
#define GT_HOST_STACK   (8u << 20)      /* lifted C recurses deeply on the host stack */

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    double   st[8];
    int      fp_top;
    uint16_t fpu_cw;
    uint64_t mm[8];
    uint32_t fs_base, cur_func;
} cpu_t;

typedef struct {
    void    *fiber;
    cpu_t    cpu;
    uint32_t start, arg, tid;
    uint32_t stack_lo, stack_hi;
    HANDLE   done_event;            /* doubles as the thread handle the client holds */
    uint32_t exit_code;
    int      done, suspended;
} gthread_t;

static gthread_t g_threads[GT_MAX];
static int       g_nthreads, g_cur;

static void save(cpu_t *c)
{
    c->eax = g_eax; c->ecx = g_ecx; c->edx = g_edx; c->ebx = g_ebx;
    c->esp = g_esp; c->ebp = g_ebp; c->esi = g_esi; c->edi = g_edi;
    memcpy(c->st, g_st, sizeof c->st); c->fp_top = g_fp_top; c->fpu_cw = g_fpu_cw;
    memcpy(c->mm, g_mm, sizeof c->mm);
    c->fs_base = g_fs_base; c->cur_func = g_cur_func;
}

static void restore(const cpu_t *c)
{
    g_eax = c->eax; g_ecx = c->ecx; g_edx = c->edx; g_ebx = c->ebx;
    g_esp = c->esp; g_ebp = c->ebp; g_esi = c->esi; g_edi = c->edi;
    memcpy(g_st, c->st, sizeof c->st); g_fp_top = c->fp_top; g_fpu_cw = c->fpu_cw;
    memcpy(g_mm, c->mm, sizeof c->mm);
    g_fs_base = c->fs_base; g_cur_func = c->cur_func;
}

/* ---- scheduler -------------------------------------------------------- */

static int runnable(int i) { return !g_threads[i].done && !g_threads[i].suspended; }

int gt_others_runnable(void)
{
    for (int i = 0; i < g_nthreads; i++) if (i != g_cur && runnable(i)) return 1;
    return 0;
}

/* Run every other runnable fiber once (round-robin), then come back. */
void gt_yield(void)
{
    int self = g_cur, next = self;
    for (int k = 1; k <= g_nthreads; k++) {
        int i = (self + k) % g_nthreads;
        if (runnable(i)) { next = i; break; }
    }
    if (next == self) return;
    save(&g_threads[self].cpu);
    g_cur = next;
    SwitchToFiber(g_threads[next].fiber);
    /* whoever switched back to us set g_cur = self and saved their own state */
    restore(&g_threads[self].cpu);
}

/* Current guest thread's simulated stack, for native_call.c's TEB swap. */
void gt_stack_bounds(uint32_t *lo, uint32_t *hi)
{
    *lo = g_threads[g_cur].stack_lo;
    *hi = g_threads[g_cur].stack_hi;
}

static void finish(uint32_t code)
{
    gthread_t *t = &g_threads[g_cur];
    t->done = 1;
    t->exit_code = code;
    SetEvent(t->done_event);
    printf("  [threads] thread %d exited, code %u\n", g_cur, code);
    gt_yield();                     /* never scheduled again */
    printf("  [threads] finished fiber resumed\n");
    exit(1);
}

static void CALLBACK fiber_main(void *p)
{
    gthread_t *t = (gthread_t *)p;
    restore(&t->cpu);
    printf("  [threads] thread %d running (tid %X)\n", g_cur, t->tid);
    recomp_func_t f = recomp_lookup(t->start);
    if (!f) { printf("  [threads] start 0x%08X not lifted\n", t->start); finish(0); }
    PUSH32(g_esp, t->arg);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    finish(g_eax);
}

static uint32_t alloc_tib(uint32_t lo, uint32_t hi, uint32_t tid)
{
    uint32_t *t = (uint32_t *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    t[0x00 / 4] = 0xFFFFFFFFu;                       /* SEH chain: end */
    t[0x04 / 4] = hi;
    t[0x08 / 4] = lo;
    t[0x18 / 4] = (uint32_t)(uintptr_t)t;            /* self */
    t[0x24 / 4] = tid;
    return (uint32_t)(uintptr_t)t;
}

/* Call once, after the main thread's stack and TIB exist. */
void gt_init(uint32_t stack_lo, uint32_t stack_hi)
{
    gthread_t *t = &g_threads[0];
    t->fiber = ConvertThreadToFiber(NULL);
    t->tid = GetCurrentThreadId();
    t->stack_lo = stack_lo;
    t->stack_hi = stack_hi;
    t->done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_nthreads = 1;
    g_cur = 0;
}

static HANDLE spawn(uint32_t start, uint32_t arg, uint32_t stack, int suspended, uint32_t *tid_out)
{
    if (g_nthreads == GT_MAX) { printf("  [threads] GT_MAX reached\n"); exit(1); }
    gthread_t *t = &g_threads[g_nthreads];
    uint32_t size = stack ? (stack + 0xFFFFu) & ~0xFFFFu : GT_GUEST_STACK;
    uint8_t *mem = (uint8_t *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    memset(t, 0, sizeof *t);
    t->start = start;
    t->arg = arg;
    t->tid = 0x4000u + (uint32_t)g_nthreads;
    t->stack_lo = (uint32_t)(uintptr_t)mem;
    t->stack_hi = t->stack_lo + size;
    t->suspended = suspended;
    t->done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->cpu.esp = t->cpu.ebp = t->stack_hi - 0x100;
    t->cpu.fpu_cw = 0x027F;
    t->cpu.fs_base = alloc_tib(t->stack_lo, t->stack_hi, t->tid);
    t->fiber = CreateFiberEx(64 * 1024, GT_HOST_STACK, 0, fiber_main, t);
    if (!t->fiber || !mem) { printf("  [threads] fiber alloc failed\n"); exit(1); }
    if (tid_out) *tid_out = t->tid;
    printf("  [threads] thread %u: start 0x%08X arg 0x%08X%s\n",
           g_nthreads, start, arg, suspended ? " (suspended)" : "");
    g_nthreads++;
    return t->done_event;
}

static gthread_t *by_handle(uint32_t h)
{
    for (int i = 0; i < g_nthreads; i++)
        if ((uint32_t)(uintptr_t)g_threads[i].done_event == h) return &g_threads[i];
    return NULL;
}

/* ---- creation and lifecycle ------------------------------------------- */

/* _beginthreadex(security, stack, start, arg, initflag, &tid) -- cdecl */
void imp__beginthreadex(void)
{
    RETP(spawn(ARG(2), ARG(3), ARG(1), ARG(4) & CREATE_SUSPENDED, (uint32_t *)ARGP(5)));
    STDRET(0);
}

/* _beginthread(start, stack, arg) -- cdecl; the routine is cdecl void(void*),
 * which fiber_main calls the same way (the extra arg word is harmless). */
void imp__beginthread(void)
{
    RETP(spawn(ARG(0), ARG(2), ARG(1), 0, NULL));
    STDRET(0);
}

/* CreateThread(security, stack, start, arg, flags, &tid) -- stdcall */
void imp_CreateThread(void)
{
    RETP(spawn(ARG(2), ARG(3), ARG(1), ARG(4) & CREATE_SUSPENDED, (uint32_t *)ARGP(5)));
    STDRET(6);
}

void imp_ResumeThread(void)
{
    gthread_t *t = by_handle(ARG(0));
    uint32_t was = t ? (uint32_t)t->suspended : 0xFFFFFFFFu;
    if (t) t->suspended = 0;
    RET(was); STDRET(1);
}

void imp_SuspendThread(void)
{
    gthread_t *t = by_handle(ARG(0));
    uint32_t was = t ? (uint32_t)t->suspended : 0xFFFFFFFFu;
    if (t) t->suspended = 1;
    RET(was); STDRET(1);
    if (t == &g_threads[g_cur]) gt_yield();
}

void imp_ExitThread(void) { finish(ARG(0)); }

void imp_TerminateThread(void)
{
    gthread_t *t = by_handle(ARG(0));
    if (t && t != &g_threads[g_cur]) { t->done = 1; t->exit_code = ARG(1); SetEvent(t->done_event); }
    RET(t != NULL); STDRET(2);
}

void imp_GetExitCodeThread(void)
{
    gthread_t *t = by_handle(ARG(0));
    if (t && ARG(1)) MEM32(ARG(1)) = t->done ? t->exit_code : STILL_ACTIVE;
    RET(t != NULL); STDRET(2);
}

void imp_SetThreadPriority(void) { RET(1); STDRET(2); }
void imp_GetCurrentThreadId(void) { RET(g_threads[g_cur].tid); STDRET(0); }
void imp_GetCurrentThread(void)   { RETP(g_threads[g_cur].done_event); STDRET(0); }

/* ---- TLS -> FLS -------------------------------------------------------- */

void imp_TlsAlloc(void)    { RET(FlsAlloc(NULL));                             STDRET(0); }
void imp_TlsFree(void)     { RET((uint32_t)FlsFree(ARG(0)));                  STDRET(1); }
void imp_TlsGetValue(void) { RETP(FlsGetValue(ARG(0)));                       STDRET(1); }
void imp_TlsSetValue(void) { RET((uint32_t)FlsSetValue(ARG(0), ARGP(1)));     STDRET(2); }

/* ---- blocking points: yield instead of blocking the OS thread --------- */

/* ponytail: throttles with a 1 ms real wait per round while others run --
 * latency <= 1 ms per fiber hop. Upgrade to a wait-list scheduler if that
 * ever shows in a profile. */
static DWORD coop_wait(HANDLE h, DWORD ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        if (!gt_others_runnable()) {
            DWORD left = ms == INFINITE ? INFINITE : ms - min(ms, GetTickCount() - start);
            return WaitForSingleObject(h, left);
        }
        DWORD r = WaitForSingleObject(h, 0);
        if (r != WAIT_TIMEOUT) return r;
        if (ms != INFINITE && GetTickCount() - start >= ms) return WAIT_TIMEOUT;
        gt_yield();
        r = WaitForSingleObject(h, 1);
        if (r != WAIT_TIMEOUT) return r;
    }
}

void imp_WaitForSingleObject(void)
{
    HANDLE h = (HANDLE)ARGP(0);
    DWORD ms = ARG(1);
    DWORD r = coop_wait(h, ms);     /* gt_yield restores our whole CPU state */
    RET(r); STDRET(2);
}

void imp_Sleep(void)
{
    DWORD ms = ARG(0), start = GetTickCount();
    if (!gt_others_runnable()) Sleep(ms);
    else do { gt_yield(); if (ms) Sleep(1); } while (GetTickCount() - start < ms);
    RET(0); STDRET(1);
}

/* select() with a timeout: poll with a zero timeout and yield between polls.
 * The sets are in/out, so each poll works on copies of the caller's. */
int gt_select(int n, fd_set *r, fd_set *w, fd_set *e, const struct timeval *tv)
{
    DWORD ms = tv ? (DWORD)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : INFINITE;
    DWORD start = GetTickCount();
    fd_set r0, w0, e0;
    static const struct timeval zero = { 0, 0 };
    if (r) r0 = *r;
    if (w) w0 = *w;
    if (e) e0 = *e;
    for (;;) {
        if (r) *r = r0;
        if (w) *w = w0;
        if (e) *e = e0;
        int rc = select(n, r, w, e, gt_others_runnable() ? &zero : tv);
        if (rc != 0 || !gt_others_runnable()) return rc;
        if (ms != INFINITE && GetTickCount() - start >= ms) return 0;
        gt_yield();
        Sleep(1);
    }
}

/* RaiseException: the client's only use is MSVC's SetThreadName idiom
 * (0x406D1388, at 0x623719E8), which exists for a debugger and is caught by
 * the caller's own __except when none is attached -- so returning is exactly
 * what happens natively. Anything else would need real SEH dispatch through
 * lifted frames; it reports and stops rather than pretend. */
void imp_RaiseException(void)
{
    if (ARG(0) != 0x406D1388u) {
        printf("\n  [RaiseException] code %08X flags %X -- SEH dispatch not implemented\n", ARG(0), ARG(1));
        exit(1);
    }
    RET(0); STDRET(4);
}
