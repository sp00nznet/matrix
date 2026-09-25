/*
 * ILTLoginMediator stand-in: the object launcher.exe hands InitClientDLL.
 *
 * Login lives in the launcher. It binds "ILTLoginMediator.Default" in its own
 * interface database (static object 0x004D4E38, vtable 0x004B01C8, 101 slots),
 * logs in, then passes the object as InitClientDLL's fifth argument. The client
 * refuses to finish init without it: 0x62003380 needs `vt[4]()` true.
 *
 * Almost every slot in the launcher's class is a plain accessor on one large
 * state object -- a byte, a dword or an address at an offset, or a read through
 * the selected-world sub-object at +0x680. So the stand-in is that object with
 * the launcher's layout, zeroed, and a table transcribing each accessor from
 * the launcher method it mirrors. Filling in login state later means writing
 * fields, not code. Slots with logic (most go through a login state machine at
 * +0x10) are not transcribed yet: they report and stop, as engine_iface.c does,
 * because a guessed pop count corrupts the caller's frame.
 *
 * The launcher's methods are thiscall: `this` in ECX, callee pops the args.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

#define MED_VA_BASE   0x7C000000u    /* clear of image, stack, 0x7D MSLU and 0x7E iface VAs */
#define MED_SLOTS     101u
#define MED_OBJ_SIZE  0x1500u        /* launcher's accesses reach +0x1470 */
#define MED_SUB_SIZE  0x200u         /* sub-object at +0x680: reads reach +0x110 */

typedef enum {
    K_BYTE,       /* al  = byte  [this+a]                                  */
    K_DWORD,      /* eax = dword [this+a]                                  */
    K_ADDR,       /* eax = this+a                                          */
    K_DWORD_ADD,  /* eax = dword [this+a] + b   (no null check, as shipped) */
    K_NONZERO,    /* eax = dword [this+a] != 0                             */
    K_SETBYTE1,   /* byte [this+a] = 1                                     */
    K_SET_DEREF,  /* dword [this+a] = *(arg0)                               */
    K_SET,        /* dword [this+a] = arg0                                  */
    K_SET3,       /* dword [this+a], +4, +8 = arg0..2                       */
    K_OUT,        /* if (arg0) *(u16|u32*)arg0 = [this+b]; eax = dword [this+a] */
    K_SUB_BYTE,   /* al  = byte  [[this+0x680]+a]                          */
    K_SUB_DWORD,  /* eax = dword [[this+0x680]+a]                          */
    K_SUB_IF,     /* p = [[this+0x680]+a]; eax = p ? (b==0xA8 ? [p+b] : p+b) : dflt */
    K_ZERO_IF_NULL,/* dword [this+a] == 0 -> return 0; otherwise report      */
    K_CLASSNAME,  /* eax = "CLTLoginMediator" (every class's slot 0)         */
    K_LISTEN_ADD, /* set<listener> at +0x674 insert; eax = !inserted          */
    K_LISTEN_DEL, /* ... erase; eax = !erased                                */
} kind_t;

typedef struct { uint16_t slot; uint8_t kind, argc, wide; uint32_t a, b; } med_slot_t;

/* Transcribed from launcher.exe's vtable at 0x004B01C8; the launcher address
 * of each method is in the comment. Slots the client never calls are left out. */
