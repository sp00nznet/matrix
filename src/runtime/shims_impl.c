/*
 * Real bodies for the imports that block bring-up.
 *
 * Everything gen_imports.py does not find here gets a stub that returns 0 and
 * reports its name. That is fine for most of the import surface and fatal for a
 * handful: the first run reached `malloc`, got 0 back, and faulted on the
 * dereference nine indirect calls into InitClientDLL. These are the ones worth
 * writing before anything else.
 *
 * Calling convention, since it is easy to get wrong and fails silently:
 *
 *   On entry the simulated stack holds the dummy return address at [esp], then
 *   arguments from [esp+4] upward. ARG(0) is the first argument. Each shim must
 *   finish with STDRET(k) -- k=0 for cdecl (the caller cleans up), k=argc for
 *   stdcall (we do). gen_imports.py knows which each import is and checks the
 *   count, so the value here has to match what it derived.
 *
 * Host pointers are used directly as guest addresses. This is a 32-bit process
 * with g_mem_base = 0, so a host malloc() result is already a valid VA for the
 * lifted code to dereference.
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
#define RETP(p)  RET((uint32_t)(uintptr_t)(p))

/* ---- CRT memory (cdecl: pop 0) ------------------------------------- */

void imp_malloc(void)  { RETP(malloc(ARG(0)));            STDRET(0); }
void imp_free(void)    { free(ARGP(0));          RET(0);  STDRET(0); }
void imp_calloc(void)  { RETP(calloc(ARG(0), ARG(1)));    STDRET(0); }
void imp_realloc(void) { RETP(realloc(ARGP(0), ARG(1)));  STDRET(0); }
void imp_memmove(void) { RETP(memmove(ARGP(0), ARGP(1), ARG(2))); STDRET(0); }

/* `_aligned_free` must pair with `_aligned_malloc`, never with plain free():
 * the aligned allocator stores its bookkeeping behind the returned pointer, so
 * freeing it the ordinary way corrupts the heap rather than failing. */
void imp__aligned_malloc(void) { RETP(_aligned_malloc(ARG(0), ARG(1))); STDRET(0); }
void imp__aligned_free(void)   { _aligned_free(ARGP(0)); RET(0);        STDRET(0); }

/* Not here on purpose: memset, memcpy, memcmp, strlen and operator new/delete
 * are NOT in this binary's import table. MSVC 7.1 emits them as intrinsics, so
 * they are already inside the lifted code. Writing shims for them would compile
 * and simply never be called -- which is why gen_imports.py fails on any
 * HAND_WRITTEN name it cannot find in the import table. */

/* ---- interlocked (stdcall: we pop) --------------------------------- */
/*
 * The real ones are atomic and so are these, which matters less than it looks
 * right now -- the lifted code is single-threaded until something calls
 * CreateThread -- but getting the *return value* right matters immediately.
 * InterlockedExchange returns the PREVIOUS value, and code that uses it as a
 * lock will spin forever or fall straight through if that is wrong.
 */
void imp_InterlockedExchange(void)
{
    LONG prev = InterlockedExchange((volatile LONG *)ARGP(0), (LONG)ARG(1));
    RET((uint32_t)prev); STDRET(2);
}

void imp_InterlockedExchangeAdd(void)
{
    LONG prev = InterlockedExchangeAdd((volatile LONG *)ARGP(0), (LONG)ARG(1));
    RET((uint32_t)prev); STDRET(2);
}

void imp_InterlockedIncrement(void)
{
    RET((uint32_t)InterlockedIncrement((volatile LONG *)ARGP(0))); STDRET(1);
}

void imp_InterlockedDecrement(void)
{
    RET((uint32_t)InterlockedDecrement((volatile LONG *)ARGP(0))); STDRET(1);
}

void imp_InterlockedCompareExchange(void)
{
    LONG prev = InterlockedCompareExchange((volatile LONG *)ARGP(0),
                                           (LONG)ARG(1), (LONG)ARG(2));
    RET((uint32_t)prev); STDRET(3);
}

/* ---- process and path identity (stdcall) --------------------------- *
 *
 * These are what startup asks for first, and a stub returning 0 is actively
 * harmful here rather than merely unhelpful: GetModuleFileNameA returning 0
 * leaves the caller with an empty path that it then parses.
 *
 * The paths are answered as the host's own, which is the honest answer -- the
 * recompiled client *is* this process. Where it matters later (the client
 * looking for its resource tree) this is the place to redirect.
 */
void imp_GetModuleFileNameA(void)
{
    RET((uint32_t)GetModuleFileNameA(NULL, (char *)ARGP(1), ARG(2)));
    STDRET(3);
}

