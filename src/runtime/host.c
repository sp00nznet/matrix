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


/* ---- the window the engine would have owned ------------------------ *
 *
 * client.dll imports no CreateWindowEx, no RegisterClass, no GetMessage and no
 * DefWindowProc -- only window manipulation. That is the LithTech split: the
 * engine host owns the window and the message pump and hands the client an
 * HWND. So the host creates one, and the user32 shims that take an HWND get a
 * real one to work on.
 */
static HWND g_hwnd;

HWND host_hwnd(void) { return g_hwnd; }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void make_window(int width, int height)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "MxORecompHost";
    RegisterClassA(&wc);

    RECT r = { 0, 0, width, height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExA(0, "MxORecompHost", "The Matrix Online (recompiled)",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
    if (g_hwnd) {
        ShowWindow(g_hwnd, SW_SHOW);
        UpdateWindow(g_hwnd);
    }
    printf("window: %p (%dx%d)\n", (void *)g_hwnd, width, height);
}

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

/* Consulted before the dispatch table and the import bridges.
 *
 * This is what the hook was for: engine interface vtable slots hold
 * synthetic VAs rather than real addresses, because a host function
 * pointer in a vtable is unreachable from lifted code -- the dispatch
 * table knows only lifted VAs and IAT slots, and a host function would
 * want its arguments on the host stack. */
recomp_func_t mslu_resolve(uint32_t va);   /* shims_mslu.c */
void mslu_install(void);

recomp_func_t native_resolve(uint32_t va); /* native_call.c */
static uint32_t g_image_size;

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    if (va - g_image_base < g_image_size) return NULL;   /* lifted: fast path */
    recomp_func_t f = mslu_resolve(va);
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

    /* The simulated registers, not the host's. For an access violation the
     * faulting address is almost always sitting in one of them, and which one
     * says which operand was wrong -- a null, a small integer used as a
     * pointer, and a plausible-looking address that is simply unmapped are
     * three different bugs. */
    fprintf(stderr, "    eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                    "    esp=%08X ebp=%08X esi=%08X edi=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
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
    report();
    fflush(stdout); fflush(stderr);
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
 * ponytail: the last four are 0 until something asks for them. The mediator is
 * the one that matters -- it is the launcher's half of login.
 */
static void push_init_args(void)
{
    static uint32_t argv0[1];                 /* empty, NULL-terminated argv */
    uint32_t a[8] = { 0, (uint32_t)(uintptr_t)argv0, g_image_base,
                      (uint32_t)(uintptr_t)g_cres, 0, 0, 0, 0 };
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

int main(int argc, char **argv)
{
    const char *path = "game/client.dll";
    const char *what = "--init";
    uint32_t manual_va = 0;
    int no_dllmain = 0;

    /* Unbuffered: a fail-fast (0xC0000409) otherwise takes the whole log with it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    _set_invalid_parameter_handler(invalid_param);

    for (int i = 1; i < argc; i++) {
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
    printf("stack: 0x%08X..0x%08X, esp=0x%08X\n",
           MXO_STACK_BASE, MXO_STACK_BASE + MXO_STACK_SIZE, g_esp);

    build_tib();
    mslu_install();   /* before DllMain: static constructors may convert strings */
    make_window(1024, 768);
    printf("TIB:   0x%08X (fs base)\n", g_fs_base);

    printf("dispatch entries:      %u\n", recomp_dispatch_count);
    printf("import bridges:        %u\n", mxo_import_bridge_count);
    printf("data imports:          %u\n", mxo_import_data_count);
    printf("IAT slots patched:     %u\n", patch_iat());

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
