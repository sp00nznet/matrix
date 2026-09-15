# Working plan: three threads, and the order to take them

Three things are ready to move. This is the order and why.

| | Thread | Depends on | Character |
|---|---|---|---|
| **A** | Re-target the lift to the final client (`MxO_7.6005`) | nothing | mechanical, mostly machine time |
| **B** | P6: find the interface bind step | best done *after* A | uncertain, semantic |
| **C** | Server: build the fork, fix its two licence defects | nothing | independent |

## Why A before B

B is the thread we were on, and it is the one at risk of being wasted work.

Every P6 finding so far — holder addresses, the 55 registration sites, the
nifty-counter cluster — is expressed in **retail 1.0 addresses**. If we re-target
to the final client, every one of those addresses moves. The *mechanism*
transfers; the coordinates do not.

And we are re-targeting regardless, because the final client is version parity
with what the emulators target, which is the roadmap's largest risk. So doing B
first means doing the address archaeology twice.

**A first.** It is also the cheapest of the three in attention: the pipeline is
already built and the steps are `run and wait`.

## Thread A — re-target to the final client

| Step | What | Notes |
|---|---|---|
| A1 | Extract `MxO_7.6005` in full | 2,666 files, ~6 GB |
| A2 | PE analysis of the new `client.dll` | 10,924,032 bytes vs retail 9,515,008 |
| A3 | IDA `ida_funcs` + `ida_xrefs` | ~40 min each, background |
| A4 | `disasm32` for the gap fill | IDA alone missed ~20k functions on retail |
| A5 | Merge catalog, lift, build, run | pipeline is unchanged |
| A6 | Re-run `find_interfaces.py` | new holder addresses |

**The one real risk to check early:** the shipped `client.dll` is dated
2025-02-26 and the distribution includes `mxowrap.dll` (2025-11-03), so this is a
**community-maintained** build, not a pristine 2009 original. If `client.dll`
itself has been patched — to redirect servers, or to skip checks — we are lifting
someone's modifications rather than the game. A2 checks that: same exports, same
section layout, same `STLPORT_`, same import shape means the structure is intact.

Worth stating plainly: *some* modification is likely the point — a client that
can talk to a private server has to have been pointed at one somehow. That is
useful rather than contaminating, as long as we know it is there.

## Thread B — P6 bind, on whichever client wins

The open question is sharp and cheap to answer: **does `register()` execute at
all during a run?** We know it is not binding at registration time (the
`ILTLoginMediator` holder stays NULL through an `InitClientDLL` that contains a
flags=1 registration for it), and the database looks empty.

Two outcomes, both useful:

* **It runs** ⇒ registrations go somewhere we have not found, and the bind is a
  separate pass to locate.
* **It never runs** ⇒ the static initialisers that register are not among the
  12,189 `_initterm` executed, and the question becomes *why*.

The instrument: `register()` is reached by direct `RECOMP_CALL`, not `ICALL`, so
`recomp_lookup_manual` cannot intercept it. The way in is
`recomp_lookup_manual`'s sibling — put the register VA in the manual table and
have the lift route it — or simpler, patch the single generated call site to
count. Either is a few lines.

## Thread C — server, in parallel

Independent of both. Build the fork, confirm it runs, and clear the two defects
already documented in `FORK-NOTES.md`: the AngularJS `LICENSE`, and the
`enigmaLib.dll` native dependency in favour of the managed `WorldEncryption.cs`.

C needs no client at all until roadmap step 3, so it can proceed whenever A is
occupying machine time.

## What "done" looks like for this round

1. The final client lifts, compiles, links and completes `InitClientDLL` — the
   same milestone retail reached, on the version that matters.
2. `register()` instrumented, and the empty-database question answered either
   way.
3. The forked server builds and runs.


---

## A2 result: what the final client actually is

| | retail | final (`MxO_7.6005`) |
|---|---|---|
| image base | `0x10000000` | **`0x62000000`** — rebased |
| exports | 10 | **5** |
| imports | 479 | 521 |
| timestamp | 2005-02-18 | **2013-01-06** |
| `.text` | 7,642,490 | 8,810,496 |
| `STLPORT_`, linker | present, 7.10 | present, 7.10 |

**Structurally the same game**: the five LithTech contract exports are intact,
STLport is still there, the compiler is unchanged, and the section layout
matches. The lift pipeline applies unaltered apart from the image base.

**But it is a modified binary**, and the plan should say so rather than pretend
otherwise. The 2013 timestamp is four years after shutdown, and it has been
rebased. This is a community-maintained build — which is exactly why it is the
right protocol target, and also why it is not evidence about what Monolith
shipped.

### It closes an open question: the `Cool*` stack is gone

The five `ILTCool` exports are **absent** from the final client, and the
distribution contains **no `Cool*.dll`, no `libltcool.dll`, no
`Xprt`/`Xpcs`/`Xptl`** — against eleven such files on the retail discs.

So the LithTech "Cool" middleware was removed from the shipping game. The
roadmap listed "is `Cool*` a second protocol a server must also speak?" as an
open risk. **It is not.** A server only has to speak the world protocol.

### Consequences for the host

`MXO_IMAGE_BASE` becomes `0x62000000`. No conflicts: the simulated stack
(`0x0E000000`), TIB (`0x0E800000`) and synthetic interface VAs (`0x7E000000`)
all sit clear of `0x62000000`–`0x62B28000`.