void imp_GetCurrentDirectoryA(void)
{
    RET((uint32_t)GetCurrentDirectoryA(ARG(0), (char *)ARGP(1)));
    STDRET(2);
}

void imp_GetEnvironmentVariableA(void)
{
    RET((uint32_t)GetEnvironmentVariableA((const char *)ARGP(0),
                                          (char *)ARGP(1), ARG(2)));
    STDRET(3);
}

void imp_GetLastError(void)       { RET(GetLastError());                STDRET(0); }
void imp_SetLastError(void)       { SetLastError(ARG(0)); RET(0);       STDRET(1); }
void imp_GetCurrentProcess(void)  { RET(0xFFFFFFFFu);                   STDRET(0); }
void imp_GetCurrentProcessId(void){ RET(GetCurrentProcessId());         STDRET(0); }
void imp_GetCurrentThreadId(void) { RET(GetCurrentThreadId());          STDRET(0); }
void imp_GetTickCount(void)       { RET(GetTickCount());                STDRET(0); }

/* ---- CRT string and file (cdecl: pop 0) ---------------------------- */

void imp_strncpy(void)
{
    RETP(strncpy((char *)ARGP(0), (const char *)ARGP(1), ARG(2)));
    STDRET(0);
}

void imp_fopen(void)
{
    RETP(fopen((const char *)ARGP(0), (const char *)ARGP(1)));
    STDRET(0);
}

void imp_fclose(void) { RET((uint32_t)fclose((FILE *)ARGP(0)));                 STDRET(0); }
void imp_fread(void)  { RET((uint32_t)fread(ARGP(0), ARG(1), ARG(2), (FILE *)ARGP(3)));  STDRET(0); }
void imp_fwrite(void) { RET((uint32_t)fwrite(ARGP(0), ARG(1), ARG(2), (FILE *)ARGP(3))); STDRET(0); }
void imp_fseek(void)  { RET((uint32_t)fseek((FILE *)ARGP(0), (long)ARG(1), (int)ARG(2))); STDRET(0); }
void imp_ftell(void)  { RET((uint32_t)ftell((FILE *)ARGP(0)));                  STDRET(0); }
void imp_feof(void)   { RET((uint32_t)feof((FILE *)ARGP(0)));                   STDRET(0); }

/* ---- dbghelp ------------------------------------------------------- *
 *
 * The client wires up its own crash reporter during startup. We do not want
 * it symbolising anything -- a fault should surface through the host's VEH
 * with the lifted VA, not be swallowed by the game's handler -- so this
 * succeeds and does nothing.
 */
void imp_SymInitialize(void) { RET(1); STDRET(3); }
void imp_SymCleanup(void)    { RET(1); STDRET(1); }

/* ---- toolhelp: the client's crash reporter enumerating modules ------ *
 *
 * Startup builds a module map for its own crash handler. These forward to the
 * real API: the host is a 32-bit process with a 1:1 mapping, so the
 * MODULEENTRY32 the client hands us is directly usable as a host pointer.
 *
 * Forwarding rather than failing is the cheaper choice here. A stubbed
 * CreateToolhelp32Snapshot returns 0, and the client then calls Module32First
 * on a null handle rather than checking -- so "make it fail cleanly" would mean
 * emulating a failure path the client does not actually take.
 */
#include <tlhelp32.h>

void imp_CreateToolhelp32Snapshot(void)
{
    RET((uint32_t)(uintptr_t)CreateToolhelp32Snapshot(ARG(0), ARG(1)));
    STDRET(2);
}

void imp_Module32First(void)
{
    RET((uint32_t)Module32First((HANDLE)(uintptr_t)ARG(0), (LPMODULEENTRY32)ARGP(1)));
    STDRET(2);
}

void imp_Module32Next(void)
{
    RET((uint32_t)Module32Next((HANDLE)(uintptr_t)ARG(0), (LPMODULEENTRY32)ARGP(1)));
    STDRET(2);
}

void imp_CloseHandle(void)
{
    RET((uint32_t)CloseHandle((HANDLE)(uintptr_t)ARG(0)));
    STDRET(1);
}

void imp_SymGetOptions(void) { RET(0);           STDRET(0); }
void imp_SymSetOptions(void) { RET(ARG(0));      STDRET(1); }

/* `_msize` reports the usable size of a block from this heap. Our malloc is the
 * host's, so its answer is the right one -- and a stub returning 0 would tell
 * the client every allocation is empty. */
void imp__msize(void) { RET((uint32_t)_msize(ARGP(0))); STDRET(0); }

/* Crash-reporter symbol loading. Returning 0 means "no symbols for that
 * module", which is both true and a path the client already handles. */
void imp_SymLoadModule64(void) { RET(0); STDRET(6); }
