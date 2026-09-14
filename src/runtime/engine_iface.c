/*
 * Engine interface holders -- the P6 mechanism.
 *
 * client.dll never creates its engine interfaces. It declares a **holder** per
 * interface (vtable at +0, interface pointer at +4) and waits for the engine to
 * install the pointer. `tools/find_interfaces.py` recovered all 28 by name;
 * this file is the engine side of that contract.
 *
 * ## How a slot is called, and why the vtable is full of synthetic addresses
 *
 * The lifted code reaches a method the ordinary C++ way:
 *
 *     ecx = MEM32(holder + 4)     ; the interface pointer we install
 *     eax = MEM32(ecx)            ; its vtable
 *     ICALL(MEM32(eax + 0x3C))    ; slot 15
 *
 * so whatever we put in the vtable has to be a value `RECOMP_ICALL` can
 * resolve. Host function pointers cannot be: the dispatch table only knows
 * lifted VAs and IAT slots, and a host function reached that way would expect
 * its arguments on the host stack rather than the simulated one.
 *
 * So each slot holds a **synthetic VA** encoding (interface, slot), and
 * `recomp_lookup_manual` -- the hook the runtime already consults first --
 * resolves it. That hook existed and returned NULL from the start; this is what
 * it was for.
 *
 * ## Why an unimplemented slot stops the run
 *
 * A shim must pop exactly the arguments its caller pushed. For an import we
 * derive that from the SDK; for an unknown C++ virtual there is nothing to
 * derive it from, and popping the wrong number does not fail at the call -- it
 * shifts the caller's frame so everything read afterwards is garbage, and the
 * crash lands somewhere unrelated. That failure mode has already cost this
 * project several rounds.
 *
 * So an unimplemented slot reports which interface and slot it is, dumps the
 * stack words the caller pushed, and **stops**. One clean worklist entry per
 * run, and the arity comes from reading the call site rather than guessing.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "engine_iface.h"

/* Object layout we hand the client: vtable pointer, then slack for any members
 * it writes through the interface. Generous on purpose -- a too-small object
 * corrupts the heap silently, and these are 28 allocations, not a hot path. */
#define IFACE_OBJ_WORDS 64

static uint32_t *g_vtables[64];
static uint32_t  g_objects[64];
static uint32_t  g_installed;

/* Set by mxo_iface_resolve immediately before the handler runs.
 *
 * ponytail: a single global rather than a thunk per slot. RECOMP_ICALL looks a
 * target up and calls it on the next line, and the handler below does not
 * itself make an interface call, so nothing can interleave. If an implemented
 * slot ever needs to call back into another interface, this becomes a stack.
 */
static uint32_t g_pending_va;


/* ---- implemented slots ---------------------------------------------- *
 *
 * One line per slot the client has actually asked for. Arity comes from reading
 * the call site in the lifted C -- count the PUSH32s between the `ecx = this`
 * and the ICALL, ignoring the prologue's register saves -- never from guessing.
 *
 * RET_OBJ hands back a fresh object with its own synthetic vtable, so whatever
 * the client does with the result keeps reporting instead of faulting. That is
 * what lets one run advance past a slot rather than stopping at it.
 */
typedef enum {
    RET_ZERO,     /* eax = 0 */
    RET_ONE,      /* eax = 1 */
    RET_OBJ,      /* eax = a fresh object with its own synthetic vtable */
    RET_PTRNULL,  /* eax = a pointer to a word holding NULL */
} iface_ret_t;

/* These interfaces are COM-shaped, and the idiom shows up all over the client:
 *
 *     p = slot_n();            ; p points AT an interface pointer
 *     if (*p != cur) {
 *         if (cur) cur->vt[2]();   ; slot 2 = Release
 *         cur = *p;
 *         if (cur) cur->vt[1]();   ; slot 1 = AddRef
 *     }
 *
 * So vtable slot 1 is AddRef and slot 2 is Release throughout, and a getter of
 * this shape returns a pointer-to-pointer rather than the object itself.
 *
 * RET_PTRNULL answers that with a pointer to a NULL word -- "no object" -- which
 * is a legitimate state the client already handles: it releases whatever it
 * held, adopts NULL, and skips the AddRef. That walks the whole path without
 * having to invent an object, and is why it beats RET_OBJ here.
 */

typedef struct {
    const char *iface;      /* substring match on the interface name */
    uint32_t    slot;
    uint32_t    argc;       /* stack args, excluding `this` (thiscall: callee pops) */
    iface_ret_t ret;
} iface_impl_t;

static const iface_impl_t g_impls[] = {
    /* INetMgr slot 3: called from sub_10306720 as "if the interface exists,
     * call slot 3 on it", with no arguments pushed. The instruction after the
     * call clears a local -- the return value is never read -- so RET_ZERO is
     * both sufficient and safer.
     *
     * It was RET_OBJ first, and that was a mistake worth keeping the note for:
     * handing back a fabricated object invites the client to use it, and its
     * zeroed members then get read as function pointers. That produced
     * "ICALL: unresolved VA 0x0099CCA0" (our own allocation, called as code)
     * and "unresolved VA 0x00000000". Never invent an object for a result
     * nobody reads. */
    { "INetMgr", 3, 0, RET_ZERO },
    /* INetMgr slot 15: called from sub_103065E0 -- the function that was
     * faulting before any of this existed. One argument pushed (a pointer
     * to a caller local, i.e. an out-parameter), and the result is
     * dereferenced on the next instruction, so it must be a real object. */
    { "INetMgr", 15, 1, RET_PTRNULL },
};

/* Objects we hand out beyond the 28 holders (sub-objects returned by slots).
 * They get their own indices so the report can tell them apart. */
