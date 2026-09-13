/*
 * Host for the recompiled Matrix Online client (pipeline Phase 4).
 *
 * client.dll is a LithTech client DLL, so there is no WinMain to jump to. The
 * engine loads it and calls five exports; this host stands in for the engine
 * and calls InitClientDLL, which is the first thing the game's own code does.
 *
 *   mxo_host [--init | --run | --term | --va 0x........]
 *
 * What it has to do before any of that:
 *
 *   1. Map the real client.dll at its link base, 0x10000000, so every MEM*(va)
 *      in the lifted code reads the program's actual initial data. The lifted
 *      code is the .text; only the data sections matter here.
 *
 *   2. Give it a stack. The lifted code runs on a simulated one addressed by
 *      g_esp, not the host's.
 *
 *   3. Self-patch the IAT. A raw-mapped image's IAT slots hold name-table RVAs,
 *      not function pointers, so an indirect call through one goes nowhere.
 *      Writing each slot's own VA into itself turns "call [iat_slot]" into
 *      ICALL(iat_va), which recomp_lookup_import resolves to the shim.
 *
 *   4. Point the 16 data imports at real storage. Those slots are dereferenced,
 *      not called -- see gen_imports.py.
 *
 * Nothing here is expected to get far on the first run. Every unlifted target
 * and every unimplemented import reports its own address or name, so a run is a
 * worklist rather than a crash.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "recomp_funcs.h"   /* generated: the mxo_va_* export addresses */
#include "imports.h"

/* ---- the register model the lifted code runs on ---- */
uint32_t  g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double    g_st[8];
int       g_fp_top;
uint16_t  g_fpu_cw = 0x037F;
uint16_t  g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
uint64_t  g_mm[8];
uint32_t  g_fs_base, g_gs_base;
ptrdiff_t g_mem_base = 0;          /* 1:1 mapping, so no bias */

uint32_t  g_cur_func;
uint32_t  g_icall_trace[ICALL_TRACE_SIZE];
uint32_t  g_icall_trace_idx;
uint32_t  g_icall_count;

#define MXO_IMAGE_BASE 0x10000000u
#define MXO_STACK_BASE 0x0E000000u
#define MXO_STACK_SIZE 0x00400000u          /* 4 MB */
#define MXO_TIB_BASE   0x0E800000u
#define MXO_TIB_SIZE   0x00001000u

/* A simulated Thread Information Block, because MSVC's SEH prologue is the
 * very first thing the game's code does.
 *
 * InitClientDLL's first call lands in sub_10322960, which opens with the
 * standard `push -1 / push handler / mov eax, fs:[0] / push eax /
 * mov fs:[0], esp`. With g_fs_base left at 0 that reads address 0 and faults
 * before a single useful instruction has run. Nothing exotic is needed here --
 * the frame just has to be readable and writable, with the SEH chain
 * terminated.
 */
static void build_tib(void)
{
    void *tib = VirtualAlloc((void *)(uintptr_t)MXO_TIB_BASE, MXO_TIB_SIZE,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!tib) { fprintf(stderr, "TIB alloc failed\n"); exit(1); }
    memset(tib, 0, MXO_TIB_SIZE);
    g_fs_base = MXO_TIB_BASE;

    uint32_t *t = (uint32_t *)tib;
    t[0x00 / 4] = 0xFFFFFFFFu;                       /* SEH chain: end */
    t[0x04 / 4] = MXO_STACK_BASE + MXO_STACK_SIZE;   /* stack base (top) */
    t[0x08 / 4] = MXO_STACK_BASE;                    /* stack limit */
    t[0x18 / 4] = MXO_TIB_BASE;                      /* self pointer */
    t[0x24 / 4] = 1;                                 /* thread id */
    t[0x30 / 4] = 0;                                 /* PEB: none yet */
}

/* ---- dispatch ------------------------------------------------------- */

/* The generated table is address-sorted, so this is a binary search. With
 * 76,528 entries a linear scan would cost ~38k comparisons per indirect call,
 * and this binary makes a great many of them. */
recomp_func_t recomp_lookup(uint32_t va)
{
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t a = recomp_dispatch_table[mid].address;
        if (a == va) return recomp_dispatch_table[mid].func;
        if (a < va)  lo = mid + 1;
        else         hi = mid;
    }
    return NULL;
}

/* No overrides yet. This is where a hand-written replacement for a specific
 * lifted function goes when one turns out to need it. */
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

recomp_func_t recomp_lookup_import(uint32_t va)
{
    for (uint32_t i = 0; i < mxo_import_bridge_count; i++)
        if (mxo_import_bridges[i].address == va)
            return mxo_import_bridges[i].func;
    return NULL;
}

/* ---- reporting ------------------------------------------------------ */
/*
 * A first run produces thousands of these, and the point is the *set*, not the
 * stream. Both counters dedupe and print a ranked summary at exit, so the run
 * ends with a worklist instead of a scrollback.
 */
#define SEEN_MAX 4096
static uint32_t g_nl_va[SEEN_MAX];  static uint32_t g_nl_hits[SEEN_MAX];
static uint32_t g_nl_n;
static const char *g_is_name[SEEN_MAX]; static uint32_t g_is_hits[SEEN_MAX];
static uint32_t g_is_n;

void recomp_not_lifted(uint32_t va)
{
    for (uint32_t i = 0; i < g_nl_n; i++)
        if (g_nl_va[i] == va) { g_nl_hits[i]++; return; }
    if (g_nl_n < SEEN_MAX) { g_nl_va[g_nl_n] = va; g_nl_hits[g_nl_n++] = 1; }
}

