/*
 * The CRT layer: real bodies for MSVCR71 imports.
 *
 * `client.dll` imports 182 functions from MSVCR71. Most are pure forwards to
 * the host CRT, and leaving them as stubs is not neutral -- a stub returns 0,
 * and 0 is a *valid-looking* answer for most of these. That is how the last
 * blocker presented: `strlen` faulted reading address 0, because path-parsing
 * code had called `strrchr` looking for the last backslash, got NULL from the
 * stub, and passed it straight on.
 *
 * Conventions, all of which fail silently if wrong:
 *
 *   cdecl      every function here. The caller cleans up, so STDRET(0).
 *   integers   returned in EAX via RET().
 *   doubles    returned in ST(0). The lifter models the x87 stack flat -- it
 *              only ever emits `_st[0]` -- so RETD() writes g_st[0].
 *   _CIxxx     the intrinsic math forms: argument already in ST(0), result
 *              left in ST(0). No stack arguments at all.
 *
 * Not here, deliberately: the printf family, qsort, setjmp/longjmp and the C++
 * exception machinery. Each needs more than a forward -- varargs off a
 * simulated stack, a callback that must re-enter lifted code, or MSVC's EH
 * protocol -- and guessing at them is worse than a stub that reports.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <direct.h>
#include <io.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#include <sys/utime.h>
#include <time.h>
#include <wchar.h>

#include "recomp_types.h"
#include "imports.h"

#define ARG(n)   MEM32(g_esp + 4u + 4u * (n))
#define ARGP(n)  ((void *)(uintptr_t)ARG(n))
#define ARGS(n)  ((char *)(uintptr_t)ARG(n))
#define ARGCS(n) ((const char *)(uintptr_t)ARG(n))
#define ARGW(n)  ((wchar_t *)(uintptr_t)ARG(n))
#define RETP(p)  RET((uint32_t)(uintptr_t)(p))

/* A double argument occupies two stack slots. `k` is the slot index, so
 * pow(double,double) reads ARGD(0) and ARGD(2). */
static double argd(unsigned slot)
{
    double d;
    memcpy(&d, (void *)(uintptr_t)(g_esp + 4u + 4u * slot), sizeof d);
    return d;
}
#define ARGD(k)  argd(k)
#define RETD(x)  do { g_st[0] = (x); } while (0)

/* ---- type_info ------------------------------------------------------ *
 * thiscall members of MSVCR71's type_info, reached from dynamic_cast and
 * catch matching. The generator sees an unknown calling convention and makes
 * them cdecl with no arguments; they pop one. A type_info is
 * { vfptr, _m_data, char _m_d_name[] }, and equality is name equality --
 * the client and the launcher each carry their own copies of the same types. */
static int ti_eq(void) { return strcmp((const char *)(uintptr_t)(g_ecx + 8u),
                                       (const char *)(uintptr_t)(ARG(0) + 8u)) == 0; }
void imp___8type_info__QBEHABV0__Z(void) { RET((uint32_t)ti_eq());  STDRET(1); }
void imp___9type_info__QBEHABV0__Z(void) { RET((uint32_t)!ti_eq()); STDRET(1); }

/* ---- strings ------------------------------------------------------- */

void imp_strchr(void)   { RETP(strchr(ARGCS(0), (int)ARG(1)));            STDRET(0); }
void imp_strrchr(void)  { RETP(strrchr(ARGCS(0), (int)ARG(1)));           STDRET(0); }
void imp_strstr(void)   { RETP(strstr(ARGCS(0), ARGCS(1)));               STDRET(0); }
void imp_strpbrk(void)  { RETP(strpbrk(ARGCS(0), ARGCS(1)));              STDRET(0); }
void imp_strtok(void)   { RETP(strtok(ARGS(0), ARGCS(1)));                STDRET(0); }
void imp_strncat(void)  { RETP(strncat(ARGS(0), ARGCS(1), ARG(2)));       STDRET(0); }
void imp_strncmp(void)  { RET((uint32_t)strncmp(ARGCS(0), ARGCS(1), ARG(2))); STDRET(0); }
void imp_strcspn(void)  { RET((uint32_t)strcspn(ARGCS(0), ARGCS(1)));     STDRET(0); }
void imp_strcoll(void)  { RET((uint32_t)strcoll(ARGCS(0), ARGCS(1)));     STDRET(0); }
void imp_strerror(void) { RETP(strerror((int)ARG(0)));                    STDRET(0); }
void imp__strdup(void)  { RETP(_strdup(ARGCS(0)));                        STDRET(0); }
void imp__stricmp(void) { RET((uint32_t)_stricmp(ARGCS(0), ARGCS(1)));    STDRET(0); }
void imp__strnicmp(void){ RET((uint32_t)_strnicmp(ARGCS(0), ARGCS(1), ARG(2))); STDRET(0); }
void imp__strupr(void)  { RETP(_strupr(ARGS(0)));                         STDRET(0); }
void imp_memchr(void)   { RETP(memchr(ARGP(0), (int)ARG(1), ARG(2)));     STDRET(0); }
void imp_strtod(void)   { RETD(strtod(ARGCS(0), (char **)ARGP(1)));       STDRET(0); }
void imp_strtoul(void)  { RET((uint32_t)strtoul(ARGCS(0), (char **)ARGP(1), (int)ARG(2))); STDRET(0); }

