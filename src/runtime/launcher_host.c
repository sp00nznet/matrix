/*
 * launcher_host.c -- launcher.exe's part of the job, done with launcher.exe's
 * own code.
 *
 * The launcher logs in before it loads client.dll, and hands InitClientDLL two
 * objects: its TCP engine (argument 4) and its ILTLoginMediator (argument 5).
 * Both classes are LithTech library code linked into both binaries, with one
 * exception that decides the design: client.dll carries the whole mediator,
 * login state machine and auth protocol included (CLTLoginMediator, vtable
 * 0x628CAE28, same 101 slots as the launcher's 0x004B01C8), but only the
 * *abstract* base of the engine. The concrete CLTThreadPerClientTCPEngine
 * exists only in launcher.exe.
 *
 * So the launcher is lifted as a second guest module (run_lift.py --module
 * launcher, rebased to 0x58000000 so it clears every other guest range), and
 * the host does what the launcher's -nopatch path does:
 *
 *   engine = new CLTThreadPerClientTCPEngine(0)     launcher 0x431C30, 0 = polled
 *   med->vt[7](&ver); med->vt[9](&ver)              launcher 0x409A64, "0.1"
 *   med->vt[2](engine)                              Init (launcher 0x40A3FE)
 *   med->vt[12](&credentials)                       Login: state 2, auth connect
 *   ... poll the engine until the state machine says logged in ...
 *   InitClientDLL(..., engine, med, ...)
 *
 * with med being the client's own mediator object, so every byte of auth --
 * RSA, Twofish, the world list -- is the client's own lifted code talking to
 * the server.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

#define LAUNCHER_BASE     0x58000000u
#define L(va)             ((va) - 0x00400000u + LAUNCHER_BASE)   /* launcher.exe VA -> guest */

/* Final client (base 0x62000000) addresses; see HOST.md. */
#define FINAL_BASE        0x62000000u
#define CLIENT_MEDIATOR   0x62B074E8u   /* static CLTLoginMediator, registered by 0x6281A960 */
#define CLIENT_ENGINE_POLL 0x62532130u  /* engine base: poll once when it owns no threads */
#define CLIENT_USE_LAUNCHPAD 0x62B090ACu /* storage of console variable UseLaunchPad */
#define CLIENT_MASTER_KEY  0x628D01F8u /* 2048-bit modulus, big-endian; e = 17 (loader 0x6254C5C0) */

/* The master public key the client checks the auth server's key against.
 * client.dll ships Monolith's; the community's launcher.exe carries its own
 * (they patched the binary that logs in). This is the public half of the
 * 2048-bit key in our server fork (hdneo/mxo-hd, MIT: hds/utils/encryption/
 * MxoRSA.cs, privKeyXML2), which signs the server key it sends in
 * AS_GetPublicKeyReply. */
