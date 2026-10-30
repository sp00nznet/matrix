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
#include "engine_iface.h"

/* ---- the register model the lifted code runs on ---- */
uint32_t  g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
double    g_st[8];
xmm_t     g_xmm[8];
uint32_t  g_mxcsr = 0x1F80;   /* reset value: all exceptions masked */
int       g_fp_top;
uint16_t  g_fpu_cw = 0x037F;
uint16_t  g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
uint64_t  g_mm[8];
uint32_t  g_fs_base, g_gs_base;
ptrdiff_t g_mem_base = 0;          /* 1:1 mapping, so no bias */

uint32_t  g_cur_func;
uint32_t  g_icall_trace[ICALL_TRACE_SIZE];
uint32_t  g_icall_from[ICALL_TRACE_SIZE];   /* caller of each traced ICALL (toolbox header) */
uint32_t  g_icall_trace_idx;
uint32_t  g_icall_count;

/* Read from the PE rather than hardcoded: there are two targets now. Retail
 * links at 0x10000000 and the final client at 0x62000000, and a stale constant
 * here would map the image somewhere the lifted code never looks -- which
 * presents as every global reading zero, not as a load failure. */
uint32_t g_image_base = 0x10000000u;   /* shims_mslu.c scans the mapped image */

static uint32_t pe_image_base(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t lfanew = 0, base = 0;
    if (fseek(f, 0x3C, SEEK_SET) == 0 && fread(&lfanew, 4, 1, f) == 1 &&
        fseek(f, (long)lfanew + 0x34, SEEK_SET) == 0)
        fread(&base, 4, 1, f);          /* OptionalHeader.ImageBase (PE32) */
    fclose(f);
    return base;
}

#define MXO_IMAGE_BASE g_image_base
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


/* ---- the window ------------------------------------------------------ *
 *
 * The client creates its own window, through its statically linked MSLU
 * (shims_mslu.c), which publishes it here for the user32 shims that need one.
 * Earlier notes said the host must own the window; that was wrong.
 */
static HWND g_hwnd;

HWND host_hwnd(void) { return g_hwnd; }
void host_set_hwnd(HWND h) { if (!g_hwnd) g_hwnd = h; }

/* Drain the queue so the window stays responsive while lifted code runs. */
static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}


/* ---- watch: dump guest memory at phase boundaries ------------------- *
 *
 * MXO_WATCH=0x10857788,0x108A54B8 dumps those addresses (and a short window
 * after each) once per phase. Built for P6: the question there is always
 * "is this engine-interface field still the null its constructor left, or did
 * something install a pointer", and that is a two-word answer that no amount
 * of reading lifted C will give as quickly as looking.
 */
static void watch_dump(const char *when)
{
    const char *spec = getenv("MXO_WATCH");
    if (!spec || !*spec) return;

    printf("  [watch @ %s]", when);
    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        /* "@0x..." dereferences first: heap objects move between runs, so a
         * literal address is only good for statics. */
        int deref = (*tok == '@');
        uint32_t va = (uint32_t)strtoul(deref ? tok + 1 : tok, NULL, 0);
        if (deref) {
            if (IsBadReadPtr((void *)(uintptr_t)va, 4)) { printf("  %08X:<unmapped>", va); continue; }
            uint32_t p = *(uint32_t *)(uintptr_t)va;
            printf("  [%08X]=%08X:", va, p);
            va = p;
        } else {
            printf("  %08X:", va);
        }
        /* Reading guest memory that may not be mapped: probe first rather than
         * taking the fault we are trying to diagnose. */
        if (IsBadReadPtr((void *)(uintptr_t)va, 16)) { printf(" <unmapped>"); continue; }
        int words = getenv("MXO_WATCH_WORDS") ? atoi(getenv("MXO_WATCH_WORDS")) : 4;
        for (int i = 0; i < words; i++)
            printf(" %08X", *(uint32_t *)(uintptr_t)(va + 4u * i));
    }
    printf("\n");
}

