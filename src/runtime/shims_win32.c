/*
 * The Win32 layer: kernel32 / user32 / gdi32 / winmm, plus the printf family.
 *
 * Most of this is a forward. The host is a 32-bit process with g_mem_base = 0,
 * so a pointer the client hands us is already a valid host pointer, and a
 * HANDLE we get back is a valid guest value.
 *
 * One thing this file settles that is worth stating up front: **client.dll
 * never creates a window.** Its USER32 imports are all window *manipulation* --
 * ShowWindow, MoveWindow, SetWindowPos, GetClientRect, AdjustWindowRect,
 * ClipCursor -- with no CreateWindowEx, no RegisterClass, no GetMessage and no
 * DefWindowProc anywhere in the import table. That is the LithTech split: the
 * engine host owns the window and the message pump, and the client DLL is
 * handed an HWND. So the host has to be that engine, and `host_hwnd()` is where
 * the window it creates gets published.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

#define ARG(n)   MEM32(g_esp + 4u + 4u * (n))
#define ARGP(n)  ((void *)(uintptr_t)ARG(n))
#define ARGS(n)  ((char *)(uintptr_t)ARG(n))
#define ARGCS(n) ((const char *)(uintptr_t)ARG(n))
#define ARGH(n)  ((HANDLE)(uintptr_t)ARG(n))
#define ARGHW(n) ((HWND)(uintptr_t)ARG(n))
#define RETP(p)  RET((uint32_t)(uintptr_t)(p))

HWND host_hwnd(void);           /* the window the host created, or NULL */

/* ---- kernel32: synchronisation and TLS ----------------------------- *
 * Forwarded rather than faked. A CRITICAL_SECTION lives in the client's own
 * memory and the host can operate on it directly, and a stubbed
 * InitializeCriticalSection followed by a real Enter would deadlock or fault on
 * an uninitialised structure.
 */
void imp_InitializeCriticalSection(void) { InitializeCriticalSection((LPCRITICAL_SECTION)ARGP(0)); RET(0); STDRET(1); }
void imp_DeleteCriticalSection(void)     { DeleteCriticalSection((LPCRITICAL_SECTION)ARGP(0));     RET(0); STDRET(1); }
void imp_EnterCriticalSection(void)      { EnterCriticalSection((LPCRITICAL_SECTION)ARGP(0));      RET(0); STDRET(1); }
void imp_LeaveCriticalSection(void)      { LeaveCriticalSection((LPCRITICAL_SECTION)ARGP(0));      RET(0); STDRET(1); }

void imp_TlsAlloc(void)    { RET(TlsAlloc());                                  STDRET(0); }
void imp_TlsFree(void)     { RET((uint32_t)TlsFree(ARG(0)));                   STDRET(1); }
void imp_TlsGetValue(void) { RETP(TlsGetValue(ARG(0)));                        STDRET(1); }
void imp_TlsSetValue(void) { RET((uint32_t)TlsSetValue(ARG(0), ARGP(1)));      STDRET(2); }

void imp_CreateEventA(void)
{
    RETP(CreateEventA((LPSECURITY_ATTRIBUTES)ARGP(0), (BOOL)ARG(1),
                      (BOOL)ARG(2), ARGCS(3)));
    STDRET(4);
}
void imp_SetEvent(void)   { RET((uint32_t)SetEvent(ARGH(0)));   STDRET(1); }
void imp_WaitForSingleObject(void) { RET(WaitForSingleObject(ARGH(0), ARG(1))); STDRET(2); }
void imp_Sleep(void)      { Sleep(ARG(0)); RET(0);              STDRET(1); }

/* ---- kernel32: time and identity ----------------------------------- */

void imp_QueryPerformanceCounter(void)   { RET((uint32_t)QueryPerformanceCounter((LARGE_INTEGER *)ARGP(0)));   STDRET(1); }
void imp_QueryPerformanceFrequency(void) { RET((uint32_t)QueryPerformanceFrequency((LARGE_INTEGER *)ARGP(0))); STDRET(1); }
void imp_GetSystemTimeAsFileTime(void)   { GetSystemTimeAsFileTime((FILETIME *)ARGP(0)); RET(0);               STDRET(1); }
void imp_GetLocalTime(void)              { GetLocalTime((SYSTEMTIME *)ARGP(0)); RET(0);                        STDRET(1); }
void imp_GetFileType(void)               { RET(GetFileType(ARGH(0)));                                          STDRET(1); }
void imp_timeGetTime(void)               { RET(timeGetTime());                                                 STDRET(0); }