static const uint8_t k_master_modulus[256] = {
    0xA5, 0xA1, 0x54, 0x68, 0x73, 0xB3, 0xA2, 0xE9, 0x3C, 0xB6, 0x5F, 0xBE, 0x6E, 0x7B, 0x9D, 0x63,
    0x03, 0xE7, 0xE3, 0x52, 0x71, 0xEB, 0x08, 0xE9, 0xFA, 0xCF, 0x11, 0x66, 0x67, 0xF8, 0x30, 0x2C,
    0x02, 0xBE, 0xBC, 0xAA, 0x05, 0x87, 0x26, 0x53, 0xF6, 0x50, 0x06, 0x06, 0x0F, 0x62, 0x34, 0xC3,
    0x8C, 0xDC, 0xCC, 0x69, 0x6D, 0xF9, 0xCD, 0xF4, 0xB0, 0x45, 0x88, 0x60, 0xA6, 0x94, 0x04, 0xC5,
    0x34, 0xF2, 0xC4, 0x45, 0xF6, 0x1C, 0xD5, 0xD0, 0x58, 0x67, 0x7A, 0xCC, 0xFC, 0xE8, 0x02, 0xED,
    0xD0, 0xD8, 0x92, 0x2C, 0x96, 0xB4, 0x91, 0x81, 0x7B, 0x87, 0x5E, 0x80, 0x2A, 0x7A, 0xB7, 0x2F,
    0x8D, 0xD8, 0xEA, 0xA5, 0x07, 0x0E, 0xC8, 0xB9, 0x17, 0x0E, 0x25, 0xAE, 0x2F, 0x34, 0x68, 0xD7,
    0xCA, 0x12, 0x62, 0xB3, 0xD4, 0x39, 0x19, 0xA9, 0x24, 0xEB, 0x12, 0xAC, 0xF7, 0xE3, 0x2A, 0xC4,
    0x72, 0x4E, 0x20, 0x79, 0x5C, 0xFA, 0x7F, 0x03, 0x3C, 0xB5, 0x9D, 0xEB, 0x1F, 0x79, 0x77, 0xE7,
    0x06, 0x2D, 0x67, 0x08, 0xBB, 0xDA, 0x06, 0x24, 0x35, 0xC6, 0x25, 0xC6, 0x21, 0x74, 0xA8, 0x5B,
    0x85, 0x43, 0xF4, 0x62, 0x05, 0xBE, 0xC4, 0x63, 0x65, 0xC6, 0x72, 0x53, 0xF7, 0x6A, 0x42, 0xB3,
    0x16, 0xB7, 0xCC, 0xA4, 0xC8, 0x33, 0x34, 0xF2, 0xAC, 0x6D, 0xD4, 0x0C, 0xCA, 0x0C, 0x2B, 0xA4,
    0xB1, 0x2E, 0xD7, 0x27, 0x1D, 0xD2, 0x04, 0x9E, 0x1A, 0x82, 0x0A, 0xA6, 0x87, 0x90, 0xCC, 0x0D,
    0xAC, 0xBE, 0x94, 0xA6, 0x20, 0x4C, 0x02, 0xAB, 0xA4, 0xE5, 0xC8, 0x8D, 0x72, 0x90, 0xF8, 0x86,
    0xF8, 0x54, 0xD5, 0xD0, 0x56, 0x28, 0xC2, 0xCD, 0x59, 0xD0, 0x52, 0x15, 0xB4, 0x6E, 0x67, 0x42,
    0x94, 0xD5, 0x73, 0xFB, 0x93, 0x13, 0x16, 0xD3, 0x0D, 0xF6, 0x65, 0xD4, 0xD9, 0x4E, 0xE8, 0xA5,
};

extern const recomp_dispatch_entry_t launcher_dispatch_table[];
extern const uint32_t                launcher_dispatch_count;
extern const recomp_dispatch_entry_t launcher_import_bridges[];
extern const uint32_t                launcher_import_bridge_count;
extern const recomp_import_data_t    launcher_import_data[];
extern const uint32_t                launcher_import_data_count;

extern uint32_t recomp_load_image(const char *path, uint32_t image_base);
extern uint32_t g_image_base;
void *guest_alloc(size_t n);
void gt_yield(void);   /* guest_threads.c */
void gt_dump(void);

static uint32_t g_span;

int launcher_owns(uint32_t va) { return va - LAUNCHER_BASE < g_span; }