/* ---- wide strings -------------------------------------------------- */

void imp_wcslen(void)   { RET((uint32_t)wcslen(ARGW(0)));                 STDRET(0); }
void imp_wcscmp(void)   { RET((uint32_t)wcscmp(ARGW(0), ARGW(1)));        STDRET(0); }
void imp_wcsncpy(void)  { RETP(wcsncpy(ARGW(0), ARGW(1), ARG(2)));        STDRET(0); }
void imp_wcstoul(void)  { RET((uint32_t)wcstoul(ARGW(0), (wchar_t **)ARGP(1), (int)ARG(2))); STDRET(0); }
void imp__wcsicmp(void) { RET((uint32_t)_wcsicmp(ARGW(0), ARGW(1)));      STDRET(0); }
void imp__wcslwr(void)  { RETP(_wcslwr(ARGW(0)));                         STDRET(0); }
void imp__wcsupr(void)  { RETP(_wcsupr(ARGW(0)));                         STDRET(0); }
void imp_mbstowcs(void) { RET((uint32_t)mbstowcs(ARGW(0), ARGCS(1), ARG(2))); STDRET(0); }
void imp_wcstombs(void) { RET((uint32_t)wcstombs(ARGS(0), ARGW(1), ARG(2))); STDRET(0); }
void imp__itow(void)    { RETP(_itow((int)ARG(0), ARGW(1), (int)ARG(2))); STDRET(0); }
void imp__wtoi(void)    { RET((uint32_t)_wtoi(ARGW(0)));                  STDRET(0); }

/* ---- character classification -------------------------------------- */

#define CTYPE(fn) void imp_##fn(void) { RET((uint32_t)fn((int)ARG(0))); STDRET(0); }
CTYPE(isalnum) CTYPE(isalpha) CTYPE(iscntrl) CTYPE(isdigit) CTYPE(islower)
CTYPE(isprint) CTYPE(ispunct) CTYPE(isspace) CTYPE(isupper) CTYPE(isxdigit)
CTYPE(tolower) CTYPE(toupper)
#undef CTYPE

/* ---- math ---------------------------------------------------------- *
 * Results go to ST(0), not EAX. A stub leaving EAX=0 here does not merely
 * return a wrong number -- it leaves ST(0) holding whatever was there before,
 * so the error propagates into unrelated arithmetic.
 */
#define MATH1(fn) void imp_##fn(void) { RETD(fn(ARGD(0))); STDRET(0); }
#define MATH2(fn) void imp_##fn(void) { RETD(fn(ARGD(0), ARGD(2))); STDRET(0); }
MATH1(acos) MATH1(asin) MATH1(atan) MATH1(ceil) MATH1(cos) MATH1(exp)
MATH1(fabs) MATH1(floor) MATH1(log) MATH1(log10) MATH1(sin) MATH1(sqrt)
MATH1(tan)
MATH2(atan2) MATH2(fmod) MATH2(pow)
#undef MATH1
#undef MATH2

void imp_ldexp(void) { RETD(ldexp(ARGD(0), (int)ARG(2)));               STDRET(0); }
void imp_frexp(void) { RETD(frexp(ARGD(0), (int *)ARGP(2)));            STDRET(0); }
void imp__copysign(void) { RETD(_copysign(ARGD(0), ARGD(2)));           STDRET(0); }
void imp__finite(void)   { RET((uint32_t)_finite(ARGD(0)));             STDRET(0); }
void imp__isnan(void)    { RET((uint32_t)_isnan(ARGD(0)));              STDRET(0); }
void imp__fpclass(void)  { RET((uint32_t)_fpclass(ARGD(0)));            STDRET(0); }
void imp__controlfp(void){ RET((uint32_t)_controlfp(ARG(0), ARG(1)));   STDRET(0); }