/* We want a fault to surface through the host's VEH with the lifted VA, not be
 * swallowed by the client's own handler. Returning 0 (no previous filter) is
 * the honest answer to a filter we decline to install. */
void imp_SetUnhandledExceptionFilter(void) { RET(0); STDRET(1); }

/* ---- kernel32: modules --------------------------------------------- *
 * LoadLibraryA is forwarded so the client gets a real handle for the system
 * DLLs it probes. What it must NOT do is hand back a host code address the
 * lifted code then calls: an ICALL to a host address resolves to nothing,
 * because the dispatch table only knows lifted VAs and IAT slots. If
 * GetProcAddress turns up on the worklist, that is the problem to solve, and
 * faking it here would hide it.
 */
void imp_LoadLibraryA(void)
{
    /* Decline, and say what was asked for.
     *
     * Forwarding this was a mistake worth recording. Handing back a real
     * HMODULE let the client walk that module's real PE export table and call
     * a real host address -- which arrives as ICALL(0x7610EC20) and resolves
     * to nothing, because the dispatch table only knows lifted VAs and IAT
     * slots. A host function reached that way would also be expecting its
     * arguments on the host stack, not the simulated one.
     *
     * Returning 0 is the honest answer while there is no bridge for
     * dynamically-resolved imports: the module genuinely is not available to
     * this client. The name goes to the log because the set of things it tries
     * to load is exactly the list of subsystems still to shim.
     */
    const char *name = ARGCS(0);
    printf("  [LoadLibraryA] declined: %s\n", name ? name : "(null)");
    RET(0); STDRET(1);
}
void imp_FreeLibrary(void)    { RET((uint32_t)FreeLibrary((HMODULE)ARGH(0))); STDRET(1); }
void imp_GetModuleHandleA(void)
{
    /* Same trap as LoadLibraryA, and the one that actually produced the
     * unresolved ICALL to 0x7610EC20: handing back a real host HMODULE lets
     * the client walk that module's real export table and call a real host
     * address. Only the client's own module has a meaningful answer here, and
     * that answer is the guest image base.
     */
    const char *name = ARGCS(0);
    if (!name) { RET(0x10000000u); STDRET(1); return; }
    printf("  [GetModuleHandleA] declined: %s\n", name);
    RET(0); STDRET(1);
}

/* ---- user32 -------------------------------------------------------- */