recomp_func_t launcher_lookup(uint32_t va)
{
    if (!launcher_owns(va)) return NULL;
    uint32_t lo = 0, hi = launcher_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2, a = launcher_dispatch_table[mid].address;
        if (a == va) return launcher_dispatch_table[mid].func;
        if (a < va) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

recomp_func_t launcher_lookup_import(uint32_t va)
{
    if (!launcher_owns(va)) return NULL;
    for (uint32_t i = 0; i < launcher_import_bridge_count; i++)
        if (launcher_import_bridges[i].address == va) return launcher_import_bridges[i].func;
    return NULL;
}

/* Map launcher.exe from the game directory and apply its own base
 * relocations: the shipped file, not a pre-rebased copy, so the lift's input
 * (work/final/launcher_58.exe) and what runs can only differ in the base. */
static int load(const char *dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%slauncher.exe", dir);
    g_span = recomp_load_image(path, LAUNCHER_BASE);
    if (!g_span) return 0;

    uint8_t *img = (uint8_t *)(uintptr_t)LAUNCHER_BASE;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(img + ((IMAGE_DOS_HEADER *)img)->e_lfanew);
    uint32_t delta = LAUNCHER_BASE - nt->OptionalHeader.ImageBase;
    IMAGE_DATA_DIRECTORY rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    uint32_t n = 0;
    for (uint32_t off = 0; off + 8 <= rd.Size;) {
        IMAGE_BASE_RELOCATION *b = (IMAGE_BASE_RELOCATION *)(img + rd.VirtualAddress + off);
        if (!b->SizeOfBlock) break;
        uint16_t *e = (uint16_t *)(b + 1);
        for (uint32_t i = 0; i < (b->SizeOfBlock - 8) / 2; i++)
            if ((e[i] >> 12) == IMAGE_REL_BASED_HIGHLOW) {
                *(uint32_t *)(img + b->VirtualAddress + (e[i] & 0xFFF)) += delta;
                n++;
            }
        off += b->SizeOfBlock;
    }
    nt->OptionalHeader.ImageBase = LAUNCHER_BASE;

    /* IAT exactly as for the client: each slot holds its own VA, data slots
     * their storage. */
    for (uint32_t i = 0; i < launcher_import_bridge_count; i++)
        MEM32(launcher_import_bridges[i].address) = launcher_import_bridges[i].address;
    for (uint32_t i = 0; i < launcher_import_data_count; i++)
        MEM32(launcher_import_data[i].iat_va) = (uint32_t)(uintptr_t)launcher_import_data[i].storage;

    printf("launcher.exe: mapped at 0x%08X + %u, %u relocations, %u dispatch entries\n",
           LAUNCHER_BASE, g_span, n, launcher_dispatch_count);
    return 1;
}

/* Call guest code from the host: args pushed right to left, `this` in ECX.
 * A thiscall/stdcall callee pops its own arguments; for cdecl we do. */
static uint32_t gcall(uint32_t va, uint32_t self, int n, const uint32_t *a, int caller_pops)
{
    recomp_func_t f = recomp_lookup(va);
    if (!f) { printf("[launcher] no lifted body at 0x%08X\n", va); exit(1); }
    for (int i = n - 1; i >= 0; i--) PUSH32(g_esp, a[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    g_ecx = self;
    f();
    if (caller_pops) g_esp += 4u * (uint32_t)n;
    return g_eax;
}

static uint32_t vcall(uint32_t obj, uint32_t slot, int n, const uint32_t *a)
{
    return gcall(MEM32(MEM32(obj) + 4u * slot), obj, n, a, 0);
}

static uint32_t login_state(void)
{
    uint32_t st = MEM32(CLIENT_MEDIATOR + 0x10);
    return st ? vcall(st, 6, 0, NULL) : 0xFFFFFFFFu;
}

static uint32_t g_engine;

/* MXO_LOGIN=user:password. Returns 1 once the mediator reports a logged-in
 * state (3 or above: the accessors that read characters and worlds gate on it). */
int launcher_login(const char *client_path, const char *spec, uint32_t *engine, uint32_t *mediator)
{
    if (g_image_base != FINAL_BASE) { printf("[launcher] only the final client is mapped\n"); return 0; }

    char dir[MAX_PATH], *slash;
    snprintf(dir, sizeof dir, "%s", client_path);
    slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0; else dir[0] = 0;
    if (!load(dir)) return 0;

    char user[32] = {0}, pass[64] = {0};
    const char *colon = strchr(spec, ':');
    if (!colon) { printf("[launcher] MXO_LOGIN wants user:password\n"); return 0; }
    snprintf(user, sizeof user, "%.*s", (int)(colon - spec), spec);
    snprintf(pass, sizeof pass, "%s", colon + 1);

    g_engine = (uint32_t)(uintptr_t)guest_alloc(0xB4);
    uint32_t zero = 0;
    gcall(L(0x431C30), g_engine, 1, &zero, 0);
    printf("[launcher] engine %08X (vtable %08X)\n", g_engine, MEM32(g_engine));

    /* -nopatch: both version words come from the string "0.1". */
    char *ver = (char *)guest_alloc(4);
    memcpy(ver, "0.1", 4);
    uint32_t vs = (uint32_t)(uintptr_t)ver;
    uint32_t *pv = (uint32_t *)guest_alloc(8);
    pv[0] = gcall(L(0x417440), 0, 1, &vs, 1);
    pv[1] = pv[0];
    uint32_t a0 = (uint32_t)(uintptr_t)&pv[0], a1 = (uint32_t)(uintptr_t)&pv[1];
    vcall(CLIENT_MEDIATOR, 7, 1, &a0);
    vcall(CLIENT_MEDIATOR, 9, 1, &a1);

    uint32_t r = vcall(CLIENT_MEDIATOR, 2, 1, &g_engine);
    printf("[launcher] mediator Init -> %08X, state %u\n", r, login_state());
    if (getenv("MXO_LOGIN_TRACE"))
        for (uint32_t i = 0; i < 24; i++) {
            uint32_t st = MEM32(0x62B08D40u + 4u * i);
            if (st) printf("    state[%2u] = %08X vt %08X id %u\n", i, st, MEM32(st), vcall(st, 6, 0, NULL));
        }

    /* UseLaunchPad (console variable, default 1) routes Login through SOE's
     * LaunchPad API (states 15-19) instead of the auth server. The server we
     * talk to speaks the auth protocol. autoexec.cfg cannot say so in time:
     * it is read inside InitClientDLL, and the launcher logs in before that. */
    MEM32(CLIENT_USE_LAUNCHPAD) = 0;
    memcpy((void *)(uintptr_t)CLIENT_MASTER_KEY, k_master_modulus, sizeof k_master_modulus);

    /* Credentials as Login reads them: name at +0, password at +0x20, and an
     * empty SecurID vector at +0x60/+0x64. */
    uint8_t *cred = (uint8_t *)guest_alloc(0x100);
    memcpy(cred, user, strlen(user));
    memcpy(cred + 0x20, pass, strlen(pass));
    uint32_t c = (uint32_t)(uintptr_t)cred;
    r = vcall(CLIENT_MEDIATOR, 12, 1, &c);
    printf("[launcher] Login(\"%s\") -> %08X, state %u\n", user, r, login_state());
    if (r) return 0;

    const char *s = getenv("MXO_LOGIN_SECONDS");
    DWORD deadline = GetTickCount() + 1000u * (DWORD)(s ? atoi(s) : 30);
    uint32_t last = login_state();
    while ((int)(GetTickCount() - deadline) < 0) {
        gcall(CLIENT_ENGINE_POLL, g_engine, 0, NULL, 0);
        uint32_t st = login_state();
        if (st != last) {
            printf("[launcher] login state %u -> %u\n", last, st);
            if (st == 0) { last = st; break; }   /* back to idle: the login failed */
            last = st;
        }
        if (st >= 3 && st != 0xFFFFFFFFu) break;
        static DWORD next_dump;
        if (getenv("MXO_LOGIN_TRACE") && (int)(GetTickCount() - next_dump) >= 0) {
            gt_dump();
            next_dump = GetTickCount() + 3000;
        }
        /* The connection's I/O runs on its own guest thread (thread per
         * client), a fiber that only runs when this one yields. */
        gt_yield();
        Sleep(1);
    }
#ifdef RECOMP_TRACE
    if (getenv("MXO_LOGIN_TRACE")) {
        /* The newest function entries, oldest first: the ring spans every
         * fiber, so this is what ran most recently anywhere. */
        printf("[launcher] last entries:");
        for (uint32_t i = RECOMP_ENTER_SIZE; i > 0; i--) {
            uint32_t va = g_enter_trace[(g_enter_idx - i) & (RECOMP_ENTER_SIZE - 1)];
            static uint32_t prev;
            if (va != prev) printf(" %08X", va);
            prev = va;
        }
        printf("\n");
    }
#endif
    *engine = g_engine;
    *mediator = CLIENT_MEDIATOR;
    printf("[launcher] login %s (state %u)\n", last >= 3 ? "complete" : "did not complete", last);
    return last >= 3;
}
