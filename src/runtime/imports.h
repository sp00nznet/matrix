/* Import-shim support for the recompiled client.
 *
 * Every shim runs on the simulated stack, so it is responsible for unwinding
 * its own frame. Two macros do that and the distinction between them is the
 * whole correctness story of this layer.
 */
#ifndef MXO_IMPORTS_H
#define MXO_IMPORTS_H

#include <stdint.h>
#include "recomp_types.h"

/* Return value goes in EAX, like any Win32 call. */
#define RET(v)  do { g_eax = (uint32_t)(v); } while (0)

/* Unwind the frame: the dummy return address plus `k` argument slots.
 *
 * `k` is 0 for cdecl (the caller cleans up) and the real argument count for
 * stdcall (the callee does). Getting it wrong does not fault here -- it shifts
 * the caller's frame, so the caller then reads its own locals from the wrong
 * slots and fails somewhere unrelated. gen_imports.py derives every stdcall
 * count from the SDK import libraries rather than letting anyone type one.
 */
#define STDRET(k)  do { g_esp += 4u + (uint32_t)(k) * 4u; } while (0)

/* A shim with no body yet. Reports once per import, by name, so a run says
 * which one to write next instead of dying anonymously.
 */
#ifndef MXO_QUIET_IMPORTS
#define IMPORT_STUB(nm)                                            \
    do {                                                           \
        static int _warned;                                        \
        if (!_warned) {                                            \
            _warned = 1;                                           \
            recomp_import_stub_hit(nm);                            \
        }                                                          \
    } while (0)
#else
#define IMPORT_STUB(nm)  ((void)0)
#endif

void recomp_import_stub_hit(const char *name);

/* A data import: a slot the client dereferences rather than calls.
 * `storage` stands in until the runtime can repoint the slot at a real export.
 */
typedef struct {
    uint32_t    iat_va;
    void       *storage;
    const char *dll;
    const char *name;
} recomp_import_data_t;

extern const recomp_dispatch_entry_t mxo_import_bridges[];
extern const uint32_t                mxo_import_bridge_count;
extern const recomp_import_data_t    mxo_import_data[];
extern const uint32_t                mxo_import_data_count;

#endif /* MXO_IMPORTS_H */
