/*
 * The Microsoft Layer for Unicode (MSLU / unicows), statically linked into
 * client.dll -- and the window the client creates through it.
 *
 * client.dll *does* create its own window. It just never imports
 * CreateWindowExW: it calls through a function-pointer variable that MSLU's
 * loader fills on first use, by walking kernel32's or unicows.dll's export
 * table by hand. Our LoadLibraryA/GetModuleHandleA decline those modules (see
 * shims_win32.c), so every such variable resolved to MSLU's failure stub and
 * InitClientDLL returned 0 with "Could not create application window." -- which
 * earlier notes misread as success.
 *
 * Each stub has one fixed shape, so the variables are recovered by scanning:
 *
 *     push fallback ; push [x] ; push &ptrvar ; push "FuncW" ; push "dll" ; call loader
 *
 * mslu_install() pre-fills every ptrvar with a synthetic VA before any lifted
 * code runs, so the loader is never reached. A synthetic VA resolves (via
 * recomp_lookup_manual) to the shim below, or -- for names without one -- to a
 * reporter that names the function and stops, the same rule as engine_iface.c:
 * a guessed pop count shifts the caller's frame and crashes somewhere else.
 *
 * The window procedure is the one callback: Windows can only call a host
 * function, so RegisterClassExW gets `guest_wndproc`, which pushes the four
 * arguments onto the simulated stack and re-enters the lifted procedure.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

#define ARG(n)   MEM32(g_esp + 4u + 4u * (n))
#define ARGP(n)  ((void *)(uintptr_t)ARG(n))
#define ARGW(n)  ((LPWSTR)(uintptr_t)ARG(n))
#define ARGHW(n) ((HWND)(uintptr_t)ARG(n))
#define RETP(p)  RET((uint32_t)(uintptr_t)(p))

#define MSLU_VA_BASE  0x7D000000u   /* clear of image, stack, TIB and 0x7E iface VAs */
#define MSLU_STRIDE   16u
#define MSLU_MAX      64

extern uint32_t g_image_base;
HMODULE mxo_resource_module(uint32_t hinst);   /* host.c: guest hInstance -> datafile handle */

/* ---- window procedure bridge --------------------------------------- */

/* ponytail: one guest window class. A second RegisterClassExW with a different
 * procedure reports and stops; upgrade to a class-name -> VA map then. */
static uint32_t g_guest_wndproc;

static LRESULT CALLBACK guest_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    recomp_func_t f = recomp_lookup(g_guest_wndproc);
    if (!f) return DefWindowProcW(h, m, w, l);
    /* Lifted code keeps its registers in globals; a real callback would leave
     * the callee-saved ones intact because the procedure itself saves them, and
     * the caller-saved ones are fair game -- so only eax needs keeping for the
     * shim that was running when Windows called us. */
    uint32_t saved_eax = g_eax, saved_esp = g_esp;
    /* Re-entered from inside a native call (CreateDevice sends WM_* messages
     * synchronously), the host is itself running on the simulated stack just
     * below g_esp -- pushing there overwrites its frames and trips the /GS
     * cookie check (0xC0000409). Start the guest frame below the real ESP --
     * and well below: lifted C runs on the real stack too, and a DefWindowProcW
     * under it goes several KB deep. A 0x400 gap let user32's frames overwrite
     * the guest's saved EBX/ESI/EBP (seen on WM_STYLECHANGED during Release).
     * ponytail: fixed 256 KB per nesting level, ~15 levels in the 4 MB stack. */
    uint32_t here = (uint32_t)(uintptr_t)&saved_esp;
    if (here < g_esp && here > g_esp - 0x400000u) g_esp = (here - 0x40000u) & ~0xFu;
    PUSH32(g_esp, (uint32_t)l);
    PUSH32(g_esp, (uint32_t)w);
    PUSH32(g_esp, (uint32_t)m);
    PUSH32(g_esp, (uint32_t)(uintptr_t)h);
    PUSH32(g_esp, RECOMP_RETADDR);
    uint32_t b = g_ebx, si = g_esi, di = g_edi, bp = g_ebp, sp = g_esp;
    f();                                /* stdcall: pops its own 4 args */
    if (g_ebx != b || g_esi != si || g_edi != di || g_ebp != bp || g_esp != sp + 20)
        printf("  [wndproc] msg %04X clobbered: ebx %08X->%08X esi %08X->%08X edi %08X->%08X ebp %08X->%08X esp +%d\n",
               m, b, g_ebx, si, g_esi, di, g_edi, bp, g_ebp, (int)(g_esp - sp));
    LRESULT r = (LRESULT)g_eax;
    g_eax = saved_eax;
    g_esp = saved_esp;
    return r;
}

