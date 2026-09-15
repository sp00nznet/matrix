# The Matrix Online — Static Recompilation

Static recompilation of **The Matrix Online** (Monolith Productions / Sony
Online Entertainment, 2005) from its shipping Win32 client to native C — and,
on top of that, an MIT-licensed server that implements the rules the client
actually expects.

Built on the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolchain.

> The official servers shut down on 2009-07-31. Two emulator projects have kept
> some version of the game alive since, both built from packet captures taken
> while the lights were still on. That method recovers the wire format well and
> the rules behind it badly — a capture shows you the damage number, not the
> formula. This project comes at it from the other side: the client is still
> here, it still contains everything it ever knew about how the game works, and
> nobody has read it.

## Project status: **`InitClientDLL` completes.** 12,189 constructors, 64,377 calls.

Two results, and the second one is the interesting one.

**The whole binary lifts, compiles, links and starts up.** 76,530 functions to
4.85 million lines of C with zero lift errors. The recompiled client now runs
its 12,189 C++ static constructors and returns cleanly from `InitClientDLL`,
64,377 indirect calls in. See [P4](#p4-it-runs-and-where-it-stops).

**The gameplay rules were never compiled in the first place.** Combat,
abilities, interlock and missions ship as Python 2.3 bytecode, in an archive
format that took about forty lines to unpack. See
[The rules are in the box](#the-rules-are-in-the-box).

| | |
|---|---:|
| target | `client.dll` |
| `.text` | **7,642,490 bytes** |
| imports | 479 across 19 DLLs |
| protection | **none** |
| functions recovered | **76,530** (IDA 56,287 + 20,241 gap fill + 2 seeds) |
| call edges | 85,297 |
| **lifted** | **76,530** |
| **lift errors** | **0** |
| generated C | 4,854,290 lines in 154 files |
| **compiles** | **156 objects, 0 errors** (MSVC 2022, x86) |
| **links** | **mxo_host.exe, 121 MB** |
| **runs** | **`DllMain` + `InitClientDLL` complete, exit 0** |
| static constructors run | **12,189** |
| import shims with real bodies | **~215 of 479** |
| window | **1024x768, created by the host** |
| indirect calls | **64,377** |
| game Python modules recovered | **57** (+22 string, +325 stdlib) |
| decompiled cleanly | **56 / 56 attempted**, 10,026 lines |
| engine subsystems identified | **70** |
| engine interfaces the client waits for | **28** (installable, one at a time) |
| first interface method reached | **`INetMgr.Default` slot 3** |

---

## P0: what the discs contain

Three retail CD images, `Disk1`–`Disk3`, dated 2005-02-21, authored with WinISO.
Together they carry an InstallShield 9 set (`ISc(` signature, version
`0x01009500`) spanning four cabinet volumes:

| Volume | Lives on | File indices |
|---|---|---|
| `data1.cab` | Disk 1 | 0–45 |
| `data2.cab` | Disk 1 | 45–201 |
| `data3.cab` | Disk 2 | 201–252 |
| `data4.cab` | Disk 3 | 252–2715 |

**2,716 files across 124 directories, 5.2 GB installed.** Unpacked with the
toolbox's `tools/assets/isextract.py`, which found and ordered all four volumes
on its own — the per-volume offset handling it grew for earlier projects is
exactly what a four-volume set needs.

Two files failed to extract: `packmaps/prefabs_6m.pkb` and
`packmaps/worlds_2g.pkb`, both truncated mid-chunk. They are art packs, not on
the critical path, and the cause has not been chased yet — it is recorded here
rather than fixed because nothing downstream needs them.

### The binaries

| Binary | Size | `.text` | Base | Built | Role |
|---|---:|---:|---|---|---|
| `client.dll` | 9,515,008 | **7,642,490** | `0x10000000` | 2005-02-18 | **the game** |
| `launcher.exe` | 4,816,896 | 573,882 | `0x00400000` | 2005-02-18 | launcher / patcher shell |
| `pythonMXO.dll` | 1,110,016 | 713,158 | `0x1E000000` | 2005-02-05 | embedded Python |
| `p_dlls/patchctl.dll` | 745,472 | — | — | — | patch control |
| `p_dlls/patchw32.dll` | 205,312 | — | — | — | patch transport |
| `crashreporter.exe` | 471,040 | — | — | — | crash handling |
| `binkw32.dll` | 339,456 | — | — | — | RAD Bink video |

Everything is MSVC 7.1 (linker `7.10`), and both `client.dll` and
`launcher.exe` carry a `STLPORT_` section — the game is built against STLport
rather than the Microsoft STL. That is a classification detail that will matter
a great deal later: STLport's containers inline aggressively, so a large share
of those 7.6 MB will be template expansion rather than game logic.

**`client.dll` is the whole game.** 7.64 MB of `.text` makes it the
second-largest target this toolchain has faced — behind Rise of Legends
(13.25 MB) and ahead of Star Wars: Force Commander (3.94 MB), roughly 2× X-Wing
Alliance.

`launcher.exe` is nearly 4 MB of `.rsrc` (`0x3CFDC0` = 3,997,632 bytes) wrapped
around 574 KB of code. It is a shell, not the game.

### Conditions are good

* **No DRM, no packer.** `analyze_sections.py` finds no indicators; `.text`
  entropy is 6.04. No runtime dump needed — unlike Crimson Skies or XWA.
* **Full `.reloc`**, 497,068 bytes of it. It is a DLL, so it must relocate, and
  that hands the lifter the relocation data it otherwise has to infer.
* **Fixed, known import surface**: 479 functions from 19 DLLs.
* **Modern-ish compiler.** MSVC 7.1 from 2005 is far friendlier ground than the
  16-bit Borland and MSC 5.x targets in this collection.

## The rules are in the box

`client.dll` imports **the Python C API from `pythonMXO.dll`** — `Py_Initialize`,
`Py_InitModule4`, `PyRun_SimpleString`, `PyRun_String`, `PyObject_CallObject`,
`PyImport_ImportModule`, the type objects, the lot. That made the two `.ltp`
files in `resource/Python/` the first thing to look at, ahead of any lifting.

They are archives. The format is simple enough to describe in a paragraph —
single-entry records concatenated back to back, each with an 18-byte header,
raw-deflate payload, followed by a redundant directory — and
[`tools/ltp_extract.py`](tools/ltp_extract.py) reads it in about forty lines.

Inside: **382 Python 2.3 modules.** 325 of them are the Python standard library
(`encodings`, `email`, `xml`, `compiler`, `logging`) and are of no interest.
The other **57 are the game**, and the file names are the headline:

| Package | Modules | What is in there |
|---|---:|---|
| *(root)* | 23 | `combat`, `combatvalidate`, `combat_defines`, `serverrules`, `clientrules`, `experience`, `abilities`, `augments`, `recipes`, `crafting_server`, `missionvalidate`, `generic_free_attacks`, `tempmods`, `emotes`, `trgs`, `clhooks`, `rshooks`, `ltfxmap` |
| `ability/` | 24 | `defines`, `effects`, `general`, `utility`, and the discipline trees: `hacker/` (virii, health, crafting, simulacra), `soldier/` (aikido, karate, pistol, rifle, smg), `spy/` (sneaks, misc), `npc/specialmoves` |
| `interlock/` | 7 | **`combat_resolution`**, `combat_modifiers`, `combat_abil_level`, `combat_free_attacks`, `combat_innerstrength`, `combat_utility` |
| `Missions/` | 4 | `Mission`, plus two shipped mission implementations |

Recall the community's standing explanation for why combat has never been
right, quoted in [`docs/SERVERS.md`](docs/SERVERS.md): that the damage formulas,
hit/miss calculations and interlock state machine are buried in the client
binary, and that without disassembling those specific functions everyone is
guessing from player observation.

They are not buried in the binary. There is a module called
`interlock/combat_resolution.pyc`, and it decompiles — 395 lines of it, with
`calculateDamage`, `generateVulnerabilities`, `generateExploits` and
`calcDamageResistFromLevelDifference` in plain sight.

**[`docs/COMBAT.md`](docs/COMBAT.md)** is the write-up: the full function
inventory, a point-by-point comparison against Hardline Dreams' reconstruction
(which got the vulnerability system right and the tactic modifier wrong), and
three ways to test the recovered rules against a running server.

### And the client ships the server's rule code

`serverrules.py` decompiles to **1,397 lines, 82 functions**. `clientrules.py`
is 552 lines and 42. Comparing them by name:

* **35 functions exist in both** — `DoorPickLock`, `CardKeyTest`, `TrapDisarm`,
  `ComputerInfect`, `SecurityPBXDisable`, `HardlineHackControlNodeStage1/2/3`,
  `Repair`, … The client predicts, the server authorises, and both halves are
  in the retail install.
* **47 are server-only** — `ComputerHack`, `CompileCode`, `DecryptCode`,
  `InvokeVendor`, `HardlineUpload`, `MissionTeam_RequestJoin`, `SendMissionText`,
  `InfoPickup`, … Everything that grants, consumes or reveals.
* **7 are client-only**, all presentation.
* `serverrules` has **`evalContest(A, aMod, D, dMod)`** and `clientrules` does
  not. That is the contest primitive the interaction system resolves through,
  and it is server-side only.

Beside it, `rshooks.py` (301 lines) carries the reward machinery:
`RunMissionRewards`, `RunReward`, `RunVendorList`, `RunPocketChange`,
`RunCollectorPrizeList`, `TableImport`, `GoUpALevel`, `VendorBuyback`.

For a server project this is close to the best available outcome: the
authoritative rules need not be inferred from packet captures, because the
retail client shipped them. The honest caveat is in
[`docs/COMBAT.md`](docs/COMBAT.md) — a module named `serverrules` inside a
client archive is very strong evidence, not proof, that the live server ran
that exact code, and four years of patches followed these discs.

One structural note that shapes everything else: nearly every rule function
takes a single parameter named `sentence`, and ability functions take
`subject`, `directObject`, `indirectObject`. **The interaction system is a
grammar** — actor, verb, objects, evaluated by a rule.

**This does not make the recompilation pointless — it re-targets it.** The
engine underneath is still 7.6 MB of compiled C++, and the Python only means
anything in terms of the engine API it calls into. `Py_InitModule4` is the
seam: every module the engine registers is a C++ subsystem the scripts drive.
Lifting the client is now about recovering *that* interface, with the scripts
in hand telling us what it has to do.

Extraction output goes to `work/py/` and stays there. Those modules are the
game's own code and are not ours to publish; the committed artifact is
`analysis/python_modules.json`, which records names and sizes — facts about the
archive, not a copy of it.

### A second network stack

Besides the game protocol, the install carries an entire separate networking
middleware set:

```
libltcool.dll   CoolBos CoolBucky CoolHttp CoolIcq CoolPeer
CoolSec CoolSecNss CoolSocket CoolSos CoolTih CoolWrapper
Xpcs.dll  Xprt.dll  Xptl.dll
```

The `libltcool` name ties it to LithTech. `CoolIcq` and `CoolPeer` point at
instant messaging and peer-to-peer. The working hypothesis is that this is the
chat/IM/presence transport, separate from the world protocol — which would mean
a server has **two** protocols to satisfy, not one. Unconfirmed; on the P1 list.

---

## The prior art, and the licence line

Two emulators have carried this game since shutdown. Their licences differ in a
way that decides how this project is built:

| Project | Language | Licence | Consequence for us |
|---|---|---|---|
| [hdneo/mxo-hd](https://github.com/hdneo/mxo-hd) (Hardline Dreams) | C# | **MIT** | May be used, with attribution |
| [rajkosto/mxoemu](https://github.com/rajkosto/mxoemu) (Reality) | C/C++ | **AGPL-3.0** | **Read only. Never copied.** |

mxoemu's licence file is `Reality/Source/LICENCE.TXT`, not a root `LICENSE`,
which is why GitHub's sidebar shows no licence at all and why it is widely
misreported. It is AGPL-3.0 — the one licence built specifically to reach users
who merely connect to a running server.

We stay MIT by never copying it. Facts are not copyrightable; expression is. And
the whole point of recompiling the client is that we do not need anyone else's
expression — **the client is the authoritative source for the protocol, and it
is a source we can read ourselves.**

`docs/SERVERS.md` is the full survey: what each project achieved, how it was
built, and the list of things both still guess at.

**Also closed:** there is no console version of MxO to fold in. It was
Windows-only, no cancelled port. *Enter the Matrix* and *Path of Neo* are
different studios, different engines, no relation, and nobody has decompiled
those either.

---

## The plan

Two tracks that feed each other. The recomp is not the goal; it is the
instrument that makes the server track possible.

**[`docs/ROADMAP.md`](docs/ROADMAP.md) is the plan of record** for how the two
are stood up together. The short version, because it changes the ordering:

The client's next wall and the server's first real task are **the same boundary
seen from two sides.** `RunClientDLL` is reaching for engine interfaces nobody
supplies, and those interfaces are the same 70 subsystems the game's Python
calls into — which is also, functionally, what a server is the authoritative
half of.

And there is a lever. The entire network surface is **twenty WSOCK32 functions**,
with no `connect`, `send`, `recv`, `listen` or `accept` — so the world protocol
is **UDP**, and shimming it is an afternoon rather than a project. That
**decouples "the client renders" from "the client talks"**: rendering is D3D9
through COM vtables and an asset pipeline, while talking is twenty functions.

So the renderer is deliberately scheduled *late*. Shim the sockets, point the
recompiled client at the forked server on `127.0.0.1` (both `gethostbyname` and
`inet_addr` are already shimmed), and there is a closed loop under our control —
our client, our server, and a byte-exact log of every disagreement — with the
screen still black.

That is also the cheapest way to retire the project's largest risk: these are
the February 2005 discs and the emulators target a client four years of patches
later.

### Track A — recompile the client

| Phase | What | State |
|---|---|---|
| **P0** | Unpack discs, identify binaries, rule out DRM | ✅ done |
| **P1a** | Python surface: unpack the `.ltp` archives | ✅ **57 game modules** |
| **P1b** | Decompile them; recover the engine API they call | ✅ **70 subsystems** |
| **P1c** | Recon the rest: string map, RTTI, the `Cool*` stack | ✅ [`docs/RECON.md`](docs/RECON.md) |
| **P2** | Function recovery over 7.64 MB of `.text` | ✅ **76,528, scored** |
| **P3** | Lift to C | ✅ **0 errors, 4.85M lines** |
| **P3b** | Import bridges for 479 imports; build | ✅ **compiles, 156 objects** |
| **P4** | Bring-up: `InitClientDLL` | ✅ **completes** |
| **P5** | Win32 layer + a real window | ✅ **71 shims, window up** |
| **P6** | The LithTech engine interfaces `RunClientDLL` needs | 21 holders / 8 impls; **need the bind step** |
| **P7** | **The socket layer — 20 functions, UDP** | scheduled early |
| **P8** | Renderer: D3D9 through COM vtables | deliberately late |

### P3: it lifts clean

```
$ python run_lift.py --all --split 500
  base=0x10000000 code=0x10001000-0x1074AD7A IAT=479
  catalog: 76528 functions, 85297 call edges
  roots: InitClientDLL@0x10001270, RunClientDLL@0x10001120,
         TermClientDLL@0x10001140, ErrorClientDLL@0x10001170,
         SetMasterDatabase@0x101F1DB0, DllMain@0x105C3736
  76528 functions in 154 files, 76528 lifted, 0 stubs, 0 errors, 49s
```

**4,854,290 lines of C, zero lift errors.** For scale, that is roughly ten
times Fury³ and about five times Crimson Skies, and it is the largest clean
lift this toolchain has produced.

Zero errors is a weaker claim than it sounds and is worth stating precisely: it
means every function decoded and emitted C without the lifter faulting. It does
not mean the C is correct, and nothing has been executed. That is P3b and P4.

[`run_lift.py`](run_lift.py) is noticeably shorter than the sister projects'
drivers, for one reason worth recording. Force Commander needs a branch-walking
`true_extent()` because `disasm32` records a *reachability bound* rather than an
extent — trusting it there decoded a megabyte of unrelated code as a single
function body. IDA records real ranges, so `f['end']` is used directly and the
whole mechanism disappears. The residual risk is different and smaller: IDA's
`end_ea` ends a function's *main chunk*, so a function with separated tail
chunks is under-read rather than over-read. That fails loudly at runtime as a
missing tail instead of silently swallowing its neighbour.

### P4: it runs, and `InitClientDLL` completes

```
$ build/mxo_trace.exe --init
  mapped game/client.dll: 0x10000000 + 10653696 bytes
  TIB:   0x0E800000 (fs base)
  IAT slots patched:     479

  DllMain(0x10000000, DLL_PROCESS_ATTACH, NULL) ...
    [_initterm] 0x1083B000..0x10847608: ran 12189 initialiser(s)
  DllMain returned eax=0x00000001, esp=0x0E3FFF00

  calling 0x10001270 ...
  returned, eax=0x00000000

  indirect calls: 64377      exit 0
```

**The recompiled client starts up.** 12,189 C++ static constructors run, and
`InitClientDLL` returns cleanly 64,377 indirect calls later.

Getting there took five distinct failures, and the shape of them is the useful
part — every one was silent, and none faulted anywhere near its cause.

**1. No TIB.** `InitClientDLL`'s first call opens with MSVC's SEH prologue,
reading and replacing `fs:[0]`. With `g_fs_base` at zero that dereferences
address 0 before anything useful happens. The host now builds a simulated TIB.

**2. Unlifted entry points.** The lifted `eh vector constructor iterator`
contained a *direct* `call` to an address the catalog did not have — an MSVC EH
funclet. The lifter emits `ICALL(const) /* not lifted */` for that, the runtime
resolves it to nothing, pops the frame, returns 0, and execution carries on with
garbage. A direct call is *proof* a function starts there, so `run_lift.py` now
scrapes its own output for that marker into `analysis/missing_targets.json` and
re-seeds. Converges in two passes.

**3. Stub imports returning 0.** A stub returning zero is not a neutral
placeholder — zero is a plausible-looking answer for most of the CRT.
`malloc` returning 0 faulted on the dereference; the fix is 121 real CRT bodies
in [`src/runtime/shims_crt.c`](src/runtime/shims_crt.c) plus Win32 in
[`shims_impl.c`](src/runtime/shims_impl.c).

**4. The instrumentation hid the bug.** The first traced build died with
`STATUS_HEAP_CORRUPTION` inside ntdll instead of an access violation — the worst
failure to chase, since the damage happens at one call and is detected at an
unrelated one. The allocator shims now refuse any pointer they did not hand out
and leak it instead; `_msize` reading a foreign heap header was the likely
culprit.

**5. `_initterm` was a stub, and that was the real one.** `_initterm(pfbegin,
pfend)` walks a table of function pointers and calls each — which is how C++
static constructors run, and on a binary this size that is most of program
initialisation. Stubbed, it does nothing at all, so every global a constructor
would have built stayed the zero it was mapped as. That surfaced as a NULL
string pointer four calls into `InitClientDLL`, in code doing a perfectly
ordinary lazy-init check.

It is also the one shim that has to **call back into lifted code** rather than
out to the host: the table entries are VAs of recompiled functions.

Two things follow from that, and they cost several rounds to learn:

* **`DllMain` must run before any export.** Calling `InitClientDLL` on its own
  looks reasonable and skips the entire CRT startup path.
* **A stub is a lie with a plausible return value.** Every one of these five
  failed far from its cause. The host's per-import and per-address reporting
  exists so a run ends with a worklist rather than a stack trace.

What this does *not* mean: nothing renders, there is no window, and
`RunClientDLL` — the frame loop — has not been called yet. That is P5.

### P3b: 479 imports bridged, and the whole thing builds

```
$ python gen_imports.py
  imports: 479 across 19 DLLs
    stdcall  226  (derived argc)
    cdecl    237  (pop 0)
    data      16  (storage, not shims)

$ cl /c /MP src\recomp\gen\*.c src\runtime\imports_gen.c
  EXITCODE=0     156 object files, 197 MB
```

**Every generated file compiles under MSVC 2022 x86 with zero errors.** That is
a real check on the lift rather than a restatement of it: 4.85 million lines of
machine-written C that a compiler accepts.

The import layer has three cases and the third one is a trap worth naming.

**stdcall (226)** — the callee pops, so a shim must pop exactly right. Getting
it wrong does not fault at the call; it shifts the *caller's* frame, so the
caller then reads its locals from the wrong slots and dies somewhere unrelated.
None of these counts are typed by hand: `stdcall_argc.py` derives each from the
`_Name@N` decoration in the Windows SDK import libraries.

**cdecl (237)** — the caller cleans up, so these pop nothing, and the resolver
*correctly* fails to derive a count because there is none. 181 come from
`MSVCR71.dll` and 56 from `pythonMXO.dll`.

**Data (16)** — the trap. Some of what the client imports is not code.
`pythonMXO.dll` exports 693 symbols from `.text` and **115 from `.data`**:
`PyType_Type`, `PyString_Type`, `PyExc_AttributeError`, `_Py_NoneStruct`,
`_Py_TrueStruct`, `_Py_ZeroStruct`. The client *dereferences* those slots. A
function shim there would not fail at build time and would not fail at the call
site — it would hand the client a code address where a `PyTypeObject` belongs,
and the first type check would read whatever the shim's prologue encodes. So
data imports get storage and their own table, never a stub. (`MSVCR71`
contributes three more: `_iob`, `__pioinfo`, `_adjust_fdiv`.)

Two bugs this turned up, both of which would have been silent:

* **Classify by section characteristics, never by section name.**
  `binkw32.dll` puts its decoders in about thirty custom-named *executable*
  sections — `BINK`, `BINK16`, `BINK4444`, `BINK32R` — all with
  characteristics `0x60000020`. A `.text`-only test called eight of its exports
  data and would have handed the client storage where Bink's entry points go.
* **Export names are not C identifiers.** This binary imports both kinds that
  break: MSVC C++ mangling (`?terminate@@YAXXZ`) and stdcall decoration
  (`_BinkOpenDirectSound@4`). Emitted verbatim they produce `unknown character
  '0x40'` by the hundred. `c_ident()` sanitises with collision handling and
  keeps the real name in the stub string, so a stub report still prints what
  was actually called.

### P2: 56,287 functions, and a very small door

IDA (9.1, headless via `idalib`) finds **56,287 functions** — more than any
other target in this collection, Rise of Legends included. The distribution
says why:

| | |
|---|---:|
| functions | **56,287** |
| byte coverage of `.text` | 75.2% |
| median function size | **28 bytes** |
| under 32 bytes | 50.9% |
| FLIRT-identified library | 2,101 |
| thunks | 856 |
| largest function | 48,462 bytes |

Half of all functions are under 32 bytes. That is STLport: the `STLPORT_`
section seen in P0 cashes out as tens of thousands of inlined container
accessors. The real game is a much smaller number of large functions, and
separating the two is the whole P2 problem. `analysis/engine_api.json` — the 70
subsystems the Python calls — is the list that says which ones matter.

**RTTI is a dead end here.** 7 type descriptors, 1 class. Unlike Force
Commander, which handed over 567 classes for free, this build shipped without
it. No shortcut to class recovery.

#### IDA is not ground truth on this target, and scoring proved it

The house rule is to score recovery before lifting. Doing it here produced a
result worth the 40 minutes `disasm32.py` took to finish:

| | IDA 9.1 | `disasm32.py` |
|---|---:|---:|
| functions | 56,287 | **78,105** |
| byte coverage of `.text` | 75.2% | **95.9%** |

Scored against IDA as reference, `disasm32` looks bad — 63.7% precision, with
20,241 starts "invented" outside every known function. The toolbox's own
documentation says not to stop there: *a reference is only ground truth if it
came from symbols, otherwise it is a second opinion, and a disagreement means
one of the two is wrong — go look at which.*

Two things say it is IDA that is incomplete:

* Where both tools agree a function exists, **they agree on its end 99.39% of
  the time** (46,332 of 46,616 exact). This is not two tools disagreeing about
  the same code; it is one tool seeing code the other never looked at.
* `disasm32`'s own log explains where it found them: **~19,000 functions
  reachable only through data pointers.** On an MSVC C++ binary that means
  virtual methods nothing calls directly. IDA's auto-analysis is conservative
  about those and never promotes them to functions — the same "vtable-only
  functions" pattern the toolbox records for Rise of Legends.

Measured properly, the extras fill **60% of the 1.9 MB IDA leaves
unattributed**. So the first full lift, driven by IDA alone, silently stubbed
about twenty thousand real functions.

One correction worth recording because it nearly became the conclusion:
naively summing the extras' sizes gives 6.28 MB against a 1.9 MB gap — 330%,
which is impossible. `disasm32` records a *reachability bound*, not an extent,
so those ranges overlap each other and run on into functions IDA already has.
Only the merged union, intersected with the gap, means anything.

`tools/ida_to_catalog.py --augment` now merges the two: IDA's extents where it
has them, `disasm32`'s starts where it does not, each clamped to the next known
start — the only bound available that cannot swallow a neighbour.

```
76,528 functions  39,401 with call edges  85,297 edges
from IDA: 56,287   from disasm32 gap fill: 20,241 (853,418 bytes)
```

**But the door is tiny.** `client.dll` exports **10 symbols**, and five are the
LithTech client-DLL contract:

```
InitClientDLL    RVA 0x1270      RunClientDLL   RVA 0x1120
TermClientDLL    RVA 0x1140      ErrorClientDLL RVA 0x1170
SetMasterDatabase RVA 0x1F1DB0
```

The other five are `ILTCool` — constructor, copy constructor, assignment,
destructor and vtable — which confirms that the `Cool*` DLL stack from P0 is a
LithTech engine interface rather than a bolted-on third-party library.

Four entry points is a far better bring-up target than a 7.6 MB binary with one
`DllMain`. It is the same shape that got Force Commander to its splash screen.

P1b was the pivot, and it paid off. The scripts name the engine objects they
call but never import — those are the C++ subsystems inside `client.dll`, and
`analysis/engine_api.json` now lists **70** of them with call counts and
method counts. Ranked by how many modules use each:

| Subsystem | modules | methods | |
|---|---:|---:|---|
| `discovery` | 25 | 21 | |
| `AbilityInv` | 18 | 41 | ability inventory |
| `constants` | 17 | 12 | |
| `CharMvt` | 13 | 21 | character movement |
| `Interlock` | 12 | 28 | the combat minigame |
| `consolevar` | 11 | 24 | |
| `combat` | 7 | 34 | |
| `Inventory`, `AI`, `PlayerCharacter`, `abilitylib`, `physics`, `Organization`, `MissionTeam`, `rewardselection`, `StateMachine`, `Chat`, … | | | |

That is the lift-priority list for P2/P3, and it is a far better one than
"start at the entry point and work outwards" on a 7.6 MB binary.

Four modules the scripts import were **not** in `Monolith.ltp`. Three are now
accounted for:

* `ltfxmap` is in the archive, just deferred — a quarter-megabyte of effect
  table that takes the decompiler longer than the other 56 modules combined.
* `stringtable_client` and `st_neighborhoodnames` live in a **third archive**,
  `resource/string_tables/Strings.ltp` — 22 more modules in the same LTPb
  format. `stringtable_client` alone is 2.2 MB uncompressed: every string the
  game displays, and with it the mission text, hardline names, neighbourhood
  names and cinematic subtitles.
* `RewardSelection` is still unaccounted for, and `rewardselection` also shows
  up in the engine-globals list — so it is almost certainly a C++ subsystem
  rather than a missing script.

Total recovered so far: **404 Python modules across three archives**, of which
79 are the game (57 + 22) and 325 are the Python 2.3 standard library.

**P2 gets scored before anything is lifted.** `disasm/score_recovery.py`
against an IDA export, splitting false positives into *split* and *invented* —
they need opposite fixes, and finding the gaps at runtime 30,000 calls deep is
how this goes wrong. On a target this size that discipline is not optional.

### Track B — the server

| Phase | What | State |
|---|---|---|
| **S0** | Survey prior art, settle the licence position | ✅ done — `docs/SERVERS.md` |
| **S1** | **Fork mxo-hd**; fix the two licence/provenance defects | next |
| **S2** | Auth + character select, verified against the real client | |
| **S3** | World: districts, statics, movement, doors, hardlines | |
| **S4** | Combat, from the recovered rules rather than reconstruction | |
| **S5** | **Missions.** The part nobody has finished. | |

S1 depends on nothing and can start immediately. The two tracks meet at
**step 3** of [`docs/ROADMAP.md`](docs/ROADMAP.md): the recompiled client
pointed at the forked server, which is the moment this stops being two efforts.

### Fork, don't rebuild

mxo-hd already has auth, character select, three districts, movement, chat,
doors, hardlines, vendors and inventory working. Rebuilding that to arrive at
the same place would be months spent on the part that is *already solved*, and
the ladder says reuse it. So: **fork mxo-hd for the transport and world layer,
and replace its rules layer with what the client actually says.**

That division falls out of the evidence rather than taste. Its network and
world plumbing was built from packet captures, which is the right way to
recover a wire format. Its combat was reconstructed from debug strings, which
is the wrong way to recover a formula — and `docs/COMBAT.md` shows where that
lands. Its mission system is early-stage and the three mission XML files it
ships are not read by anything. Those are exactly the pieces P1 just recovered.

Two defects to fix on the way in, neither a blocker:

1. **The `LICENSE` file is AngularJS's** — *"Copyright (c) 2010-2017 Google,
   Inc. http://angularjs.org"*, evidently pasted in. The intent to be MIT is
   unambiguous, but the notice names the wrong copyright holder, so it grants
   rights over AngularJS rather than over mxo-hd. Worth raising upstream and
   getting corrected; until then our fork's MIT covers our own changes and the
   upstream defect is documented rather than papered over.
2. **`WorldEncryptionEnigma.cs` P/Invokes `enigmaLib.dll`**, a closed-source
   native library derived from rajkosto's work. The good news is that it is a
   *binary dependency*, not vendored AGPL source — so mxo-hd's own code is not
   contaminated and the AGPL question does not reach us. But a fork inherits a
   blob we cannot rebuild or relicense. There is a managed `WorldEncryption.cs`
   beside it; prefer that path, and verify the crypto against `client.dll`
   directly — it imports `CryptGenRandom` and `CryptAcquireContextA`, so the
   client can settle what the handshake actually does.

Track B leans on Track A from S4 onward. S2 and S3 come from the fork; S4 and
S5 are precisely the parts that could not be built from packet captures, and
they are the reason to do this at all.

### About the endless part

This is an MMO. It will not be "finished", and that is fine — it is the point.
The milestones are chosen so each one is worth something on its own: a scored
function catalog is worth having even if nothing lifts; a documented protocol is
worth having even if no server ships; one working mission is worth having even
if the other several hundred never do.

---

## Open questions

Carried here until answered, then moved into the docs with the answer.

1. **Which client version should the server target?** These discs are retail
   1.0 (February 2005). The emulators target the final live client, four years
   of patches later. The protocol almost certainly moved. Retail 1.0 is a
   legitimate and possibly *easier* target — but it is a different target, and
   the choice needs making deliberately rather than by default.
2. **Where is the game executable?** Retail ships `launcher.exe` + `client.dll`
   and no `matrix.exe`. Either the launcher hosts `client.dll` directly, or the
   real host arrives via the patcher. `launcher.exe`'s imports will say.
3. ~~How much of the game is Python?~~ **Answered: 57 modules, covering
   combat, abilities, interlock, crafting and missions.** The follow-on
   question is how much *engine* sits under them — which is P1b.
4. Are the `Cool*` DLLs a second protocol a server must also speak?
5. ~~Do the combat formulas actually live in the client?~~ **Answered: yes, and
   as Python bytecode rather than machine code.** Still open: whether
   `serverrules.pyc` is what its name suggests, and which side of the wire each
   rule is actually evaluated on. A rule the client merely *predicts* and a rule
   the client *decides* are very different things for a server.
6. Do these retail-1.0 scripts match the final live client the emulators
   target? Four years of patches sit in between, and gameplay rules are exactly
   the sort of thing that gets rebalanced.

---

## Layout

```
analysis/     derived metadata — PE facts, catalogs, symbol tables. Committed.
docs/         SERVERS.md (prior art) and the recon documents to come
original/     your disc images                          (git-ignored)
disc/         ISO contents                              (git-ignored)
game/         the installed client                      (git-ignored)
src/recomp/   lifted C — derivative of client.dll       (git-ignored)
work/         scratch                                   (git-ignored)
```

## Requirements

Python 3.10+ with `capstone` and `pefile`; the
[pcrecomp](https://github.com/sp00nznet/pcrecomp) checkout at `../tools`. IDA or
Ghidra for the P2 reference catalog. CMake 3.20+ and MSVC 2022 once there is
something to build.

## Licence

**MIT** — see [LICENSE](LICENSE), which also spells out the three things MIT
cannot cover here: the game itself, the lifted output, and the AGPL emulator we
read but never copy.

**No game files are in this repository and none ever will be.** Bring your own
discs, drop them in `original/`, and the pipeline unpacks them itself. The
Matrix Online is still in copyright; an MMO whose servers were switched off is
abandoned, not public domain.