/* ---- dispatch ------------------------------------------------------- */

/* The generated table is address-sorted, so this is a binary search. With
 * 76,528 entries a linear scan would cost ~38k comparisons per indirect call,
 * and this binary makes a great many of them. */
recomp_func_t launcher_lookup(uint32_t va);          /* launcher_host.c */
recomp_func_t launcher_lookup_import(uint32_t va);
int launcher_owns(uint32_t va);
int launcher_login(const char *client_path, const char *spec, uint32_t *engine, uint32_t *mediator);

/* MXO_COUNT_VA=0x...,0x...: count indirect calls to those VAs, print at exit.
 * Answers "which of these does this run reach" without a traced build. */
static uint32_t g_image_size;
static uint32_t g_count_va[16], g_count_n[16], g_count_k = 0xFFFFFFFFu;

static void count_va_report(void)
{
    for (uint32_t i = 0; i < g_count_k; i++)
        printf("[count] %08X: %u indirect calls\n", g_count_va[i], g_count_n[i]);
}

recomp_func_t recomp_lookup(uint32_t va)
{
    if (g_count_k == 0xFFFFFFFFu) {
        const char *s = getenv("MXO_COUNT_VA");
        g_count_k = 0;
        while (s && *s && g_count_k < 16) {
            char *end;
            g_count_va[g_count_k++] = (uint32_t)strtoul(s, &end, 0);
            s = *end ? end + 1 : end;
        }
        if (g_count_k) atexit(count_va_report);
    }
    for (uint32_t i = 0; i < g_count_k; i++)
        if (g_count_va[i] == va) g_count_n[i]++;
    /* MXO_NEWCALLS=file: log each image VA the first time it is called
     * indirectly, with a timestamp -- what a new event (a packet, a key) reaches. */
    {
        static FILE *nf; static uint8_t *seen; static int init;
        if (!init) {
            const char *p = getenv("MXO_NEWCALLS");
            init = 1;
            if (p && (nf = fopen(p, "w")) != NULL) seen = calloc(1, (g_image_size >> 3) + 1);
        }
        uint32_t off = va - g_image_base;
        if (seen && off < g_image_size && !(seen[off >> 3] & (1u << (off & 7)))) {
            seen[off >> 3] |= (uint8_t)(1u << (off & 7));
            fprintf(nf, "%lu %08X\n", (unsigned long)GetTickCount(), va);
            fflush(nf);
        }
    }
    {
        extern recomp_func_t native_resolve_guest(uint32_t va);   /* native_call.c */
        recomp_func_t n = native_resolve_guest(va);
        if (n) return n;
    }

    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t a = recomp_dispatch_table[mid].address;
        if (a == va) return recomp_dispatch_table[mid].func;
        if (a < va)  lo = mid + 1;
        else         hi = mid;
    }
    return launcher_lookup(va);   /* the second guest module, when mapped */
}

/* Consulted before the dispatch table and the import bridges.
 *
 * This is what the hook was for: engine interface vtable slots hold
 * synthetic VAs rather than real addresses, because a host function
 * pointer in a vtable is unreachable from lifted code -- the dispatch
 * table knows only lifted VAs and IAT slots, and a host function would
 * want its arguments on the host stack. */
recomp_func_t mslu_resolve(uint32_t va);   /* shims_mslu.c */
void mslu_install(void);
void guest_call_install(uint32_t image_size);   /* guest_call.c */
void gt_init(uint32_t stack_lo, uint32_t stack_hi);   /* guest_threads.c */

recomp_func_t native_resolve(uint32_t va); /* native_call.c */
recomp_func_t mediator_resolve(uint32_t va);   /* login_mediator.c */
uint32_t mediator_object(void);

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    if (va - g_image_base < g_image_size) return NULL;   /* lifted: fast path */
    if (launcher_owns(va)) return NULL;
    recomp_func_t f = mslu_resolve(va);
    if (!f) f = mediator_resolve(va);
    if (!f) f = (recomp_func_t)mxo_iface_resolve(va);
    if (!f) f = native_resolve(va);
    return f;
}