/* ---- shims ----------------------------------------------------------- *
 * Arity is the SDK's; all are stdcall. hInstance values the client passes are
 * guest values (its image base, or cres.dll's handle), so anything that reads
 * resources maps them through mxo_resource_module(), and window classes are
 * registered against the host exe so RegisterClassExW and CreateWindowExW agree.
 */
static void s_RegisterClassExW(void)
{
    WNDCLASSEXW wc = *(WNDCLASSEXW *)ARGP(0);
    uint32_t proc = (uint32_t)(uintptr_t)wc.lpfnWndProc;
    if (g_guest_wndproc && g_guest_wndproc != proc) {
        printf("  [MSLU] second window procedure 0x%08X (have 0x%08X) -- extend guest_wndproc\n",
               proc, g_guest_wndproc);
        exit(0);
    }
    g_guest_wndproc = proc;
    wc.lpfnWndProc = guest_wndproc;
    wc.hInstance   = GetModuleHandleW(NULL);
    RET(RegisterClassExW(&wc));
    STDRET(1);
}

static void s_CreateWindowExW(void)
{
    HWND h = CreateWindowExW(ARG(0), ARGW(1), ARGW(2), ARG(3), (int)ARG(4), (int)ARG(5),
                             (int)ARG(6), (int)ARG(7), ARGHW(8), (HMENU)ARGP(9),
                             GetModuleHandleW(NULL), ARGP(11));
    printf("  [MSLU] CreateWindowExW -> %p\n", (void *)h);
    RETP(h);
    STDRET(12);
}

static void s_LoadStringW(void)
{
    RET(LoadStringW(mxo_resource_module(ARG(0)), ARG(1), ARGW(2), (int)ARG(3)));
    STDRET(4);
}

static void s_LoadIconW(void)
{
    HICON i = ARG(0) ? LoadIconW(mxo_resource_module(ARG(0)), ARGW(1)) : LoadIconW(NULL, ARGW(1));
    RETP(i ? i : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION));
    STDRET(2);
}