static const med_slot_t k_slots[] = {
    {  0, K_CLASSNAME, 0, 0, 0 },              /* 41F020  -> 0x004B035C "CLTLoginMediator" */
    {  4, K_BYTE,      0, 0, 0x004 },          /* 41F030  launched-by-launcher flag */
    {  5, K_SETBYTE1,  0, 0, 0x005 },          /* 41F040 */
    {  6, K_BYTE,      0, 0, 0x005 },          /* 41F050 */
    {  7, K_SET_DEREF, 1, 0, 0x008 },          /* 41F060 */
    {  8, K_ADDR,      0, 0, 0x008 },          /* 41F070 */
    {  9, K_SET_DEREF, 1, 0, 0x00C },          /* 41F080 */
    { 10, K_ADDR,      0, 0, 0x00C },          /* 41F090 */
    { 11, K_ZERO_IF_NULL, 0, 0, 0x010 },       /* 41AEE0  state machine at +0x10 */
    { 14, K_ADDR,      0, 0, 0x094 },          /* 41F0A0 */
    { 15, K_BYTE,      0, 0, 0xCC8 },          /* 41F2D0 */
    { 18, K_SUB_DWORD, 0, 0, 0x108 },          /* 41F350 */
    { 19, K_SUB_DWORD, 0, 0, 0x10C },          /* 41F360 */
    { 20, K_SUB_IF,    0, 0, 0x0F4, 0xA8 },    /* 41F370  -> dword, else 0 */
    { 22, K_SUB_BYTE,  0, 0, 0x104 },          /* 41F390 */
    { 23, K_SUB_IF,    0, 0, 0x0F4, 0x85 },    /* 41F3A0  -> string, else "" */
    { 24, K_SUB_DWORD, 0, 0, 0x0F8 },          /* 41F3C0 */
    { 25, K_SUB_DWORD, 0, 0, 0x110 },          /* 41F2B0 */
    { 26, K_BYTE, 0, 0, 0x140E }, { 27, K_BYTE, 0, 0, 0x1416 },   /* 41F0C0.. */
    { 28, K_BYTE, 0, 0, 0x141E }, { 29, K_BYTE, 0, 0, 0x1426 },
    { 30, K_BYTE, 0, 0, 0x142E }, { 31, K_BYTE, 0, 0, 0x13FE },
    { 32, K_BYTE, 0, 0, 0x1406 }, { 33, K_BYTE, 0, 0, 0x1448 },
    { 34, K_BYTE, 0, 0, 0x1452 }, { 35, K_BYTE, 0, 0, 0x13F6 },
    { 36, K_BYTE, 0, 0, 0x145A },                                 /* ..41F160 */
    { 37, K_OUT, 1, 0, 0x1408, 0x140C }, { 38, K_OUT, 1, 0, 0x1410, 0x1414 },  /* 41AD80.. */
    { 39, K_OUT, 1, 0, 0x1418, 0x141C }, { 40, K_OUT, 1, 0, 0x1420, 0x1424 },
    { 41, K_OUT, 1, 0, 0x1428, 0x142C }, { 42, K_OUT, 1, 0, 0x13F8, 0x13FC },
    { 43, K_OUT, 1, 0, 0x1400, 0x1404 }, { 44, K_OUT, 1, 1, 0x1440, 0x1444 },
    { 45, K_OUT, 1, 0, 0x144C, 0x1450 }, { 46, K_OUT, 1, 0, 0x1454, 0x1458 },
    { 49, K_OUT, 1, 0, 0x13F0, 0x13F4 },                                     /* ..41AEC0 */
    { 47, K_ADDR,      0, 0, 0xF48 },          /* 41F170 */
    { 48, K_ADDR,      0, 0, 0xF88 },          /* 41F180 */
    { 50, K_NONZERO,   0, 0, 0x145C },         /* 41F190 */
    { 51, K_DWORD,     0, 0, 0x145C },         /* 41F1A0 */
    { 52, K_ADDR,      0, 0, 0x1460 },         /* 41F1B0 */
    { 53, K_DWORD_ADD, 0, 0, 0x01C, 0x85 },    /* 41B4F0 */
    { 54, K_ZERO_IF_NULL, 0, 0, 0x010 },       /* 41AF00  state>=3 ? byte[+0x684] : 0; no machine -> 0 */
    { 61, K_ADDR,      0, 0, 0xF1C },          /* 41F1C0 */
    { 67, K_ADDR,      0, 0, 0x030 },          /* 41F2C0 */
    { 70, K_ADDR,      0, 0, 0x1470 },         /* 41AF50 */
    { 73, K_SET3,      3, 0, 0x084 },          /* 41F1D0 */
    { 74, K_DWORD,     0, 0, 0x084 },          /* 41F200 */
    { 75, K_DWORD,     0, 0, 0x08C },          /* 41F210 */
    { 76, K_DWORD,     0, 0, 0x65C },          /* 41F310 */
    { 82, K_ADDR,      0, 0, 0x664 },          /* 41F320 */
    { 83, K_SET,       1, 0, 0x660 },          /* 41F330 */
    { 85, K_DWORD,     0, 0, 0x660 },          /* 41F340 */
    { 87, K_NONZERO,   0, 0, 0x01C },          /* 41F230 */
    { 89, K_ZERO_IF_NULL, 0, 0, 0x018 },       /* 41B3F0 */
    { 90, K_DWORD,     0, 0, 0x018 },          /* 41F220 */
    { 91, K_ZERO_IF_NULL, 0, 0, 0x01C },       /* 41B420 */
    { 92, K_LISTEN_ADD, 1, 0, 0x674 },         /* 41DDB0  register login-event listener */
    { 93, K_LISTEN_DEL, 1, 0, 0x674 },         /* 41DDE0 */
    { 94, K_DWORD,     0, 0, 0x080 },          /* 41F240 */
};

static uint8_t *g_obj;

/* The launcher keeps its listeners in a std::set at +0x674; the stand-in keeps
 * them here. They are who a login state change will have to be announced to. */
static uint32_t g_listeners[16];
static uint32_t g_nlisteners;

static int listener_index(uint32_t p)
{
    for (uint32_t i = 0; i < g_nlisteners; i++) if (g_listeners[i] == p) return (int)i;
    return -1;
}
static uint32_t g_pending;
static const char k_empty[1] = "";
static const char k_class[] = "CLTLoginMediator";

