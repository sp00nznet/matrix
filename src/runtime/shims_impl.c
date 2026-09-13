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