/* ---- resource modules ------------------------------------------------ *
 *
 * The launcher hands InitClientDLL two module handles: client.dll's own (the
 * window's hInstance) and cres.dll's (strings, icons). Neither is a module
 * Windows loaded -- client.dll is mapped by hand, cres.dll not at all -- so the
 * host loads both as datafiles and the resource shims translate guest handles
 * through here. cres.dll's datafile handle is passed to the client as-is: it
 * only ever hands it back to a shim.
 */
static char    g_client_path[MAX_PATH];
static HMODULE g_client_res, g_cres;

static HMODULE load_datafile(const char *path)
{
    return LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
}

HMODULE mxo_resource_module(uint32_t hinst)
{
    if (hinst != g_image_base) return (HMODULE)(uintptr_t)hinst;
    if (!g_client_res) g_client_res = load_datafile(g_client_path);
    return g_client_res;
}

recomp_func_t recomp_lookup_import(uint32_t va)
{
    for (uint32_t i = 0; i < mxo_import_bridge_count; i++)
        if (mxo_import_bridges[i].address == va)
            return mxo_import_bridges[i].func;
    return launcher_lookup_import(va);
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

/* MXO_RUN_SECONDS=N: end the run after N seconds with the usual report.
 * RunClientDLL never returns, and a killed process prints no summary. Checked
 * from the clock-poll yield point (guest_threads.c), so it fires in the loop. */
void host_check_deadline(void)
{
    static DWORD deadline = 1;
    if (deadline == 1) {
        const char *s = getenv("MXO_RUN_SECONDS");
        deadline = s ? GetTickCount() + 1000u * (DWORD)atoi(s) : 0;
    }
    if (deadline && (int)(GetTickCount() - deadline) >= 0) {
        printf("\n[host] MXO_RUN_SECONDS reached\n");
        report();
        _flushall();
        ExitProcess(0);
    }
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

/* Bind whole DLLs natively: the IAT gets the real export addresses, exactly as
 * the Windows loader would, and every call then goes through native_call.c.
 *
 * For third-party DLLs that ship with the game (binkw32.dll) this beats a shim
 * per function, and it is the only thing that works when the client passes an
 * import *as a value*: BinkSetSoundSystem(BinkOpenDirectSound, ...) hands Bink
 * a function pointer read from the IAT, which Bink then calls natively -- a
 * self-patched slot would have it execute our IAT as code.
 *
 * pythonMXO.dll runs the game's own rules natively this way; its calls back
 * into the client (extension functions, type slots) arrive through
 * guest_call.c. MXO_NATIVE_DLLS overrides the list (comma-separated,
 * case-insensitive). */
static uint32_t bind_native_dlls(void)
{
    const char *list = getenv("MXO_NATIVE_DLLS");
    if (!list) list = "binkw32.dll,pythonMXO.dll,msacm32.dll";
    char dir[MAX_PATH], *slash;
    strcpy(dir, g_client_path);
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;

    uint8_t *img = (uint8_t *)(uintptr_t)g_image_base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    uint32_t bound = 0;

    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(img + dd.VirtualAddress); d->Name; d++) {
        const char *name = (const char *)(img + d->Name);
        char want[512];
        int hit = 0;
        snprintf(want, sizeof want, "%s", list);
        for (char *tok = strtok(want, ","); tok; tok = strtok(NULL, ","))
            if (!_stricmp(tok, name)) hit = 1;
        if (!hit) continue;

        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s%s", dir, name);
        HMODULE m = LoadLibraryA(path);
        if (!m) m = LoadLibraryA(name);   /* not shipped with the game: the system copy */
        if (!m) { printf("native DLL %s: load failed (%lu)\n", path, GetLastError()); continue; }

        uint32_t *names = (uint32_t *)(img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t *iat   = (uint32_t *)(img + d->FirstThunk);
        for (uint32_t i = 0; names[i]; i++) {
            FARPROC f = IMAGE_SNAP_BY_ORDINAL32(names[i])
                ? GetProcAddress(m, (LPCSTR)(uintptr_t)IMAGE_ORDINAL32(names[i]))
                : GetProcAddress(m, ((IMAGE_IMPORT_BY_NAME *)(img + names[i]))->Name);
            if (f) { iat[i] = (uint32_t)(uintptr_t)f; bound++; }
        }
        printf("native DLL %s: bound at %p\n", name, (void *)m);
    }
    return bound;
}

/* VirtualQuery, never IsBadReadPtr: that one probes by faulting, and this very
 * handler sees the fault first -- it recursed forever on a float-as-pointer. */
static int host_readable(uint32_t va)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)va, &mi, sizeof mi)) return 0;
    return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    fprintf(stderr, "\n[!] exception 0x%08lX at %p, in lifted 0x%08X\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress, g_cur_func);

    /* The simulated registers, not the host's. For an access violation the
     * faulting address is almost always sitting in one of them, and which one
     * says which operand was wrong -- a null, a small integer used as a
     * pointer, and a plausible-looking address that is simply unmapped are
     * three different bugs. */
    fprintf(stderr, "    eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                    "    esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    {
        /* First word behind each register that points somewhere readable --
         * usually a vtable, which names the object the fault happened in. */
        const char *nm[] = { "eax", "ecx", "edx", "ebx", "esi", "edi" };
        uint32_t rv[] = { g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi };
        for (int i = 0; i < 6; i++)
            if (rv[i] >= 0x10000u && host_readable(rv[i]))
                fprintf(stderr, "    [%s] = %08X\n", nm[i], rd32(rv[i]));
        /* MXO_DUMP_ESI=N: the first N words of the object in esi (usually `this`). */
        if (getenv("MXO_DUMP_ESI") && host_readable(g_esi)) {
            uint32_t n = (uint32_t)atoi(getenv("MXO_DUMP_ESI"));
            for (uint32_t w = 0; w < n && host_readable(g_esi + 4 * w); w++) {
                if (w % 8 == 0) fprintf(stderr, "\n    +%03X:", 4 * w);
                fprintf(stderr, " %08X", rd32(g_esi + 4 * w));
            }
            fprintf(stderr, "\n");
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        uint32_t bad = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (bad >= MXO_IFACE_VA_BASE &&
            bad < MXO_IFACE_VA_BASE + MXO_IFACE_STRIDE * 64u) {
            uint32_t off = bad - MXO_IFACE_VA_BASE;
            fprintf(stderr,
                "    ^ that is a SYNTHETIC vtable-slot address, dereferenced as\n"
                "      data (interface %u, slot %u). It means the object we handed\n"
                "      back is being read as a struct rather than called as an\n"
                "      interface -- so that slot's return type is wrong, not its\n"
                "      arity. Read the caller to see what it expects.\n",
                off / MXO_IFACE_STRIDE, (off % MXO_IFACE_STRIDE) / 16u);
        }
        fprintf(stderr, "    %s 0x%08X\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (uint32_t)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    watch_dump("fault");
    report();
#ifdef RECOMP_TRACE
    /* MXO_RING=N: the last N functions entered (the toolbox dump shows 40). */
    if (getenv("MXO_RING")) {
        uint32_t n = (uint32_t)atoi(getenv("MXO_RING"));
        if (n > RECOMP_ENTER_SIZE) n = RECOMP_ENTER_SIZE;
        fprintf(stderr, "  last %u functions entered (newest last):\n", n);
        for (uint32_t i = n; i >= 1; i--)
            fprintf(stderr, "%08X%c", g_enter_trace[(g_enter_idx - i) & (RECOMP_ENTER_SIZE - 1)],
                    (i - 1) % 8 ? ' ' : '\n');
    }
#endif
    _flushall();   /* includes the client's own ClientErrors.txt, opened via our fopen */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* InitClientDLL is cdecl with eight arguments, read from launcher.exe's call
 * (0x0040A57A): the launcher's filtered argc/argv, client.dll's module handle
 * (the window's hInstance), cres.dll's handle (strings), a launcher object kept
 * at 0x62B073E4, the launcher's ILTLoginMediator implementation (registered
 * under that name if non-NULL), a packed word and a flag byte.
 *
 * Earlier runs pushed none of them. Returns 1 on success, 0 on failure with the
 * reason in the buffer at 0x629DDE88 -- eax=0 is NOT LithTech's LT_OK here.
 *
 * The mediator is the stand-in in login_mediator.c: the launcher's half of
 * login. ponytail: args 4, 6 and 7 stay 0 until something asks for them.
 */
static uint32_t g_login_engine, g_login_mediator;   /* set by MXO_LOGIN */

static void push_init_args(void)
{
    static uint32_t argv0[1];                 /* empty, NULL-terminated argv */
    uint32_t a[8] = { 0, (uint32_t)(uintptr_t)argv0, g_image_base,
                      (uint32_t)(uintptr_t)g_cres, g_login_engine,
                      g_login_mediator ? g_login_mediator : mediator_object(), 0, 0 };
    for (int i = 7; i >= 0; i--) PUSH32(g_esp, a[i]);
}

/* The host CRT fail-fasts (0xC0000409, no VEH, no output) on an invalid
 * parameter -- which is what a forwarded CRT call does with the MSVCR71-era
 * arguments UCRT rejects. Report where lifted code was instead. */
static void invalid_param(const wchar_t *e, const wchar_t *f, const wchar_t *file,
                          unsigned line, uintptr_t r)
{
    (void)e; (void)f; (void)file; (void)line; (void)r;
    printf("\n[!] host CRT invalid parameter\n");
    report();
    exit(3);
}

/* MXO_WATCHDOG=N: a real OS thread that reports every N seconds where the
 * guest is. When the loop stops polling the clock there is no other way to
 * see it: the host's own stack is useless (lifted code) and the ring only
 * shows function entries. */
uint32_t native_current(void);   /* native_call.c */

static DWORD WINAPI watchdog(LPVOID p)
{
    DWORD ms = (DWORD)(uintptr_t)p;
    for (;;) {
        Sleep(ms);
        fprintf(stderr, "  [watchdog] cur_func=%08X esp=%08X icalls=%u native=%08X\n",
                g_cur_func, g_esp, g_icall_count, native_current());
    }
}

/* Windows can place a system DLL (apphelp.dll, seen at 0x626C0000) inside the
 * client's fixed range before main runs; DLL bases are chosen once per boot.
 * So the host restarts itself suspended, reserves the range while only ntdll
 * is mapped, and lets the loader commit into the reservation. */
static int run_with_range_reserved(int *rc)
{
    char exe[MAX_PATH];
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    if (GetEnvironmentVariableA("MXO_RANGE_HELD", NULL, 0)) return 0;  /* we are the child */
    SetEnvironmentVariableA("MXO_RANGE_HELD", "1");
    GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) return 0;
    /* ponytail: 16 MB covers client.dll's 0xBB4000 image; grow if the span does */
    if (!VirtualAllocEx(pi.hProcess, (void *)(uintptr_t)MXO_IMAGE_BASE, 0x1000000,
                        MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        fprintf(stderr, "[host] could not pre-reserve 0x%08X (%lu)\n", MXO_IMAGE_BASE, GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    *rc = (int)code;
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = "game/client.dll";
    const char *what = "--init";
    uint32_t manual_va = 0;
    int no_dllmain = 0;

    /* A CPU the lift can run: no SSE/SSE2 (or anything newer) and no 3DNow!.
     * Those instructions are UNIMPLEMENTED in the lift, and the client picks
     * code paths by CPUID -- Crypto++'s SSE2 multiply broke every RSA check
     * in the login. MMX stays: it is lifted. */
    g_cpuid_edx1 &= ~((1u << 25) | (1u << 26));       /* SSE, SSE2 */
    g_cpuid_ecx1 = 0;                                  /* SSE3 and later */
    g_cpuid_edx_ext &= ~((1u << 31) | (1u << 30));   /* 3DNow!, 3DNow!+ */
    /* And family 6: Crypto++ takes its SSE2 add/subtract on family 0xF alone,
     * which AMD hosts report. 0x686 = Pentium III, the client's own era. */
    g_cpuid_signature = 0x00000686u;

    /* Unbuffered: a fail-fast (0xC0000409) otherwise takes the whole log with it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    _set_invalid_parameter_handler(invalid_param);

    for (int i = 1; i < argc; i++) {
        extern int recomp_trace_arg(int argc, char **argv, int i);   /* recomp_trace.c */
        int took = recomp_trace_arg(argc, argv, i);
        if (took) { i += took - 1; continue; }
        if (!strcmp(argv[i], "--no-dllmain")) no_dllmain = 1;
        else if (!strcmp(argv[i], "--va") && i + 1 < argc)
            manual_va = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (argv[i][0] == '-') what = argv[i];
        else path = argv[i];
    }

    AddVectoredExceptionHandler(1, veh);

    uint32_t base = pe_image_base(path);
    if (!base) { fprintf(stderr, "cannot read image base from %s\n", path); return 1; }
    g_image_base = base;
    {
        int child_rc;
        if (run_with_range_reserved(&child_rc)) return child_rc;
    }
    if (getenv("MXO_WATCHDOG"))
        CreateThread(NULL, 0, watchdog, (LPVOID)(uintptr_t)(1000u * (DWORD)atoi(getenv("MXO_WATCHDOG"))), 0, NULL);
    GetFullPathNameA(path, MAX_PATH, g_client_path, NULL);
    {
        char cres[MAX_PATH], *slash;
        strcpy(cres, g_client_path);
        slash = strrchr(cres, '\\');
        strcpy(slash ? slash + 1 : cres, "cres.dll");
        g_cres = load_datafile(cres);
        printf("cres.dll: %p\n", (void *)g_cres);
    }

    uint32_t span = recomp_load_image(path, MXO_IMAGE_BASE);
    if (!span) { fprintf(stderr, "could not map %s at 0x%08X\n", path, MXO_IMAGE_BASE); return 1; }
    g_image_size = span;
    printf("mapped %s: 0x%08X + %u bytes\n", path, MXO_IMAGE_BASE, span);

    void *stk = VirtualAlloc((void *)(uintptr_t)MXO_STACK_BASE, MXO_STACK_SIZE,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stk) { fprintf(stderr, "stack alloc at 0x%08X failed\n", MXO_STACK_BASE); return 1; }
    g_esp = MXO_STACK_BASE + MXO_STACK_SIZE - 0x100;
    g_ebp = g_esp;
    if (!strcmp(what, "--bignum")) {
        extern int bignum_selftest(void);
        return bignum_selftest();
    }
    printf("stack: 0x%08X..0x%08X, esp=0x%08X\n",
           MXO_STACK_BASE, MXO_STACK_BASE + MXO_STACK_SIZE, g_esp);

    build_tib();
    gt_init(MXO_STACK_BASE, MXO_STACK_BASE + MXO_STACK_SIZE);   /* main = guest thread 0 */
    mslu_install();   /* before DllMain: static constructors may convert strings */
    guest_call_install(g_image_size);   /* native -> guest callbacks via DEP faults */
    /* No host window: the client creates its own through MSLU (see HOST.md). */
    printf("TIB:   0x%08X (fs base)\n", g_fs_base);

    printf("dispatch entries:      %u\n", recomp_dispatch_count);
    printf("import bridges:        %u\n", mxo_import_bridge_count);
    printf("data imports:          %u\n", mxo_import_data_count);
    printf("IAT slots patched:     %u\n", patch_iat());
    printf("IAT slots bound native: %u\n", bind_native_dlls());

    /* DllMain first, always, unless explicitly skipped.
     *
     * Calling InitClientDLL on its own looks reasonable and is wrong: the CRT
     * runs the C++ static constructors from DllMain via _initterm, so without
     * it every global a static constructor would have populated is still the
     * zero it was mapped as. That surfaced as a NULL string pointer four calls
     * deep, in code doing a perfectly ordinary lazy-init check on a global that
     * should already have been built.
     *
     * Signature is BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) -- stdcall,
     * three arguments, pushed right to left.
     */
    if (!no_dllmain) {
        recomp_func_t dm = recomp_lookup(mxo_dllmain_va);
        if (!dm) {
            fprintf(stderr, "no lifted body at DllMain 0x%08X\n", mxo_dllmain_va);
            return 1;
        }
        printf("\nDllMain(0x%08X, DLL_PROCESS_ATTACH, NULL) ...\n", MXO_IMAGE_BASE);
        PUSH32(g_esp, 0u);                  /* lpvReserved */
        PUSH32(g_esp, 1u);                  /* DLL_PROCESS_ATTACH */
        PUSH32(g_esp, MXO_IMAGE_BASE);      /* hinstDLL */
        PUSH32(g_esp, RECOMP_RETADDR);
        dm();
        watch_dump("after DllMain");
        mxo_install_interfaces();
        printf("DllMain returned eax=0x%08X, esp=0x%08X\n", g_eax, g_esp);
    }

    /* MXO_LOGIN=user:password: log in the way launcher.exe does, with its own
     * lifted TCP engine and the client's own mediator (launcher_host.c), and
     * hand both to InitClientDLL. Unset, the stand-in mediator is used. */
    if (getenv("MXO_LOGIN") && !no_dllmain) {
        launcher_login(g_client_path, getenv("MXO_LOGIN"), &g_login_engine, &g_login_mediator);
        pump();
    }

    /* --boot follows the LithTech contract: Init, then Run. Calling Run on its
     * own is not a shortcut -- InitClientDLL is what builds everything Run
     * then uses. */
    uint32_t seq[2] = {0, 0};
    if (manual_va)                     seq[0] = manual_va;
    else if (!strcmp(what, "--boot")) { seq[0] = mxo_va_InitClientDLL;
                                        seq[1] = mxo_va_RunClientDLL; }
    else if (!strcmp(what, "--run"))   seq[0] = mxo_va_RunClientDLL;
    else if (!strcmp(what, "--term"))  seq[0] = mxo_va_TermClientDLL;
    else                               seq[0] = mxo_va_InitClientDLL;

    for (int s = 0; s < 2 && seq[s]; s++) {
        recomp_func_t fn = recomp_lookup(seq[s]);
        if (!fn) { fprintf(stderr, "no lifted body at 0x%08X\n", seq[s]); return 1; }
        printf("\ncalling 0x%08X ...\n", seq[s]);
        if (seq[s] == mxo_va_InitClientDLL) push_init_args();
        PUSH32(g_esp, RECOMP_RETADDR);
        pump();
        watch_dump("before");
        fn();
        if (seq[s] == mxo_va_InitClientDLL) g_esp += 8u * 4u;   /* cdecl: caller pops */
        pump();
        watch_dump("after");
        printf("returned, eax=0x%08X  (esp=0x%08X, %u indirect calls)\n",
               g_eax, g_esp, g_icall_count);
    }

    report();
    return 0;
}