static const med_slot_t *find(uint32_t slot)
{
    for (size_t i = 0; i < sizeof k_slots / sizeof *k_slots; i++)
        if (k_slots[i].slot == slot) return &k_slots[i];
    return NULL;
}

#define ARG(n) MEM32(g_esp + 4u + 4u * (n))
#define F8(o)  (*(uint8_t  *)(uintptr_t)(o))
#define F32(o) (*(uint32_t *)(uintptr_t)(o))

static void report(uint32_t slot, const char *why)
{
    printf("\n[mediator] ILTLoginMediator slot %u (vtable +0x%X) %s\n", slot, slot * 4u, why);
    printf("           launcher method: see vtable 0x004B01C8 + 0x%X; called with this=%08X\n",
           slot * 4u, g_ecx);
    printf("           stack: %08X %08X %08X %08X\n", ARG(0), ARG(1), ARG(2), ARG(3));
    fflush(stdout);
    exit(0);
}

static void med_handler(void)
{
    uint32_t slot = g_pending, t = g_ecx;
    const med_slot_t *s = find(slot);
    if (!s) report(slot, "not transcribed yet");

    uint32_t r = 0, sub = F32(t + 0x680);
    switch (s->kind) {
    case K_BYTE:      r = F8(t + s->a); break;
    case K_DWORD:     r = F32(t + s->a); break;
    case K_ADDR:      r = t + s->a; break;
    case K_DWORD_ADD: r = F32(t + s->a) + s->b; break;
    case K_NONZERO:   r = F32(t + s->a) != 0; break;
    case K_SETBYTE1:  F8(t + s->a) = 1; break;
    case K_SET_DEREF: F32(t + s->a) = F32(ARG(0)); break;
    case K_SET:       F32(t + s->a) = ARG(0); break;
    case K_SET3:      F32(t + s->a) = ARG(0); F32(t + s->a + 4) = ARG(1); F32(t + s->a + 8) = ARG(2); break;
    case K_OUT:
        if (ARG(0)) {
            if (s->wide) F32(ARG(0)) = F32(t + s->b);
            else *(uint16_t *)(uintptr_t)ARG(0) = *(uint16_t *)(uintptr_t)(t + s->b);
        }
        r = F32(t + s->a);
        break;
    case K_SUB_BYTE:  r = F8(sub + s->a); break;
    case K_SUB_DWORD: r = F32(sub + s->a); break;
    case K_SUB_IF: {
        uint32_t p = F32(sub + s->a);
        if (s->b == 0xA8) r = p ? F32(p + 0xA8) : 0;
        else              r = p ? p + s->b : (uint32_t)(uintptr_t)k_empty;
        break;
    }
    case K_CLASSNAME: r = (uint32_t)(uintptr_t)k_class; break;
    case K_LISTEN_ADD:
        r = listener_index(ARG(0)) >= 0;
        if (!r && g_nlisteners < 16) {
            g_listeners[g_nlisteners++] = ARG(0);
            printf("  [mediator] listener %08X registered\n", ARG(0));
        }
        break;
    case K_LISTEN_DEL: {
        int i = listener_index(ARG(0));
        r = i < 0;
        if (i >= 0) g_listeners[i] = g_listeners[--g_nlisteners];
        break;
    }
    case K_ZERO_IF_NULL:
        if (F32(t + s->a)) report(slot, "reached with its object present -- logic not transcribed");
        r = 0;
        break;
    }
    g_eax = r;
    g_esp += 4u + 4u * s->argc;      /* return address + args; thiscall callee pops */
}

recomp_func_t mediator_resolve(uint32_t va)
{
    if (va < MED_VA_BASE || va >= MED_VA_BASE + MED_SLOTS * 16u) return NULL;
    g_pending = (va - MED_VA_BASE) / 16u;
    return med_handler;
}

/* Build the object once; the host passes it as InitClientDLL's fifth argument.
 * State is a login that succeeded with nothing else known: launched-by-launcher
 * (+4) set, an empty selected-world record at +0x680, everything else zero. */
uint32_t mediator_object(void)
{
    if (g_obj) return (uint32_t)(uintptr_t)g_obj;
    uint32_t *vt = (uint32_t *)calloc(MED_SLOTS, 4);
    g_obj = (uint8_t *)calloc(1, MED_OBJ_SIZE);
    uint8_t *sub = (uint8_t *)calloc(1, MED_SUB_SIZE);
    for (uint32_t i = 0; i < MED_SLOTS; i++) vt[i] = MED_VA_BASE + i * 16u;
    F32((uintptr_t)g_obj) = (uint32_t)(uintptr_t)vt;
    F8((uintptr_t)g_obj + 0x004) = 1;
    F32((uintptr_t)g_obj + 0x680) = (uint32_t)(uintptr_t)sub;
    return (uint32_t)(uintptr_t)g_obj;
}