void imp_GetSystemMetrics(void)    { RET((uint32_t)GetSystemMetrics((int)ARG(0)));        STDRET(1); }
void imp_GetDesktopWindow(void)    { RETP(GetDesktopWindow());                            STDRET(0); }
void imp_GetForegroundWindow(void) { RETP(host_hwnd() ? host_hwnd() : GetForegroundWindow()); STDRET(0); }
void imp_GetDC(void)               { RETP(GetDC(ARGHW(0)));                               STDRET(1); }
void imp_ReleaseDC(void)           { RET((uint32_t)ReleaseDC(ARGHW(0), (HDC)ARGP(1)));    STDRET(2); }
void imp_GetClientRect(void)       { RET((uint32_t)GetClientRect(ARGHW(0), (RECT *)ARGP(1)));  STDRET(2); }
void imp_GetWindowRect(void)       { RET((uint32_t)GetWindowRect(ARGHW(0), (RECT *)ARGP(1)));  STDRET(2); }
void imp_AdjustWindowRect(void)    { RET((uint32_t)AdjustWindowRect((RECT *)ARGP(0), ARG(1), (BOOL)ARG(2))); STDRET(3); }
void imp_ClientToScreen(void)      { RET((uint32_t)ClientToScreen(ARGHW(0), (POINT *)ARGP(1))); STDRET(2); }
void imp_ScreenToClient(void)      { RET((uint32_t)ScreenToClient(ARGHW(0), (POINT *)ARGP(1))); STDRET(2); }
void imp_ShowWindow(void)          { RET((uint32_t)ShowWindow(ARGHW(0), (int)ARG(1)));    STDRET(2); }
void imp_UpdateWindow(void)        { RET((uint32_t)UpdateWindow(ARGHW(0)));               STDRET(1); }
void imp_MoveWindow(void)          { RET((uint32_t)MoveWindow(ARGHW(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (BOOL)ARG(5))); STDRET(6); }
void imp_SetWindowPos(void)        { RET((uint32_t)SetWindowPos(ARGHW(0), ARGHW(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), ARG(6))); STDRET(7); }
void imp_BringWindowToTop(void)    { RET((uint32_t)BringWindowToTop(ARGHW(0)));           STDRET(1); }
void imp_SetForegroundWindow(void) { RET((uint32_t)SetForegroundWindow(ARGHW(0)));        STDRET(1); }
void imp_SetFocus(void)            { RETP(SetFocus(ARGHW(0)));                            STDRET(1); }
void imp_IsIconic(void)            { RET((uint32_t)IsIconic(ARGHW(0)));                   STDRET(1); }
void imp_ValidateRect(void)        { RET((uint32_t)ValidateRect(ARGHW(0), (const RECT *)ARGP(1))); STDRET(2); }
void imp_PostQuitMessage(void)     { PostQuitMessage((int)ARG(0)); RET(0);                STDRET(1); }
void imp_TranslateMessage(void)    { RET((uint32_t)TranslateMessage((const MSG *)ARGP(0))); STDRET(1); }
void imp_MessageBoxA(void)
{
    /* Routed to the console instead of a modal box: a message box during
     * bring-up blocks an automated run forever, and the text is the point. */
    printf("\n[MessageBox] %s -- %s\n", ARGCS(2) ? ARGCS(2) : "(no title)",
           ARGCS(1) ? ARGCS(1) : "(no text)");
    RET(1); STDRET(4);
}

void imp_GetCursorPos(void)   { RET((uint32_t)GetCursorPos((POINT *)ARGP(0)));       STDRET(1); }
void imp_SetCursorPos(void)   { RET((uint32_t)SetCursorPos((int)ARG(0), (int)ARG(1))); STDRET(2); }
void imp_SetCursor(void)      { RETP(SetCursor((HCURSOR)ARGP(0)));                   STDRET(1); }
void imp_ShowCursor(void)     { RET((uint32_t)ShowCursor((BOOL)ARG(0)));             STDRET(1); }
void imp_DestroyCursor(void)  { RET((uint32_t)DestroyCursor((HCURSOR)ARGP(0)));      STDRET(1); }
void imp_ClipCursor(void)     { RET((uint32_t)ClipCursor((const RECT *)ARGP(0)));    STDRET(1); }
void imp_GetAsyncKeyState(void) { RET((uint32_t)(int32_t)GetAsyncKeyState((int)ARG(0))); STDRET(1); }
void imp_SystemParametersInfoA(void)
{
    RET((uint32_t)SystemParametersInfoA(ARG(0), ARG(1), ARGP(2), ARG(3)));
    STDRET(4);
}
void imp_EnumDisplaySettingsA(void)
{
    RET((uint32_t)EnumDisplaySettingsA(ARGCS(0), ARG(1), (DEVMODEA *)ARGP(2)));
    STDRET(3);
}
void imp_DrawTextA(void)
{
    RET((uint32_t)DrawTextA((HDC)ARGP(0), ARGCS(1), (int)ARG(2), (RECT *)ARGP(3), ARG(4)));
    STDRET(5);
}

/* ---- gdi32 --------------------------------------------------------- */

void imp_GetStockObject(void)         { RETP(GetStockObject((int)ARG(0)));                     STDRET(1); }
void imp_SelectObject(void)           { RETP(SelectObject((HDC)ARGP(0), (HGDIOBJ)ARGP(1)));    STDRET(2); }
void imp_DeleteObject(void)           { RET((uint32_t)DeleteObject((HGDIOBJ)ARGP(0)));         STDRET(1); }
void imp_DeleteDC(void)               { RET((uint32_t)DeleteDC((HDC)ARGP(0)));                 STDRET(1); }
void imp_CreateCompatibleDC(void)     { RETP(CreateCompatibleDC((HDC)ARGP(0)));                STDRET(1); }
void imp_CreateCompatibleBitmap(void) { RETP(CreateCompatibleBitmap((HDC)ARGP(0), (int)ARG(1), (int)ARG(2))); STDRET(3); }
void imp_SetBkColor(void)             { RET(SetBkColor((HDC)ARGP(0), ARG(1)));                 STDRET(2); }
void imp_SetBkMode(void)              { RET((uint32_t)SetBkMode((HDC)ARGP(0), (int)ARG(1)));   STDRET(2); }
void imp_SetTextColor(void)           { RET(SetTextColor((HDC)ARGP(0), ARG(1)));               STDRET(2); }
void imp_BitBlt(void)
{
    RET((uint32_t)BitBlt((HDC)ARGP(0), (int)ARG(1), (int)ARG(2), (int)ARG(3),
                         (int)ARG(4), (HDC)ARGP(5), (int)ARG(6), (int)ARG(7), ARG(8)));
    STDRET(9);
}
void imp_GetDIBits(void)
{
    RET((uint32_t)GetDIBits((HDC)ARGP(0), (HBITMAP)ARGP(1), ARG(2), ARG(3),
                            ARGP(4), (BITMAPINFO *)ARGP(5), ARG(6)));
    STDRET(7);
}
void imp_CreateFontA(void)
{
    RETP(CreateFontA((int)ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4),
                     ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10), ARG(11),
                     ARG(12), ARGCS(13)));
    STDRET(14);
}

