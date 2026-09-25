/*
 * Guest entry from native code: the counterpart of native_call.c.
 *
 * native_call.c lets lifted code call real DLLs. The reverse also happens: a
 * real DLL calls a function pointer the client gave it -- Python calls the
 * client's extension functions and type slots, msacm32 calls an enumeration
 * callback -- and that pointer is a guest address, which the CPU cannot run.
 *
 * Rather than find and rewrite every such pointer, the guest image is mapped
 * non-executable. A native jump into it faults with a DEP violation; the
 * vectored handler redirects to guest_entry_thunk, which copies the caller's
 * arguments onto the simulated stack and runs the lifted function. As in
 * native_call.c, the callee's own `ret N` reports how many arguments to pop, so
 * cdecl and stdcall callbacks both return correctly with no per-API table.
 *
 * Limits, by construction:
 *   - Only the host thread. A callback from a thread the native DLL created
 *     would race every guest register global; it is reported and stopped.
 *   - Integer returns (EAX:EDX). A callback returning float in ST(0) would need
 *     g_st moved onto the real FPU stack; none has been seen.
 *   - GUEST_ARGS words are copied; more than that is not supported.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

#define GUEST_ARGS 16

extern uint32_t g_image_base;
void gt_stack_bounds(uint32_t *lo, uint32_t *hi);   /* guest_threads.c */

static uint32_t g_image_size, g_host_tid;
static uint32_t g_entry_target, g_pop, g_ret;

/* Runs on the native caller's stack. `frame` is the pushad block; the native
 * return address and the arguments sit just above it. */
static uint32_t __cdecl run_guest(uint32_t *frame)
{
    uint32_t target = g_entry_target;
    uint32_t *native_esp = frame + 8;           /* [0] = return address, [1..] = args */
    recomp_func_t f = recomp_lookup(target);

    if (GetCurrentThreadId() != g_host_tid) {
        printf("\n  [guest_call] 0x%08X called back from a foreign thread %lu\n",
               target, GetCurrentThreadId());
        exit(1);
    }
    if (!f) { printf("\n  [guest_call] 0x%08X has no lifted body\n", target); exit(1); }

    uint32_t save[8] = { g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi };

    /* Called from inside a native call, the native code is running on the
     * simulated stack just below g_esp: start the guest frame well below the
     * real ESP (the stack-collision rule, same as guest_wndproc). */
    uint32_t here = (uint32_t)(uintptr_t)&target, lo, hi;
    gt_stack_bounds(&lo, &hi);
    if (here > lo && here < hi) g_esp = (here - 0x40000u) & ~0xFu;

    for (int i = GUEST_ARGS; i >= 1; i--) PUSH32(g_esp, native_esp[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    uint32_t base = g_esp;
    f();
    uint32_t popped = g_esp - base - 4;         /* the callee's own ret N */

    frame[7] = g_eax;                           /* pushad slot for EAX */
    frame[5] = g_edx;                           /* and EDX */
    g_eax = save[0]; g_ecx = save[1]; g_edx = save[2]; g_ebx = save[3];
    g_esp = save[4]; g_ebp = save[5]; g_esi = save[6]; g_edi = save[7];
    return popped;
}

static void __declspec(naked) guest_entry_thunk(void)
{
    __asm {
        pushad
        mov  eax, esp
        push eax
        call run_guest
        add  esp, 4
        mov  g_pop, eax
        popad                       ; EAX/EDX come back as the guest's results
        pop  dword ptr [g_ret]      ; native return address
        add  esp, dword ptr [g_pop] ; callee-popped arguments (0 for cdecl)
        jmp  dword ptr [g_ret]
    }
}

static LONG CALLBACK dep_handler(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    uint32_t addr = (uint32_t)ep->ContextRecord->Eip;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] != 8 /* execute */ ||
        addr - g_image_base >= g_image_size)
        return EXCEPTION_CONTINUE_SEARCH;
    g_entry_target = addr;
    ep->ContextRecord->Eip = (DWORD)(uintptr_t)guest_entry_thunk;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* After the image is mapped: drop execute permission so native jumps into it
 * fault, and install the handler that turns the fault into a guest call. */
void guest_call_install(uint32_t image_size)
{
    DWORD old;
    g_image_size = image_size;
    g_host_tid = GetCurrentThreadId();
    VirtualProtect((void *)(uintptr_t)g_image_base, image_size, PAGE_READWRITE, &old);
    AddVectoredExceptionHandler(1, dep_handler);
}
