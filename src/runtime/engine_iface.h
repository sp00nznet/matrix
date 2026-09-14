/* Engine interface holders: the P6 mechanism.
 *
 * client.dll declares a holder per engine interface -- vtable at +0, interface
 * pointer at +4 -- and waits for the engine to install the pointer. The host is
 * the engine, so the host installs them.
 */
#ifndef MXO_ENGINE_IFACE_H
#define MXO_ENGINE_IFACE_H

#include <stdint.h>

typedef struct {
    uint32_t    holder_va;   /* the holder object in the client's .data */
    const char *name;        /* e.g. "INetMgr.Default" */
} mxo_iface_t;

extern const mxo_iface_t mxo_ifaces[];
extern const uint32_t    mxo_iface_count;

/* Build an object per interface and install it into every holder. Must run
 * AFTER the static constructors (which set the holder's +4 to NULL) and before
 * anything that uses an interface -- i.e. after DllMain, before InitClientDLL. */
void mxo_install_interfaces(void);

/* Synthetic VAs used for engine vtable slots. Chosen well clear of the client's
 * image (0x10000000 + ~10 MB) and of the simulated stack and TIB. */
#define MXO_IFACE_VA_BASE  0x7E000000u
#define MXO_IFACE_SLOTS    256u
#define MXO_IFACE_STRIDE   (MXO_IFACE_SLOTS * 16u)

/* Resolve a synthetic VA to the reporting handler; returns NULL if `va` is not
 * one of ours, so recomp_lookup_manual can fall through to the real tables. */
typedef void (*mxo_iface_fn)(void);
mxo_iface_fn mxo_iface_resolve(uint32_t va);

#endif /* MXO_ENGINE_IFACE_H */