/* ---- the printf family --------------------------------------------- *
 *
 * These are varargs off a *simulated* stack, so the host's own va_list cannot
 * be used: there is no way to hand the host CRT a pointer into g_esp and have
 * it walk correctly. So the format string is walked here and each conversion
 * pulls its own argument from the simulated stack.
 *
 * Each directive is reassembled and handed to the host's snprintf one at a
 * time. That keeps flags, width and precision working -- %-8.3f behaves --
 * without reimplementing float formatting, which is the part worth not getting
 * wrong.
 *
 * Doubles occupy two stack slots and are promoted from float by the caller, so
 * %f/%e/%g read eight bytes; everything else reads four.
 */
/* `ap` is the address of the first variadic argument: a stack slot for the
 * printf family, or the va_list itself for the v-forms. */
static int fmt_from_args(char *out, size_t cap, const char *fmt, uint32_t ap)
{
    size_t n = 0;
    char spec[64];

    #define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        if (p[1] == '%') { PUT('%'); p++; continue; }

        const char *start = p++;
        while (*p && strchr("-+ #0", *p)) p++;              /* flags */
        while (*p && (*p == '*' || (*p >= '0' && *p <= '9'))) p++;   /* width */
        if (*p == '.') { p++; while (*p && (*p == '*' || (*p >= '0' && *p <= '9'))) p++; }
        while (*p && strchr("hlLwI3264", *p)) p++;          /* length */
        if (!*p) { p--; continue; }

        size_t len = (size_t)(p - start) + 1;
        if (len >= sizeof spec) len = sizeof spec - 1;
        memcpy(spec, start, len);
        spec[len] = 0;

        char piece[512];
        int w = 0;
        char conv = *p;
        if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
            conv == 'g' || conv == 'G' || conv == 'a') {
            double d;
            memcpy(&d, (void *)(uintptr_t)ap, sizeof d);
            ap += 8;
            w = snprintf(piece, sizeof piece, spec, d);
        } else if (conv == 's') {
            const char *s = (const char *)(uintptr_t)MEM32(ap);
            ap += 4;
            w = snprintf(piece, sizeof piece, spec, s ? s : "(null)");
        } else if (conv == 'c') {
            uint32_t v = MEM32(ap); ap += 4;
            w = snprintf(piece, sizeof piece, spec, (int)v);
        } else {
            uint32_t v = MEM32(ap); ap += 4;
            w = snprintf(piece, sizeof piece, spec, v);
        }
        for (int i = 0; i < w && piece[i]; i++) PUT(piece[i]);
    }
    #undef PUT

    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
}

