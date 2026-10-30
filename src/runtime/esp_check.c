/* esp_check.c -- catch a lifted function that leaves the stack or a callee-saved
 * register different from how the calling convention says it must.
 *
 * Each CHECKED(va, pops) is listed in run_lift.py --host-impl, so its lifted
 * body is sub_<va>_lifted and this wrapper is what every caller reaches. After
 * the call, esp must be entry + 4 + 4*pops, and ebx/esi/edi/ebp unchanged.
 * Debugging aid: list a function here only while hunting a desync.
 */
#include <stdio.h>
#include "recomp_types.h"

static void report(uint32_t va, uint32_t esp0, uint32_t pops,
                   uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t ebp)
{
    static int n;
    if (n++ > 20) return;
    printf("[esp_check] 0x%08X: esp %+d (want +%u)%s%s%s%s\n", va,
           (int)(g_esp - esp0), 4 + 4 * pops,
           g_ebx != ebx ? " EBX" : "", g_esi != esi ? " ESI" : "",
           g_edi != edi ? " EDI" : "", g_ebp != ebp ? " EBP" : "");
}

#define CHECKED(VA, POPS)                                                       \
    void sub_##VA##_lifted(void);                                               \
    void sub_##VA(void)                                                         \
    {                                                                           \
        uint32_t esp0 = g_esp, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp; \
        sub_##VA##_lifted();                                                    \
        if (g_esp != esp0 + 4 + 4 * (POPS) || g_ebx != ebx || g_esi != esi ||  \
            g_edi != edi || g_ebp != ebp)                                       \
            report(0x##VA##u, esp0, POPS, ebx, esi, edi, ebp);                  \
    }

/* Example (each also needs run_lift.py --host-impl 0x<va>):
 * CHECKED(624DB1A0, 1)   -- found the 1-byte catalog entry (ida_to_catalog.py
 *                           merge_mid_instruction), 2026-10-30 */