/* The x87 intrinsic forms: argument already in ST(0), result left there, and
 * nothing on the stack to pop. */
#define CIMATH(name, fn) void imp_##name(void) { g_st[0] = fn(g_st[0]); STDRET(0); }
CIMATH(_CIacos, acos) CIMATH(_CIasin, asin) CIMATH(_CIcosh, cosh)
CIMATH(_CIexp, exp)   CIMATH(_CIsinh, sinh) CIMATH(_CItanh, tanh)
#undef CIMATH
void imp__CIpow(void)  { g_st[0] = pow(g_st[1], g_st[0]);  STDRET(0); }
void imp__CIfmod(void) { g_st[0] = fmod(g_st[1], g_st[0]); STDRET(0); }

/* _ftol truncates ST(0) into EAX:EDX and pops. */
void imp__ftol(void)
{
    long long v = (long long)g_st[0];
    g_eax = (uint32_t)(v & 0xFFFFFFFF);
    g_edx = (uint32_t)((unsigned long long)v >> 32);
    STDRET(0);
}

/* ---- conversion ---------------------------------------------------- */

void imp_atoi(void)  { RET((uint32_t)atoi(ARGCS(0)));  STDRET(0); }
void imp_atof(void)  { RETD(atof(ARGCS(0)));           STDRET(0); }
void imp__ecvt(void) { RETP(_ecvt(ARGD(0), (int)ARG(2), (int *)ARGP(3), (int *)ARGP(4))); STDRET(0); }
void imp__fcvt(void) { RETP(_fcvt(ARGD(0), (int)ARG(2), (int *)ARGP(3), (int *)ARGP(4))); STDRET(0); }

/* ---- stdio --------------------------------------------------------- */

void imp_ferror(void)  { RET((uint32_t)ferror((FILE *)ARGP(0)));                 STDRET(0); }
void imp_fflush(void)  { RET((uint32_t)fflush((FILE *)ARGP(0)));                 STDRET(0); }
void imp_fgets(void)   { RETP(fgets(ARGS(0), (int)ARG(1), (FILE *)ARGP(2)));     STDRET(0); }
void imp_fputc(void)   { RET((uint32_t)fputc((int)ARG(0), (FILE *)ARGP(1)));     STDRET(0); }
void imp_fputs(void)   { RET((uint32_t)fputs(ARGCS(0), (FILE *)ARGP(1)));        STDRET(0); }
void imp_getc(void)    { RET((uint32_t)getc((FILE *)ARGP(0)));                   STDRET(0); }
void imp_putc(void)    { RET((uint32_t)putc((int)ARG(0), (FILE *)ARGP(1)));      STDRET(0); }
void imp_puts(void)    { RET((uint32_t)puts(ARGCS(0)));                          STDRET(0); }
void imp_rewind(void)  { rewind((FILE *)ARGP(0)); RET(0);                        STDRET(0); }
void imp_setvbuf(void) { RET((uint32_t)setvbuf((FILE *)ARGP(0), ARGS(1), (int)ARG(2), ARG(3))); STDRET(0); }
void imp_ungetc(void)  { RET((uint32_t)ungetc((int)ARG(0), (FILE *)ARGP(1)));    STDRET(0); }
void imp_fgetpos(void) { RET((uint32_t)fgetpos((FILE *)ARGP(0), (fpos_t *)ARGP(1))); STDRET(0); }
void imp_fsetpos(void) { RET((uint32_t)fsetpos((FILE *)ARGP(0), (const fpos_t *)ARGP(1))); STDRET(0); }
void imp_remove(void)  { RET((uint32_t)remove(ARGCS(0)));                        STDRET(0); }
void imp_rename(void)  { RET((uint32_t)rename(ARGCS(0), ARGCS(1)));              STDRET(0); }
void imp_tmpfile(void) { RETP(tmpfile());                                        STDRET(0); }
void imp_tmpnam(void)  { RETP(tmpnam(ARGS(0)));                                  STDRET(0); }
void imp__itoa(void) { RETP(_itoa((int)ARG(0), ARGS(1), (int)ARG(2))); STDRET(0); }

