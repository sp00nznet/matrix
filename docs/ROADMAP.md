# The path forward: client and server, stood up together

Two tracks have been running in this repo since P0 — recompiling the client, and
building a server. They have been described as parallel. They are not: they meet
at a specific, enumerable boundary, and the fastest route to either runs through
the other.

This document is the plan of record for how they come together.

---

## The convergence thesis

The client's next wall (P6) and the server's first real task (S1) are **the same
boundary seen from two sides.**

`RunClientDLL` faults dereferencing a null field inside a live object, because
it is reaching for engine interfaces nobody supplies. Those interfaces are the
same 70 subsystems the game's Python calls into — `discovery`, `AbilityInv`,
`Interlock`, `CharMvt`, `mission` — catalogued in `analysis/engine_api.json`.

A server is, functionally, the authoritative half of exactly those subsystems.
When the client asks its engine "what type is this object", the answer comes
from the object database. When it asks "did that attack land", the answer comes
from the server. **Standing up the engine interface and specifying the server
are the same act of reverse engineering**, and doing them separately would mean
doing the work twice.

### The lever: the client can talk long before it can draw

The single most useful fact in this repo's network analysis:

```
WSOCK32.dll (20)
  WSAStartup  WSACleanup  WSAGetLastError  socket  bind  closesocket
  sendto  recvfrom  select  ioctlsocket  getsockname  getsockopt  setsockopt
  gethostbyname  inet_addr  inet_ntoa  htonl  htons  ntohl  ntohs
```

**No `connect`, no `send`, no `recv`, no `listen`, no `accept`.** The world
protocol is **UDP**, and the entire network surface is twenty functions — small
enough to shim completely in an afternoon.

That matters far more than it looks, because it **decouples "the client renders"
from "the client talks."** Rendering is the expensive, open-ended half of the
client track: D3D9 through COM vtables, shaders, the whole asset pipeline.
Talking is twenty functions.

So we do not have to finish the client to start testing the server against it.
We can drive the client's own protocol code — the authoritative implementation
of the wire format, which no packet capture can equal — while the screen is
still black.

Also worth noting: `WININET` appears separately (`InternetOpenW`,
`InternetConnectW`, `HttpOpenRequestW`, `HttpSendRequestW`), wide-char, which is
a *different* channel — almost certainly patch/news rather than gameplay. And
there is **no TCP anywhere in `client.dll`**, which raises a specific,
checkable question: if the emulators' auth handshake is TCP, then auth does not
happen in the client at all — it happens in `launcher.exe`, which passes a
token. That is worth confirming early because it decides what S2 has to build.

---

## The shared spine

Four artifacts serve both tracks. Building each one is a single job, not two.

| # | Artifact | Feeds the client | Feeds the server |
|---|---|---|---|
| **SP1** | **Socket shim + packet log** | Lets startup get past network init | Ground-truth wire format, from the client's own code |
| **SP2** | **GameObject database** (`SetMasterDatabase`, `Cookbook.cbb`, `gocategories.gcb`) | `RunClientDLL` needs it to initialise | The server spawns and describes the same objects |
| **SP3** | **The engine interface map** (the 70 subsystems) | The interfaces the host must implement | The vocabulary every server message manipulates |
| **SP4** | **The Python rules as an executable spec** | Client-side prediction | Server-side authority |

Everything below is scheduled around producing these four.

---

## Track A — the client

| Phase | Work | State |
|---|---|---|
| P0–P5 | Discs → lift → compile → link → startup → Win32 layer | ✅ done |
| **P6** | **Engine interfaces: make `RunClientDLL` survive** | next |
| **P7** | Socket layer — 20 functions, UDP *(**SP1**)* | ✅ **built**, gated on P6 |
| P8 | Renderer: D3D9 through COM vtables | |
| P9 | Input, audio, the asset pipeline | |