static void s_LoadCursorW(void)
{
    HCURSOR c = ARG(0) ? LoadCursorW(mxo_resource_module(ARG(0)), ARGW(1)) : LoadCursorW(NULL, ARGW(1));
    RETP(c ? c : LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
    STDRET(2);
}

/* GWL_WNDPROC would hand the client a host address or hand Windows a guest one. */
static int wndproc_index(int i) { return i == GWLP_WNDPROC; }

static void s_GetWindowLongW(void)
{
    if (wndproc_index((int)ARG(1))) { printf("  [MSLU] GetWindowLongW(GWL_WNDPROC)\n"); exit(0); }
    RET(GetWindowLongW(ARGHW(0), (int)ARG(1))); STDRET(2);
}
static void s_SetWindowLongW(void)
{
    if (wndproc_index((int)ARG(1))) { printf("  [MSLU] SetWindowLongW(GWL_WNDPROC)\n"); exit(0); }
    RET(SetWindowLongW(ARGHW(0), (int)ARG(1), (LONG)ARG(2))); STDRET(3);
}
static void s_GetWindowLongA(void)
{
    if (wndproc_index((int)ARG(1))) { printf("  [MSLU] GetWindowLongA(GWL_WNDPROC)\n"); exit(0); }
    RET(GetWindowLongA(ARGHW(0), (int)ARG(1))); STDRET(2);
}
static void s_SetWindowLongA(void)
{
    if (wndproc_index((int)ARG(1))) { printf("  [MSLU] SetWindowLongA(GWL_WNDPROC)\n"); exit(0); }
    RET(SetWindowLongA(ARGHW(0), (int)ARG(1), (LONG)ARG(2))); STDRET(3);
}

static void s_DefWindowProcW(void)  { RET(DefWindowProcW(ARGHW(0), ARG(1), ARG(2), ARG(3))); STDRET(4); }
static void s_DispatchMessageW(void){ RET(DispatchMessageW((MSG *)ARGP(0)));                  STDRET(1); }
static void s_PeekMessageW(void)    { RET(PeekMessageW((MSG *)ARGP(0), ARGHW(1), ARG(2), ARG(3), ARG(4))); STDRET(5); }
static void s_IsWindowUnicode(void) { RET(IsWindowUnicode(ARGHW(0)));                         STDRET(1); }
static void s_EnableWindow(void)    { RET(EnableWindow(ARGHW(0), ARG(1)));                    STDRET(2); }

/* A modal box blocks an automated run; same policy as MessageBoxA. */
static void s_MessageBoxW(void)
{
    printf("  [MessageBoxW] %ls: %ls\n", ARG(2) ? ARGW(2) : L"", ARG(1) ? ARGW(1) : L"");
    RET(IDOK); STDRET(4);
}

/* Mirrors GetModuleFileNameA: the host exe stands in for every module. */
static void s_GetModuleFileNameW(void) { RET(GetModuleFileNameW(NULL, ARGW(1), ARG(2))); STDRET(3); }

static void s_GetVersionExW(void)          { RET(GetVersionExW((OSVERSIONINFOW *)ARGP(0))); STDRET(1); }
static void s_GetDiskFreeSpaceExW(void)    { RET(GetDiskFreeSpaceExW(ARGW(0), ARGP(1), ARGP(2), ARGP(3))); STDRET(4); }
static void s_EnumDisplaySettingsW(void)   { RET(EnumDisplaySettingsW(ARGW(0), ARG(1), (DEVMODEW *)ARGP(2))); STDRET(3); }
static void s_SystemParametersInfoW(void)  { RET(SystemParametersInfoW(ARG(0), ARG(1), ARGP(2), ARG(3))); STDRET(4); }
static void s_GetClipboardData(void)       { RETP(GetClipboardData(ARG(0)));                  STDRET(1); }
static void s_GetKeyNameTextW(void)        { RET(GetKeyNameTextW((LONG)ARG(0), ARGW(1), (int)ARG(2))); STDRET(3); }
static void s_MapVirtualKeyW(void)         { RET(MapVirtualKeyW(ARG(0), ARG(1)));             STDRET(2); }
static void s_WideCharToMultiByte(void)
{
    RET(WideCharToMultiByte(ARG(0), ARG(1), ARGW(2), (int)ARG(3), (LPSTR)ARGP(4), (int)ARG(5),
                            (LPCSTR)ARGP(6), (LPBOOL)ARGP(7)));
    STDRET(8);
}
static void s_MultiByteToWideChar(void)
{
    RET(MultiByteToWideChar(ARG(0), ARG(1), (LPCSTR)ARGP(2), (int)ARG(3), ARGW(4), (int)ARG(5)));
    STDRET(6);
}
static void s_GetTimeFormatW(void)   { RET(GetTimeFormatW(ARG(0), ARG(1), ARGP(2), ARGW(3), ARGW(4), (int)ARG(5))); STDRET(6); }
static void s_GetDateFormatW(void)   { RET(GetDateFormatW(ARG(0), ARG(1), ARGP(2), ARGW(3), ARGW(4), (int)ARG(5))); STDRET(6); }
static void s_GetNumberFormatW(void) { RET(GetNumberFormatW(ARG(0), ARG(1), ARGW(2), ARGP(3), ARGW(4), (int)ARG(5))); STDRET(6); }
static void s_GetStringTypeW(void)   { RET(GetStringTypeW(ARG(0), ARGW(1), (int)ARG(2), (LPWORD)ARGP(3))); STDRET(4); }
static void s_LCMapStringW(void)     { RET(LCMapStringW(ARG(0), ARG(1), ARGW(2), (int)ARG(3), ARGW(4), (int)ARG(5))); STDRET(6); }
static void s_CompareStringW(void)   { RET(CompareStringW(ARG(0), ARG(1), ARGW(2), (int)ARG(3), ARGW(4), (int)ARG(5))); STDRET(6); }
static void s_GetCPInfo(void)        { RET(GetCPInfo(ARG(0), (LPCPINFO)ARGP(1)));             STDRET(2); }
static void s_GetLocaleInfoW(void)   { RET(GetLocaleInfoW(ARG(0), ARG(1), ARGW(2), (int)ARG(3))); STDRET(4); }

/* A real module handle gives a real address, which native_call.c can now run.
 * Declined modules arrive as 0 (see GetModuleHandleA), and NULL is the answer
 * every caller so far checks for -- e.g. the GetLogicalProcessorInformation
 * probe at 0x62370393. */
static void s_GetProcAddress(void)
{
    RETP(ARG(0) ? GetProcAddress((HMODULE)ARGP(0), (LPCSTR)ARGP(1)) : NULL);
    STDRET(2);
}

/* Deliberately absent: wsprintfW (cdecl varargs off the simulated stack -- needs
 * the fmt_from_stack treatment) and CreateProcessW. They report by name if reached. */
static const struct { const char *name; void (*fn)(void); } k_shims[] = {
    { "RegisterClassExW", s_RegisterClassExW },   { "CreateWindowExW", s_CreateWindowExW },
    { "LoadStringW", s_LoadStringW },             { "LoadIconW", s_LoadIconW },
    { "LoadCursorW", s_LoadCursorW },             { "DefWindowProcW", s_DefWindowProcW },
    { "GetWindowLongW", s_GetWindowLongW },       { "SetWindowLongW", s_SetWindowLongW },
    { "GetWindowLongA", s_GetWindowLongA },       { "SetWindowLongA", s_SetWindowLongA },
    { "DispatchMessageW", s_DispatchMessageW },   { "PeekMessageW", s_PeekMessageW },
    { "IsWindowUnicode", s_IsWindowUnicode },     { "EnableWindow", s_EnableWindow },
    { "MessageBoxW", s_MessageBoxW },             { "GetModuleFileNameW", s_GetModuleFileNameW },
    { "GetVersionExW", s_GetVersionExW },         { "GetDiskFreeSpaceExW", s_GetDiskFreeSpaceExW },
    { "EnumDisplaySettingsW", s_EnumDisplaySettingsW }, { "SystemParametersInfoW", s_SystemParametersInfoW },
    { "GetClipboardData", s_GetClipboardData },   { "GetKeyNameTextW", s_GetKeyNameTextW },
    { "MapVirtualKeyW", s_MapVirtualKeyW },       { "WideCharToMultiByte", s_WideCharToMultiByte },
    { "MultiByteToWideChar", s_MultiByteToWideChar }, { "GetTimeFormatW", s_GetTimeFormatW },
    { "GetDateFormatW", s_GetDateFormatW },       { "GetNumberFormatW", s_GetNumberFormatW },
    { "GetStringTypeW", s_GetStringTypeW },       { "LCMapStringW", s_LCMapStringW },
    { "CompareStringW", s_CompareStringW },       { "GetCPInfo", s_GetCPInfo },
    { "GetLocaleInfoW", s_GetLocaleInfoW },
    { "GetProcAddress", s_GetProcAddress },
};

/* ---- install + resolve ---------------------------------------------- */

static struct { const char *name; void (*fn)(void); } g_slots[MSLU_MAX];
static uint32_t g_nslots;
static uint32_t g_pending;   /* same single-global argument as engine_iface.c */

static void unimplemented(void)
{
    printf("\n  [MSLU] %s reached with no shim -- write it (stack: %08X %08X %08X %08X)\n",
           g_slots[g_pending].name, ARG(0), ARG(1), ARG(2), ARG(3));
    exit(0);
}

static int in_image(uint32_t va, uint32_t size) { return va >= g_image_base && va < g_image_base + size; }

void mslu_install(void)
{
    uint8_t *img = (uint8_t *)(uintptr_t)g_image_base;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    uint32_t size = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER *text = IMAGE_FIRST_SECTION(nt);
    uint8_t *p = img + text->VirtualAddress, *end = p + text->Misc.VirtualSize - 26;
    uint32_t unshimmed = 0;

    for (; p < end && g_nslots < MSLU_MAX; p++) {
        /* 68 fb | FF 35 x | 68 ptr | 68 name | 68 dll | E8 */
        if (p[0] != 0x68 || p[5] != 0xFF || p[6] != 0x35 || p[11] != 0x68 ||
            p[16] != 0x68 || p[21] != 0x68 || p[26] != 0xE8) continue;
        uint32_t ptr = *(uint32_t *)(p + 12), nm = *(uint32_t *)(p + 17), dll = *(uint32_t *)(p + 22);
        if (!in_image(ptr, size) || !in_image(nm, size) || !in_image(dll, size)) continue;

        const char *name = (const char *)(uintptr_t)nm;
        void (*fn)(void) = unimplemented;
        for (size_t i = 0; i < sizeof k_shims / sizeof *k_shims; i++)
            if (!strcmp(k_shims[i].name, name)) fn = k_shims[i].fn;
        if (fn == unimplemented) unshimmed++;

        g_slots[g_nslots].name = name;
        g_slots[g_nslots].fn   = fn;
        MEM32(ptr) = MSLU_VA_BASE + g_nslots * MSLU_STRIDE;
        g_nslots++;
        p += 26;
    }
    printf("MSLU stubs bound:      %u (%u without a shim)\n", g_nslots, unshimmed);
}

recomp_func_t mslu_resolve(uint32_t va)
{
    uint32_t i = (va - MSLU_VA_BASE) / MSLU_STRIDE;
    if (va < MSLU_VA_BASE || i >= g_nslots) return NULL;
    g_pending = i;
    return g_slots[i].fn;
}
