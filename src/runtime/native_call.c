/*
 * Native passthrough: run a real host function on the simulated stack.
 *
 * The recurring wall in this project is a *host* code address reaching lifted
 * code -- a real COM vtable slot, a real export. RECOMP_ICALL cannot resolve it
 * (the dispatch table knows only lifted VAs and IAT slots), and even if it
 * could, a host function expects its arguments on the host stack.
 *
 * But the host is a 32-bit process with g_mem_base = 0, and the simulated stack
 * is ordinary committed memory. So point the real ESP at it and call: the
 * arguments the lifted caller pushed are exactly where the callee looks for
 * them. And a stdcall callee pops its own arguments, so the ESP it returns with
 * *measures* the arity -- no SDK, no table, no guessing. That is what makes a
 * whole COM API (D3D9: IDirect3D9, the device, every resource) usable without
 * writing a line per method. A cdecl callee pops nothing, which is also right.
 *
 * Limits, by construction:
 *   - The callee must not call back into guest code by address (that needs a
 *     host thunk, as guest_wndproc in shims_mslu.c does).
 *   - The TEB's stack bounds are swapped for the call so stack probes and any
 *     SEH walk inside the callee see a stack they accept.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

void gt_stack_bounds(uint32_t *lo, uint32_t *hi);   /* guest_threads.c: per guest thread */

static uint32_t g_target, g_host_esp, g_ret_eax, g_ret_edx, g_after_esp, g_sim_esp;

static void __declspec(naked) call_on_sim_stack(void)
{
    __asm {
        pushad
        mov  g_host_esp, esp
        mov  esp, g_sim_esp
        add  esp, 4                 ; drop the dummy return address; `call` pushes a real one
        call dword ptr [g_target]
        mov  g_ret_eax, eax
        mov  g_ret_edx, edx
        mov  g_after_esp, esp
        mov  esp, g_host_esp
        popad
        ret
    }
}

/* Call `target` as if the lifted code had called it: args already pushed,
 * RECOMP_RETADDR on top. Leaves eax/edx and g_esp as a real return would. */
void native_invoke(uint32_t target)
{
    static int trace = -1;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void *base = tib->StackBase, *limit = tib->StackLimit;
    uint32_t outer_host_esp = g_host_esp;   /* re-entrant: a guest callback may nest */

    if (trace < 0) trace = getenv("MXO_NATIVE_TRACE") != NULL;
    if (trace) printf("  [native] -> %08X this=%08X esp=%08X\n", target, MEM32(g_esp + 4), g_esp);
    g_target  = target;
    g_sim_esp = g_esp;
    {
        uint32_t lo, hi;
        gt_stack_bounds(&lo, &hi);
        tib->StackBase  = (void *)(uintptr_t)hi;
        tib->StackLimit = (void *)(uintptr_t)lo;
    }
    call_on_sim_stack();
    tib->StackBase  = base;
    tib->StackLimit = limit;
    g_host_esp = outer_host_esp;

    if (trace) printf("  [native] <- eax=%08X popped=%u\n", g_ret_eax, (g_after_esp - g_sim_esp - 4) / 4);
    g_eax = g_ret_eax;
    g_edx = g_ret_edx;
    g_esp = g_after_esp;        /* = old esp + 4 + 4*(args the callee popped) */
}

static uint32_t g_pending;
static void native_handler(void) { native_invoke(g_pending); }

/* A VA is native if it lies in a module Windows loaded, other than the host exe
 * (the exe's own functions are never reached by guest address). Called only for
 * VAs outside the guest image, so the lifted fast path never pays for this. */
recomp_func_t native_resolve(uint32_t va)
{
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)va, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    g_pending = va;            /* same single-global argument as engine_iface.c */
    return native_handler;
}

/* COM factory imports: forward to the system DLL, natively. Every call on the
 * object they return then comes back through native_resolve, so the whole API
 * works without a shim per method. `argc` is only for the failure path (the
 * DLL is missing); on success the callee's own pop is what counts. */
static void forward_native(const char *dll, const char *fn, uint32_t argc)
{
    HMODULE m = LoadLibraryA(dll);
    uint32_t f = m ? (uint32_t)(uintptr_t)GetProcAddress(m, fn) : 0;
    if (!f) { printf("  [%s] %s unavailable\n", fn, dll); g_eax = 0; g_esp += 4 + 4 * argc; return; }
    native_invoke(f);
    printf("  [%s] -> %08X\n", fn, g_eax);
}

/* Retail imports Direct3DCreate9 by name. The final client imports it as
 * r3d9.dll!RajkoD3DCreate9 -- the community wrapper's name for the same call,
 * same argument (0x20, D3D_SDK_VERSION, pushed at 0x6234E091). */
void imp_Direct3DCreate9(void)  { forward_native("d3d9.dll", "Direct3DCreate9", 1); }
void imp_RajkoD3DCreate9(void)  { forward_native("d3d9.dll", "Direct3DCreate9", 1); }

/* DirectInput8Create(hinst, version, riid, ppv, outer): hinst must be a module
 * Windows knows, and the client passes its own (guest) base. */
void imp_DirectInput8Create(void)
{
    MEM32(g_esp + 4) = (uint32_t)(uintptr_t)GetModuleHandleA(NULL);
    forward_native("dinput8.dll", "DirectInput8Create", 5);
}

/* COM activation: the object comes back real, and its methods run natively like
 * D3D9's. The CLSID is logged so each class the client instantiates is on record. */
void imp_CoInitialize(void)   { forward_native("ole32.dll", "CoInitialize", 1); }
void imp_CoCreateInstance(void)
{
    const GUID *c = (const GUID *)(uintptr_t)MEM32(g_esp + 4);
    printf("  [CoCreateInstance] {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
           c->Data1, c->Data2, c->Data3, c->Data4[0], c->Data4[1], c->Data4[2],
           c->Data4[3], c->Data4[4], c->Data4[5], c->Data4[6], c->Data4[7]);
    forward_native("ole32.dll", "CoCreateInstance", 5);
}

/* dllMediaPlayer.dll (the in-game video player) crashes when run natively in
 * this host, and the client already copes with "no player": it was a NULL
 * stub when InitClientDLL first succeeded. So say so, honestly. Both are
 * cdecl (no arguments popped by the callee). */
static const char k_no_player[] = "media player not available in the recomp host";
void imp__CreateMediaPlayer__YAPAVIMediaPlayer__PAUHWND_____Z(void) { g_eax = 0; g_esp += 4; }
void imp__GetMediaPlayerLastError__YAPBDXZ(void) { g_eax = (uint32_t)(uintptr_t)k_no_player; g_esp += 4; }

/* The native call in progress, for the watchdog (0 when none). */
uint32_t native_current(void) { return g_target; }