**P6 is the immediate task.** The approach is not to guess at the interface: it
is to follow the null. The fault at `0x103065E0` is reached by a three-call path
from `RunClientDLL`, and `SetMasterDatabase` is an *export* — the engine calls
it to hand the client its object database. That is the thread to pull.

**P8 is deliberately last of the big pieces.** A software-path renderer or even
a black window that does not crash is enough for everything the server track
needs. Chasing pixels first would be the expensive order.

---

## Track B — the server

| Phase | Work | State |
|---|---|---|
| S0 | Prior art, licence position | ✅ done — `docs/SERVERS.md` |
| **S1** | **Fork mxo-hd**; fix the two licence/provenance defects | ✅ private mirror |
| S2 | Auth + character select, verified against the real client | |
| S3 | World: districts, statics, movement, doors, hardlines | |
| S4 | Combat, from the recovered rules rather than reconstruction | |
| S5 | **Missions** — the part nobody has finished | |

S1 and S2 do not depend on the client track at all and can start immediately.
S4 and S5 are where the recompilation pays for itself, because they are exactly
the parts that could not be built from packet captures.

---

## How the two tracks interlock

The order below is chosen so that each step unblocks something on the *other*
track. That is the point.

**Step 1 — Fork mxo-hd (S1).** ✅ Done, as a **private mirror** at
`sp00nznet/mxo-hd-recomp`, checked out at `../mxo-hd-recomp` with `upstream`
wired for syncing.

A GitHub *fork* could not be used: forks of public repositories are always
public and cannot be made private, so `gh repo fork` produced a public repo that
had to be deleted and redone as a mirror. Upstream is 51 commits, last touched
2026-04-02 — a merge of combat data-model work, which is precisely the area we
intend to replace.

Still to do on the fork: correct the AngularJS `LICENSE`, and prefer the managed
`WorldEncryption.cs` over the `enigmaLib.dll` binary.

**Step 2 — Shim the twenty sockets (P7 = SP1).** ✅ *Built* —
[`src/runtime/shims_net.c`](../src/runtime/shims_net.c).

**Correction to the ordering, found by building it:** the socket layer compiles,
links and is wired, but the client never calls `WSAStartup` during
`InitClientDLL`. Network init happens later, behind the *same* P6 engine-
interface wall that stops `RunClientDLL`. So sockets are not gated on the
renderer — that part of the thesis holds — but they **are** gated on P6.

The revised order is **P6 → step 3**, and P6 is now on the critical path for
both tracks at once, which if anything strengthens the case for doing it next.
The socket layer sits ready and will start logging the moment the client reaches
network init.

What it does once reached: every `sendto` and `recvfrom` logged with peer,
length and a hex window to `work/packets.log`; `gethostbyname` and `inet_addr`
answer with the sidecar so nothing leaves the machine; `MXO_SERVER` and
`MXO_PACKET_BYTES` tune it without a rebuild. That gives the client its network
init and gives the server track a byte-exact trace produced by the authoritative
implementation.

**Step 3 — Point the recompiled client at the forked server.** `gethostbyname`
and `inet_addr` are both shimmed, so redirecting to `127.0.0.1` is two lines. Now
there is a **closed loop entirely under our control**: our client, our server,
and a packet log of every disagreement between them.

This is the moment the project stops being two efforts.

**Step 4 — Differential protocol work (S2/S3 against P7).** Every mismatch is a
concrete, reproducible bug with both sides visible. This is how the emulators'
inherited guesses get corrected — not by arguing from memory of 2005, but by
running the client that was there.

**Step 5 — The rules sidecar (SP4 → S4).** Stand the recovered `interlock/` and
`ability/` modules up as a rules service the server delegates to, per
`docs/COMBAT.md`. Differential-test it against mxo-hd's `CombatFormulas.cs` to
enumerate where the reconstruction diverges.