#define IFACE_MAX_OBJS 64
static uint32_t g_extra_used;

static uint32_t make_object(uint32_t idx)
{
    uint32_t *vt = (uint32_t *)calloc(MXO_IFACE_SLOTS, sizeof(uint32_t));
    uint32_t *ob = (uint32_t *)calloc(IFACE_OBJ_WORDS, sizeof(uint32_t));
    if (!vt || !ob) return 0;
    for (uint32_t s = 0; s < MXO_IFACE_SLOTS; s++)
        vt[s] = MXO_IFACE_VA_BASE + idx * MXO_IFACE_STRIDE + s * 16u;
    ob[0] = (uint32_t)(uintptr_t)vt;
    return (uint32_t)(uintptr_t)ob;
}

static const iface_impl_t *find_impl(const char *name, uint32_t slot)
{
    for (size_t i = 0; i < sizeof g_impls / sizeof g_impls[0]; i++)
        if (g_impls[i].slot == slot && strstr(name, g_impls[i].iface))
            return &g_impls[i];
    return NULL;
}

static void iface_slot_handler(void);

mxo_iface_fn mxo_iface_resolve(uint32_t va)
{
    uint32_t off = va - MXO_IFACE_VA_BASE;
    if (va < MXO_IFACE_VA_BASE || off >= MXO_IFACE_STRIDE * 64u)
        return NULL;
    g_pending_va = va;
    return iface_slot_handler;
}

static void iface_slot_handler(void)
{
    uint32_t off  = g_pending_va - MXO_IFACE_VA_BASE;
    uint32_t idx  = off / MXO_IFACE_STRIDE;
    uint32_t slot = (off % MXO_IFACE_STRIDE) / 16u;
    const char *name = idx < mxo_iface_count ? mxo_ifaces[idx].name : "?";

    const iface_impl_t *impl = find_impl(name, slot);
    if (impl) {
        uint32_t r = 0;
        if (impl->ret == RET_ONE) r = 1;
        else if (impl->ret == RET_OBJ) {
            if (g_extra_used < IFACE_MAX_OBJS)
                r = make_object(mxo_iface_count + g_extra_used++);
        } else if (impl->ret == RET_PTRNULL) {
            uint32_t *cell = (uint32_t *)calloc(1, sizeof(uint32_t));
            r = (uint32_t)(uintptr_t)cell;
        }
        g_eax = r;
        g_esp += 4u + impl->argc * 4u;      /* dummy return address + args */
        return;
    }

    printf("\n[iface] %s :: slot %u  (vtable +0x%X)\n", name, slot, slot * 4u);
    printf("        this=0x%08X  esp=0x%08X  called from lifted 0x%08X\n",
           g_ecx, g_esp, g_cur_func);
    printf("        stack:");
    for (int i = 0; i < 8; i++)
        printf(" %08X", MEM32(g_esp + 4u * (uint32_t)i));
    printf("\n");
    printf("        [esp] is the dummy return address; arguments follow.\n"
           "        Implement this slot, then re-run for the next one.\n");
    fflush(stdout);

    /* Stop rather than guess the pop count. See the header comment. */
    exit(0);
}

/* Which interfaces to install, as a comma-separated substring match:
 *
 *     MXO_IFACES=INetMgr          only that one
 *     MXO_IFACES=all              all 28
 *     (unset)                     none
 *
 * Default none, because installing all 28 at once is actively worse than
 * installing none. A holder with a non-NULL pointer sends the client down its
 * "this subsystem is available" path, and it then chases pointers *through* the
 * object we supplied. A bare vtable over zeroed members is enough to be called
 * but not enough to be used, so the client dereferences a null member and dies
 * somewhere new -- which is how installing everything turned a completing
 * InitClientDLL back into a crash, in ILTLoginMediator rather than INetMgr.
 *
 * One at a time, in the order the client asks for them.
 */
static int wanted(const char *name)
{
    const char *spec = getenv("MXO_IFACES");
    if (!spec || !*spec) return 0;
    if (!strcmp(spec, "all")) return 1;

    char buf[512];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
        if (*tok && strstr(name, tok)) return 1;
    return 0;
}

void mxo_install_interfaces(void)
{
    uint32_t n = mxo_iface_count < 64 ? mxo_iface_count : 64;

    for (uint32_t i = 0; i < n; i++) {
        if (!wanted(mxo_ifaces[i].name)) continue;
        uint32_t *vt = (uint32_t *)calloc(MXO_IFACE_SLOTS, sizeof(uint32_t));
        uint32_t *ob = (uint32_t *)calloc(IFACE_OBJ_WORDS, sizeof(uint32_t));
        if (!vt || !ob) { fprintf(stderr, "iface alloc failed\n"); return; }

        for (uint32_t s = 0; s < MXO_IFACE_SLOTS; s++)
            vt[s] = MXO_IFACE_VA_BASE + i * MXO_IFACE_STRIDE + s * 16u;

        ob[0] = (uint32_t)(uintptr_t)vt;          /* the object's vtable */
        g_vtables[i] = vt;
        g_objects[i] = (uint32_t)(uintptr_t)ob;

        /* Install into the holder's +4. The holder itself lives in the
         * client's mapped .data, so this is a plain guest write. */
        MEM32(mxo_ifaces[i].holder_va + 4u) = g_objects[i];
        g_installed++;
    }

    printf("engine interfaces: %u of %u installed (MXO_IFACES=%s)\n",
           g_installed, mxo_iface_count,
           getenv("MXO_IFACES") ? getenv("MXO_IFACES") : "(none)");
}
