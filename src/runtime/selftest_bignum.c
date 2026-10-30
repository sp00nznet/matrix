/* selftest_bignum.c -- does the lift compute Crypto++'s bignum kernels right?
 *
 * The login's RSA check fails with byte-correct inputs, so the arithmetic
 * under it is suspect. These kernels (integer.cpp: Add, Subtract, the Comba
 * multipliers) are leaf functions -- no calls, no absolute addresses -- so
 * the real bytes in the mapped image can run natively. Run each both ways on
 * the same random inputs; any difference is a lifter bug.
 *
 *   mxo_final.exe client.dll --bignum
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "recomp_types.h"

typedef uint32_t (__cdecl *native4_t)(uint32_t, uint32_t, uint32_t, uint32_t);

/* Final client. Signatures are cdecl (R/C, A, B[, N]); the extra N is
 * harmless to the three-argument multipliers. */
static const struct { uint32_t va; const char *name; } k_kernels[] = {
    { 0x6255DD20u, "Add(C,A,B,N)" },
    { 0x6255DDD0u, "Subtract(C,A,B,N)" },
    { 0x6255DE70u, "kernel de70" },
    { 0x6255DF80u, "kernel df80" },
    { 0x6255E090u, "Multiply4" },
    { 0x6255E660u, "Multiply8" },
    { 0x6255F1E0u, "kernel f1e0" },
    { 0x6255F2B0u, "kernel f2b0" },
    /* Hash compression functions, (state, data): state starts as 0xCC fill. */
    { 0x62580240u, "SHA-256 Transform" },
    { 0x62535AD0u, "MD5 Transform" },
    { 0x6257F0A0u, "SHA-1 Transform" },
};

#define WORDS 64

static uint32_t lifted4(uint32_t va, const uint32_t a[4])
{
    recomp_func_t f = recomp_lookup(va);
    if (!f) return 0xBADBADu;
    for (int i = 3; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    g_esp += 16;
    return g_eax;
}

int bignum_selftest(void)
{
    static uint32_t A[WORDS], B[WORDS], Rn[WORDS * 2], Rl[WORDS * 2];
    int bad = 0;
    srand(1234);
    for (size_t k = 0; k < sizeof k_kernels / sizeof k_kernels[0]; k++) {
        int fails = 0;
        for (int t = 0; t < 2000; t++) {
            for (int i = 0; i < WORDS; i++) {
                /* Mix in all-ones words: carries chain through them. */
                uint32_t r = ((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 2) ^ (uint32_t)rand();
                A[i] = (t & 3) == 0 ? 0xFFFFFFFFu : r;
                B[i] = (t & 7) == 1 ? 0xFFFFFFFFu : ((uint32_t)rand() << 16) ^ (uint32_t)rand();
            }
            uint32_t n = 2u * (1u + (uint32_t)(t % 16));   /* Add/Subtract want even N */
            memset(Rn, 0xCC, sizeof Rn);
            memset(Rl, 0xCC, sizeof Rl);
            uint32_t an[4] = { (uint32_t)(uintptr_t)Rn, (uint32_t)(uintptr_t)A, (uint32_t)(uintptr_t)B, n };
            uint32_t al[4] = { (uint32_t)(uintptr_t)Rl, an[1], an[2], n };
            uint32_t en = ((native4_t)(uintptr_t)k_kernels[k].va)(an[0], an[1], an[2], an[3]);
            uint32_t el = lifted4(k_kernels[k].va, al);
            /* A void kernel leaves its output pointer in eax: each side's own. */
            if (en == an[0] && el == al[0]) en = el;
            if (en != el || memcmp(Rn, Rl, sizeof Rn)) {
                if (fails++ < 3) {
                    int w = 0;
                    while (w < WORDS * 2 && Rn[w] == Rl[w]) w++;
                    printf("  %-18s case %d n=%u: eax native %08X lifted %08X; first diff word %d (%08X vs %08X)\n",
                           k_kernels[k].name, t, n, en, el, w,
                           w < WORDS * 2 ? Rn[w] : 0, w < WORDS * 2 ? Rl[w] : 0);
                }
            }
        }
        printf("%-18s @%08X: %s (%d/2000 differ)\n", k_kernels[k].name, k_kernels[k].va,
               fails ? "DIFFERS" : "ok", fails);
        bad += fails != 0;
    }
    return bad;
}