static int fmt_from_stack(char *out, size_t cap, const char *fmt, unsigned slot)
{
    return fmt_from_args(out, cap, fmt, g_esp + 4u + 4u * slot);
}

/* MSVCR71's counted forms return -1 when the output did not fit, and callers
 * test for exactly that (the client's own wrapper at 0x62005B40 terminates the
 * buffer on a negative result). Returning the untruncated length instead walks
 * the next write past the buffer. */
static uint32_t msvc_count(int n, uint32_t cap) { return (uint32_t)n > cap ? 0xFFFFFFFFu : (uint32_t)n; }

/* sprintf(buf, fmt, ...) -- args start at slot 2. */
void imp_sprintf(void)
{
    int n = fmt_from_stack(ARGS(0), 0x7FFFFFFF, ARGCS(1), 2);
    RET((uint32_t)n); STDRET(0);
}

/* _snprintf(buf, count, fmt, ...) -- args start at slot 3. */
void imp__snprintf(void)
{
    int n = fmt_from_stack(ARGS(0), ARG(1), ARGCS(2), 3);
    RET(msvc_count(n, ARG(1))); STDRET(0);
}

void imp_printf(void)
{
    char buf[4096];
    fmt_from_stack(buf, sizeof buf, ARGCS(0), 1);
    fputs(buf, stdout);
    RET((uint32_t)strlen(buf)); STDRET(0);
}

void imp_fprintf(void)
{
    char buf[4096];
    fmt_from_stack(buf, sizeof buf, ARGCS(1), 2);
    fputs(buf, (FILE *)ARGP(0));
    RET((uint32_t)strlen(buf)); STDRET(0);
}

/* ---- system queries, registry, version resources -------------------- *
 * Plain forwards. Stubbed, GetSystemInfo left the client's SYSTEM_INFO zeroed
 * -- zero processors -- and the fault surfaced later as a write through NULL.
 * The registry is read for real: absent MxO keys return an error the client
 * already handles, which is the truthful answer on this machine.
 */
void imp_GetSystemInfo(void)             { GetSystemInfo((SYSTEM_INFO *)ARGP(0)); RET(0);                 STDRET(1); }
void imp_IsProcessorFeaturePresent(void) { RET((uint32_t)IsProcessorFeaturePresent(ARG(0)));             STDRET(1); }
void imp_RegOpenKeyA(void)     { RET((uint32_t)RegOpenKeyA((HKEY)ARGP(0), ARGCS(1), (PHKEY)ARGP(2)));      STDRET(3); }
void imp_RegCloseKey(void)     { RET((uint32_t)RegCloseKey((HKEY)ARGP(0)));                               STDRET(1); }
void imp_RegQueryValueExA(void)
{
    RET((uint32_t)RegQueryValueExA((HKEY)ARGP(0), ARGCS(1), (LPDWORD)ARGP(2), (LPDWORD)ARGP(3),
                                   (LPBYTE)ARGP(4), (LPDWORD)ARGP(5)));
    STDRET(6);
}
void imp_GetFileVersionInfoSizeA(void) { RET(GetFileVersionInfoSizeA(ARGCS(0), (LPDWORD)ARGP(1)));        STDRET(2); }
void imp_GetFileVersionInfoA(void)     { RET((uint32_t)GetFileVersionInfoA(ARGCS(0), ARG(1), ARG(2), ARGP(3))); STDRET(4); }
void imp_VerQueryValueA(void)
{
    RET((uint32_t)VerQueryValueA(ARGP(0), ARGCS(1), (LPVOID *)ARGP(2), (PUINT)ARGP(3)));
    STDRET(4);
}

/* On x86 a va_list is just the address of the first argument word, so the
 * walker above takes it directly. Not forwarded to the host's _vsnprintf: UCRT
 * fail-fasts (0xC0000409) on directives MSVCR71 accepted. */
void imp__vsnprintf(void)
{
    RET(msvc_count(fmt_from_args(ARGS(0), ARG(1), ARGCS(2), ARG(3)), ARG(1)));
    STDRET(0);
}
