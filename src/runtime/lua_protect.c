/* lua_protect.c -- Lua 5.0's error handling, in the host.
 *
 * The client's UI scripts run on Lua 5.0, which reports errors by longjmp:
 * luaD_rawrunprotected (0x6237B760) setjmps and calls f, luaD_throw
 * (0x6237B720) stores a status and longjmps back. A longjmp returns from
 * setjmp a second time -- in lifted C, a jump into the middle of a function
 * further up the host stack, which C cannot do.
 *
 * So the host implements rawrunprotected itself (run_lift.py --host-impl
 * 0x6237B760 renames the lifted body) around a real host setjmp, and the
 * longjmp shim unwinds to it. The guest-visible frame is the client's own
 * struct lua_longjmp, so luaD_throw and everything else reads it unchanged:
 *     +0x00 previous errorJmp   +0x04 jmp_buf (guest's; we key on its address)
 *     +0x44 status              L->errorJmp is at L+0x58
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "imports.h"

#define L_ERRORJMP 0x58u
#define ARG(n) MEM32(g_esp + 4u + 4u * (n))

static struct { uint32_t key; jmp_buf *hb; } g_frames[256];
static int g_depth;

void sub_6237B760(void)   /* int luaD_rawrunprotected(lua_State *L, Pfunc f, void *ud), cdecl */
{
    uint32_t L = ARG(0), f = ARG(1), ud = ARG(2);
    uint32_t esp0 = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp;
    uint32_t seh = MEM32(g_fs_base);          /* frames f pushes die with the longjmp */
    int fp_top = g_fp_top;
    uint32_t lj = (esp0 - 0x60u) & ~3u;       /* where the real function keeps it */
    volatile int depth = g_depth;
    jmp_buf hb;

    if (g_depth >= (int)(sizeof g_frames / sizeof g_frames[0])) {
        fprintf(stderr, "[lua] protected calls nested too deep\n");
        exit(1);
    }
    MEM32(lj) = MEM32(L + L_ERRORJMP);
    MEM32(lj + 0x44) = 0;
    MEM32(L + L_ERRORJMP) = lj;
    g_frames[g_depth].key = lj + 4;
    g_frames[g_depth].hb = &hb;
    g_depth++;

    if (!setjmp(hb)) {
        recomp_func_t fn = recomp_lookup(f);
        if (!fn) { fprintf(stderr, "[lua] no lifted body at 0x%08X\n", f); exit(1); }
        g_esp = lj - 0x10u;
        PUSH32(g_esp, ud);
        PUSH32(g_esp, L);
        PUSH32(g_esp, RECOMP_RETADDR);
        fn();
    }

    g_depth = depth;
    g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp;
    g_esp = esp0;
    MEM32(g_fs_base) = seh;
    g_fp_top = fp_top;
    MEM32(L + L_ERRORJMP) = MEM32(lj);
    RET(MEM32(lj + 0x44));
    STDRET(0);
}

/* longjmp(buf, 1): the newest host frame registered for that guest jmp_buf. */
int lua_longjmp(uint32_t buf)
{
    for (int i = g_depth - 1; i >= 0; i--)
        if (g_frames[i].key == buf) {
            g_depth = i + 1;
            longjmp(*g_frames[i].hb, 1);
        }
    return 0;
}