void imp__fsopen(void)
{
    FILE *f = _fsopen(ARGCS(0), ARGCS(1), (int)ARG(2));
    if (getenv("MXO_FILE_TRACE")) printf("  [file] _fsopen(\"%s\", \"%s\") -> %p\n", ARGCS(0), ARGCS(1), (void *)f);
    RETP(f); STDRET(0);
}
void imp__wfopen(void) { RETP(_wfopen(ARGW(0), ARGW(1)));                        STDRET(0); }

/* ---- filesystem ---------------------------------------------------- */

void imp__access(void) { RET((uint32_t)_access(ARGCS(0), (int)ARG(1)));   STDRET(0); }
void imp__chdir(void)  { RET((uint32_t)_chdir(ARGCS(0)));                 STDRET(0); }
void imp__getcwd(void) { RETP(_getcwd(ARGS(0), (int)ARG(1)));             STDRET(0); }
void imp__findclose(void) { RET((uint32_t)_findclose((intptr_t)ARG(0)));  STDRET(0); }
void imp__splitpath(void)
{
    _splitpath(ARGCS(0), ARGS(1), ARGS(2), ARGS(3), ARGS(4));
    RET(0); STDRET(0);
}
void imp__get_osfhandle(void) { RET((uint32_t)_get_osfhandle((int)ARG(0))); STDRET(0); }

/* ---- time ---------------------------------------------------------- *
 * MSVCR71's time_t is 32 bits; the host CRT's is 64. Every call that passes a
 * time_t (or a struct holding one) by pointer must use the explicit 32-bit
 * form, or it writes past the client's variable -- _ftime's 16-byte __timeb64
 * into a 12-byte _timeb put dstflag on a saved EBP and faulted three frames up.
 */

void imp_time(void)     { RET((uint32_t)_time32((__time32_t *)ARGP(0)));         STDRET(0); }
void imp_clock(void)    { RET((uint32_t)clock());                         STDRET(0); }
void imp_localtime(void){ RETP(_localtime32((const __time32_t *)ARGP(0)));       STDRET(0); }
void imp_gmtime(void)   { RETP(_gmtime32((const __time32_t *)ARGP(0)));          STDRET(0); }
void imp_mktime(void)   { RET((uint32_t)_mktime32((struct tm *)ARGP(0)));    STDRET(0); }
void imp_difftime(void) { RETD(_difftime32((__time32_t)ARG(0), (__time32_t)ARG(1))); STDRET(0); }
void imp_strftime(void)
{
    RET((uint32_t)strftime(ARGS(0), ARG(1), ARGCS(2), (const struct tm *)ARGP(3)));
    STDRET(0);
}
void imp__strdate(void) { RETP(_strdate(ARGS(0)));                        STDRET(0); }
void imp__strtime(void) { RETP(_strtime(ARGS(0)));                        STDRET(0); }
void imp__ftime(void)   { _ftime32((struct __timeb32 *)ARGP(0)); RET(0);       STDRET(0); }

/* ---- misc ---------------------------------------------------------- */

void imp_getenv(void)    { RETP(getenv(ARGCS(0)));                        STDRET(0); }
void imp_rand(void)      { RET((uint32_t)rand());                         STDRET(0); }
void imp_srand(void)     { srand(ARG(0)); RET(0);                         STDRET(0); }
void imp_setlocale(void) { RETP(setlocale((int)ARG(0), ARGCS(1)));        STDRET(0); }
void imp__errno(void)    { RETP(_errno());                                STDRET(0); }
void imp__sleep(void)    { Sleep(ARG(0)); RET(0);                         STDRET(0); }
void imp__mkdir(void)    { RET((uint32_t)_mkdir(ARGCS(0)));               STDRET(0); }
void imp__heapchk(void)  { RET((uint32_t)_heapchk());                     STDRET(0); }

/* `exit` and `abort` must not return into lifted code -- there is no frame to
 * return to and the simulated stack would be left inconsistent. Report and
 * leave, so a run that ends this way says so plainly. */
void imp_exit(void)
{
    printf("\n[exit] the client called exit(%u)\n", ARG(0));
    fflush(stdout);
    exit((int)ARG(0));
}
void imp_abort(void)
{
    printf("\n[exit] the client called abort()\n");
    fflush(stdout);
    exit(3);
}