**Step 6 — Missions (S5).** The contract is already recovered: twelve callbacks
and twenty-one engine verbs (`docs/RECON.md`). Implementing those verbs server-
side, with the shipped example missions as the conformance tests, is the first
time anyone has been able to run an MxO mission from its own definition.

---

## What is genuinely uncertain

Stated plainly, because a plan that hides its risks is not a plan.

* **Retail 1.0 versus the final live client.** These discs are February 2005;
  the emulators target the client four years of patches later. The protocol
  almost certainly moved. Step 3 is where this stops being theoretical — and it
  may force a decision to re-target the whole client track. **This is the single
  largest open risk, and it is cheap to retire early**, which is another reason
  to do the socket shim before the renderer.
* **Where auth happens.** No TCP in `client.dll` at all. If auth is in
  `launcher.exe`, S2's shape changes.
* **The `Cool*` / `ILTCool` stack.** Possibly a second protocol (chat/presence)
  a server must also speak. Still unconfirmed.
* **How much of the engine interface P6 needs.** The 70 subsystems are
  enumerated but not sized. `RunClientDLL` may need a handful or most of them.
* **D3D9 through COM vtables.** `Direct3DCreate9` is the only d3d9 import;
  everything else is vtable dispatch, which the import-bridge machinery does not
  reach. `runtime/hybrid/` (built for Encarta's MFC boundary) is the nearest
  existing machinery.

## What this is not

This will not produce a playable MxO soon, and the roadmap does not pretend
otherwise. What it produces, in order, is: a server we control, a client that
talks to it, a byte-exact record of what they say, and then — using rules
recovered rather than guessed — combat and missions that behave the way the
original did.

Each of those is worth having on its own, which is the test a milestone has to
pass to be on this list.


---

## Finding that reframes P6: the engine is not on the disc

`RunClientDLL` needs 28 engine interfaces. Nothing in the retail install fills
them, and that is not an oversight in our host — **the engine executable is
missing from the retail discs.**

`launcher.exe` carries this table, adjacent in one string block:

```
matrix.exe   launcher.exe   client.dll
ErrorClientDLL   TermClientDLL   RunClientDLL   InitClientDLL
cres.dll
http://update.beta.thematrixonline.com/
```

**`matrix.exe` is the LithTech engine host**, it knows the four client-DLL
exports by name, and it is nowhere in the 2,716 files we unpacked. The launcher
downloads it. (The `update.beta.` URL dates these discs to the pre-launch beta
window, which fits a 2005-02-21 master for a March 2005 launch.)

`launcher.exe` itself is not the engine: it contains **none** of
`INetMgr`, `ILTRenderDevice`, `ILTClassMgr`, `ILTWorldRep_Metro` or
`ILTDrawPrim`. It does contain **`ILTLoginMediator`** (11 references), which is
the one interface it shares with the client — so login genuinely does span the
launcher, answering that open question from the other direction.

### What this means

P6 was never "write 28 interfaces from scratch". It is "the other half of this
program is a separate binary we do not have".

That changes the options:

1. **Obtain the final client distribution.** It includes `matrix.exe`. This
   both unblocks P6 *and* retires the roadmap's largest risk — that these
   February 2005 discs differ from the client the emulators target — because it
   settles the version question by making the target the same one. It also
   turns P6 back into a lifting problem, which this project is good at, rather
   than a specification problem.
2. **Write the engine interfaces by hand.** Possible but expensive: arity is
   mechanical, return *type* and object *shape* are semantic, and there are 28
   interfaces of unknown size. Two `INetMgr` slots took a session.
3. **Push the server track instead.** It does not need P6 until step 3.

Option 1 is strictly better than 2 on both cost and risk, and it is the
recommendation. Option 3 remains available in parallel either way.

### Revised reading of the retail discs

They remain the right thing to have started from — they gave the lift, the
Python rules, the engine interface list and the protocol surface, none of which
needed `matrix.exe`. But they are **not a complete game**, and any plan that
assumed they were needs this correction.