void recomp_import_stub_hit(const char *name)
{
    for (uint32_t i = 0; i < g_is_n; i++)
        if (g_is_name[i] == name) { g_is_hits[i]++; return; }
    if (g_is_n < SEEN_MAX) { g_is_name[g_is_n] = name; g_is_hits[g_is_n++] = 1; }
}

static int by_hits_u32(const void *a, const void *b)
{
    uint32_t ia = *(const uint32_t *)a, ib = *(const uint32_t *)b;
    return (g_nl_hits[ib] > g_nl_hits[ia]) - (g_nl_hits[ia] > g_nl_hits[ib]);
}

static void report(void)
{
    printf("\n---- run summary ----\n");
    printf("  indirect calls:        %u\n", g_icall_count);
    printf("  distinct imports hit:  %u\n", g_is_n);
    printf("  distinct unlifted VAs: %u\n", g_nl_n);

    if (g_is_n) {
        printf("\n  imports that need a real body (top 20 by hits):\n");
        for (uint32_t i = 0; i < g_is_n && i < 20; i++)
            printf("    %6u  %s\n", g_is_hits[i], g_is_name[i]);
    }
    if (g_nl_n) {
        uint32_t idx[SEEN_MAX];
        for (uint32_t i = 0; i < g_nl_n; i++) idx[i] = i;
        qsort(idx, g_nl_n, sizeof idx[0], by_hits_u32);
        printf("\n  unlifted functions reached (top 20 by hits):\n");
        for (uint32_t i = 0; i < g_nl_n && i < 20; i++)
            printf("    %6u  0x%08X\n", g_nl_hits[idx[i]], g_nl_va[idx[i]]);
    }
    printf("\n  last %d indirect targets:\n   ", ICALL_TRACE_SIZE);
    for (int i = 0; i < ICALL_TRACE_SIZE; i++)
        printf(" %08X", g_icall_trace[(g_icall_trace_idx + i) & (ICALL_TRACE_SIZE - 1)]);
    printf("\n");

#ifdef RECOMP_TRACE
    /* The entry ring is the only backtrace available here: the lifted code runs
     * on a simulated stack, so the host's own call stack says nothing about
     * which game function called which. Newest last. */
    printf("\n  last 40 functions entered (oldest first):\n");
    uint32_t n = g_enter_idx < 40 ? g_enter_idx : 40;
    for (uint32_t i = n; i > 0; i--) {
        uint32_t va = g_enter_trace[(g_enter_idx - i) & (RECOMP_ENTER_SIZE - 1)];
        printf("    0x%08X%s", va, (i % 6) == 1 ? "\n" : "");
    }
    printf("\n");
#endif
}

/* ---- image + IAT ---------------------------------------------------- */

static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, (void *)(uintptr_t)va, 4); return v; }
static void     wr32(uint32_t va, uint32_t v) { memcpy((void *)(uintptr_t)va, &v, 4); }

extern uint32_t recomp_load_image(const char *path, uint32_t image_base);

/* Make every IAT slot resolvable: a slot holding its own VA turns
 * `call [slot]` into ICALL(slot_va), which the bridge table answers. */
static uint32_t patch_iat(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < mxo_import_bridge_count; i++) {
        wr32(mxo_import_bridges[i].address, mxo_import_bridges[i].address);
        n++;
    }
    /* Data slots get the address of their storage, because the client reads
     * through them rather than calling them. */
    for (uint32_t i = 0; i < mxo_import_data_count; i++) {
        wr32(mxo_import_data[i].iat_va, (uint32_t)(uintptr_t)mxo_import_data[i].storage);
        n++;
    }
    return n;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    fprintf(stderr, "\n[!] exception 0x%08lX at %p, in lifted 0x%08X\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress, g_cur_func);
    report();
    fflush(stdout); fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    const char *path = "game/client.dll";
    const char *what = "--init";
    uint32_t manual_va = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--va") && i + 1 < argc)
            manual_va = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (argv[i][0] == '-') what = argv[i];
        else path = argv[i];
    }

    AddVectoredExceptionHandler(1, veh);

    uint32_t span = recomp_load_image(path, MXO_IMAGE_BASE);
    if (!span) { fprintf(stderr, "could not map %s at 0x%08X\n", path, MXO_IMAGE_BASE); return 1; }
    printf("mapped %s: 0x%08X + %u bytes\n", path, MXO_IMAGE_BASE, span);

    void *stk = VirtualAlloc((void *)(uintptr_t)MXO_STACK_BASE, MXO_STACK_SIZE,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stk) { fprintf(stderr, "stack alloc at 0x%08X failed\n", MXO_STACK_BASE); return 1; }
    g_esp = MXO_STACK_BASE + MXO_STACK_SIZE - 0x100;
    g_ebp = g_esp;
    printf("stack: 0x%08X..0x%08X, esp=0x%08X\n",
           MXO_STACK_BASE, MXO_STACK_BASE + MXO_STACK_SIZE, g_esp);

    build_tib();
    printf("TIB:   0x%08X (fs base)\n", g_fs_base);

    printf("dispatch entries:      %u\n", recomp_dispatch_count);
    printf("import bridges:        %u\n", mxo_import_bridge_count);
    printf("data imports:          %u\n", mxo_import_data_count);
    printf("IAT slots patched:     %u\n", patch_iat());

    uint32_t va = manual_va ? manual_va :
                  !strcmp(what, "--run")  ? mxo_va_RunClientDLL  :
                  !strcmp(what, "--term") ? mxo_va_TermClientDLL :
                                            mxo_va_InitClientDLL;
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) { fprintf(stderr, "no lifted body at 0x%08X\n", va); return 1; }

    printf("\ncalling 0x%08X ...\n", va);
    PUSH32(g_esp, RECOMP_RETADDR);
    fn();
    printf("returned, eax=0x%08X\n", g_eax);

    report();
    return 0;
}
