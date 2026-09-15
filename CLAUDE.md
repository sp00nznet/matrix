# The Matrix Online Static Recompilation — Project Memory

## ✅ P6 IS NOT BLOCKED — the bind step is what's missing
**Earlier note here claimed `matrix.exe` was a missing engine binary. That was
WRONG.** A full post-patch client (`MxO_7.6005`, in `~/Downloads`, 2,666 files)
has no `matrix.exe` either, and no binary in it besides `client.dll` carries the
engine interface names (checked: `launcher.exe`, `r3d9.dll`, `mxowrap.dll`,
`cres.dll`). Retail vs final `client.dll` counts are near-identical
(`INetMgr` 10/10, `ILTRenderDevice` 4/4) — architecture unchanged.

**`sub_101F1A00` registers BOTH sides, told apart by the flag:**
```
push 0 ; push "Name" ; push holder   -> declare a holder awaiting fill
push 1 ; push "Name" ; push object   -> register an IMPLEMENTATION
```
**55 call sites**, not 28 — `find_interfaces.py` only caught immediate-pushed
operands. Properly counted: **21 holders, 8 implementations**, implementations
supplied by `client.dll` itself:
`ILTLoginMediator.Default` (by `sub_10001270` = **`InitClientDLL`**, and
`sub_1070F500`), `ILTInputMgr.Default`, `ILTMultiProtocolMgr.Default`,
`ILTPredictViewMgr.Default`, `ILTRsetManager.Default`,
`ILTWorldRep_Metro.Default`/`.Internal`.

**So even self-implemented interfaces were NULL because the BIND never ran.**
Holders and impls both land in an internal database; something must match them
by name. **Next: find and call the bind/resolve entry point.**
`SetMasterDatabase` (export, RVA `0x1F1DB0`) is the obvious candidate — handing
the client a database is exactly that shape. Neighbours worth reading:
`sub_101F18E0`, `sub_101F1830`, `sub_101F1220`.

**`INetMgr.Default` has NO implementation among the 8** — so it either comes
from one of the 27 register-sourced call sites, or genuinely from outside.

Assets: final client at `~/Downloads/MxO_7.6005.7z` (1.3 GB), patch
`MxO_7.6004p.zip`; key binaries extracted to `work/final/`. Version parity with
what the emulators target ⇒ the roadmap's biggest risk is retired.

## PLAN OF RECORD — `docs/ROADMAP.md`
The two tracks are **not** parallel: P6 (engine interfaces) and the server's
real task are the **same boundary from two sides**. The 70 subsystems in
`analysis/engine_api.json` are what the client wants from its engine *and* what
a server is the authoritative half of. Build each shared artifact once.

**The lever: the client can talk long before it can draw.**
`WSOCK32` is **20 functions** — `sendto`, `recvfrom`, `bind`, `select`,
byte-order helpers — with **no `connect`/`send`/`recv`/`listen`/`accept`**, so
the world protocol is **UDP** and the whole surface is an afternoon.
Rendering is D3D9 through COM vtables + an asset pipeline. **So: sockets early
(P7), renderer deliberately late (P8).**

Ordering (each step unblocks the *other* track):
1. **S1** fork mxo-hd — depends on nothing, start anytime.
2. **P7/SP1** shim the 20 sockets + packet log.
3. **Point the recompiled client at the forked server on `127.0.0.1`** —
   `gethostbyname` and `inet_addr` are already shimmed. Closed loop under our
   control. *This is where it stops being two efforts.*
4. Differential protocol work (S2/S3 vs the real client's own code).
5. Rules sidecar (`interlock/` + `ability/`) → S4.
6. Missions → S5 (contract already recovered: 12 callbacks, 21 verbs).

**Biggest risk, cheap to retire early:** these are Feb 2005 retail discs; the
emulators target a client four years of patches later. Step 3 settles it — which
is the other reason sockets come before the renderer.
**Open:** no TCP anywhere in `client.dll`, yet the emulators' auth is TCP ⇒ auth
may live in `launcher.exe`. `WININET` (wide-char HTTP) is a separate channel,
probably patch/news.

## P6 SOLVED IN SHAPE — 28 named engine interface holders
**The client does not create its engine interfaces.** It declares **holders**
(vtable at `+0`, interface pointer at `+4`) and waits for the engine to install
the pointer. `+4` stays NULL until something does. That is the entire P6 wall.

- Proven with `MXO_WATCH=0x10857788,...` (host dumps guest memory at phase
  boundaries): vtable `0x10780D38` set by a static ctor, `+4 = 00000000`
  unchanged across `DllMain` and `InitClientDLL`. `RunClientDLL` faults at
  `0x103065E0` calling through it.
- Registered by static initialisers shaped `register(holder, "Name", flags)` →
  `sub_101F1A00`. **`tools/find_interfaces.py`** recovers all of them statically
  into `analysis/engine_interfaces.json`. **28 holders.**
- **Networking (7):** `INetMgr.Default` ← *the one that crashes*,
  `INetShell.Default`, `ILTMultiProtocolMgr.Default`,
  **`ILTDistrObjExecutive.Default`** (client half of server object replication),
  `CLTRemoteCommCtxMgr.Default`, `ILTCommHandleMgr.Default`,
  `ILTReactorHandleMgr.Default`.
  **Login:** `ILTLoginMediator.Default`.
  **Objects:** `ILTGOGameObjMgr`, `ILTGOComponentFactory`, `ILTObjectMgr`,
  `ILTClassMgr`, `ILTRsetManager`. **World:** `ILTWorldRep_Metro.Default`/
  `.Internal` (Mega City = "Metro"). **Render/UI:** `ILTRenderDevice`,
  `ILTRenderStyles`, `ILTTexture`, `ILTDrawPrim.Default`/`.Internal`,
  `ILTFontManager`, `ILTWidgetManager`. **Input:** `ILTInputMgr`,
  `LTWinInputMgr`. **Misc:** `ILTEventScheduler`, `ILTPredictViewMgr`,
  `ILTDiagnosticMgr.DObSys`, `ICompress`.
- **Why the socket layer never fired:** `WSAStartup` isn't called during
  `InitClientDLL` because `INetMgr.Default` was never installed. Network is
  *behind* this wall, not beside it.
- **P6 is now concrete:** per holder, supply an object whose vtable has the
  methods the client calls, install the pointer at `+4`. Missing methods report
  themselves like missing imports. **Do `INetMgr` first** — it is what
  `RunClientDLL` reaches first, and it unblocks the whole server track.

### P6 mechanism WORKS — `src/runtime/engine_iface.{c,h}`
- Vtable slots hold **synthetic VAs** (`0x7E000000 + iface*4096 + slot*16`),
  resolved by **`recomp_lookup_manual`** — the hook that returned NULL from the
  start. A host function pointer cannot go in a vtable: dispatch knows only
  lifted VAs and IAT slots, and a host function would want args on the *host*
  stack.
- **Unimplemented slot reports and `exit(0)` — never guesses the pop count.**
  No SDK to derive arity from for a C++ virtual; a wrong pop shifts the
  *caller's* frame and crashes elsewhere. One worklist entry per run; get arity
  by reading the call site.
- **`MXO_IFACES=INetMgr` / `all` / unset(none). Default none — installing all 28
  is WORSE than none.** A non-NULL holder sends the client down its
  "subsystem available" path, where it chases pointers *through* our object; a
  bare vtable over zeroed members is callable but not usable. All-28 turned a
  completing `InitClientDLL` into a crash in `ILTLoginMediator` (holder
  `0x109961D8`, deref `+0x58`). Regression-checked: none installed ⇒
  `InitClientDLL` still completes at 65,029 calls.
- **First slot reached: `INetMgr.Default :: slot 3` (vtable +0xC)**,
  `this=` our object, caller near `0x100064A0`. Next: read that call site for
  arity, implement, re-run for the next slot.
- `MXO_WATCH=0x...,0x...` dumps guest memory at phase boundaries.
- `find_interfaces.py` emits `src/runtime/interfaces_gen.c` alongside the JSON.

### P6 slot semantics — the COM refcount idiom
**These interfaces are COM-shaped. `vtable[1] = AddRef`, `vtable[2] = Release`,
throughout.** The recurring getter idiom:
```
p = slot_n();            ; p points AT an interface pointer
if (*p != cur) { if (cur) cur->vt[2](); cur = *p; if (cur) cur->vt[1](); }
```
So such a getter returns a **pointer-to-pointer**, and `RET_PTRNULL` (pointer to
a NULL word) answers it cleanly — "no object" is a state the client handles.

**Arity is mechanical** (count `PUSH32` between `ecx = this` and the ICALL,
excluding the prologue's register saves). **Return *type* is semantic** — read
what the caller does with the result.

**NEVER invent an object for a result nobody reads.** Slot 3's caller clears a
local on the next instruction, so its result is unused ⇒ `RET_ZERO`. Using
`RET_OBJ` there invited the client to use a fabricated object whose zeroed
members were then read as function pointers → `ICALL: unresolved VA
0x0099CCA0` (our own allocation, called as code) and `unresolved VA 0x00000000`.
`ecx=DEAD0000` in a fault dump = the dummy return address leaked into a
register = stack desync.

Implemented so far: `INetMgr` slot 3 (argc 0, RET_ZERO), slot 15 (argc 1,
RET_PTRNULL). The host names synthetic-VA-dereferenced-as-data faults
explicitly.

**Limit reached:** generic scaffolding gets every question asked precisely and
cannot answer them. Past a couple of slots the client needs objects whose
*shape* matches expectation — per-interface semantic work, not a default.

## The server fork — `../mxo-hd-recomp`
**Private mirror**, not a GitHub fork: `sp00nznet/mxo-hd-recomp`, checked out at
`G:/recomp/pc/mxo-hd-recomp`, `upstream` → `hdneo/mxo-hd` for syncing.
**`gh repo fork` cannot be used** — forks of public repos are always public and
cannot be made private. Upstream: 51 commits, last 2026-04-02 (combat
data-model merge, i.e. the area we replace). Two large CSVs (72 MB, 60 MB)
exceed GitHub's recommended file size; they pushed fine.
Still to do: correct the AngularJS `LICENSE`; prefer managed
`WorldEncryption.cs` over the `enigmaLib.dll` binary.

## The socket layer — `src/runtime/shims_net.c` (P7, built)
20 functions, UDP, forwarding to real Winsock (client holds genuine handles).
`gethostbyname` + `inet_addr` answer with the loopback sidecar **and** `sendto`
rewrites its destination — belt and braces, so a cached/hard-coded address can't
escape and make the log meaningless. Logs every datagram (peer, length, hex
window) to `work/packets.log`. Env: `MXO_SERVER`, `MXO_PACKET_LOG`,
`MXO_PACKET_BYTES`. Link needs `ws2_32.lib`; `winsock2.h` **before**
`windows.h`.
Shape borrowed from **tstorecomp**: server inside the app as a loopback sidecar
started at launch, not a service beside it.

**Ordering fact, found by building it:** the client never calls `WSAStartup`
during `InitClientDLL` — network init is behind the **same P6 wall** as
`RunClientDLL`. Sockets are *not* gated on the renderer, but they *are* gated on
P6. So **P6 is on the critical path for both tracks.**

## Standing Directive
Keep the README's status table and numbers current with every phase. Commit and
push to `main` on GitHub regularly. Repo is **private**.

## Project Overview
Two tracks. **A:** static recompilation of the retail `client.dll` (2005,
Monolith / SOE) to C. **B:** an MIT-licensed server that implements the rules
read out of the lifted client, rather than replayed from packet captures.
Track A exists to make Track B possible — the recomp is the instrument, the
server is the goal.

## Key Paths
- **Repo root**: `G:/recomp/pc/matrix`
- **pcrecomp toolbox**: `G:/recomp/pc/tools` (scripts live in `tools/tools/`).
  Referenced, never vendored.
- `original/` discs, `disc/` ISO contents, `game/` installed client — all
  git-ignored, 5.2 GB, never committed.

## Decision of record: fork mxo-hd for the plumbing, replace its rules
Its network/world layer came from packet captures (the right method for a wire
format) and works: auth, character select, three districts, movement, chat,
doors, hardlines, vendors, inventory. Its combat came from `.rdata` debug
strings (the wrong method for a formula) and is wrong in ways `docs/COMBAT.md`
documents; its mission system is early-stage and its three mission XMLs are
read by nothing. **Fork the first, replace the second from the recovered
Python.** Do not rebuild S2/S3 from scratch.

Two upstream defects to handle, neither a blocker:
- **mxo-hd's `LICENSE` is AngularJS's** ("Copyright (c) 2010-2017 Google, Inc.
  http://angularjs.org"). Intent to be MIT is unambiguous but the notice names
  the wrong holder. Raise upstream; meanwhile document it rather than ignore it.
- **`WorldEncryptionEnigma.cs` P/Invokes `enigmaLib.dll`**, a closed native lib
  derived from rajkosto's work. It is a *binary dependency, not vendored AGPL
  source*, so mxo-hd's own code is clean and the AGPL question does not reach
  us — but a fork inherits an unrebuildable blob. Prefer the managed
  `WorldEncryption.cs`, and verify crypto against `client.dll` (it imports
  `CryptGenRandom`, `CryptAcquireContextA`).

## The licence rule — read this before touching prior art
- **`rajkosto/mxoemu` is AGPL-3.0.** The licence file is
  `Reality/Source/LICENCE.TXT`, which is why GitHub's sidebar shows no licence
  and why it gets misreported as unlicensed. **Never copy, adapt or translate
  any of it.** Read it for facts only; record in `docs/SERVERS.md` when a fact
  came from there and how it was re-derived from the binary.
- **`hdneo/mxo-hd` is MIT.** Usable with attribution in the file that uses it.
- Do not adopt mxo-hd's SQL schema wholesale — it is named `reality_hd.sql`
  against mxoemu's `reality.sql` and the provenance is unresolved. Derive ours
  from the client's object model.
- Lifted C is a derivative work of `client.dll`. It goes in `src/recomp/gen/`
  and is git-ignored. Derived *metadata* (addresses, vtables, imports) is fact,
  and is committed to `analysis/`.

## Hard-Won Facts (do not re-derive)
- **`client.dll` is the game.** 9,515,008 bytes, `.text` 7,642,490, image base
  `0x10000000`, entry `0x105C3736`, MSVC 7.1, built 2005-02-18. Second-largest
  target in the collection after Rise of Legends.
- **No DRM, no packer.** `.text` entropy 6.04, no indicators. Static analysis
  works directly — no runtime dump needed.
- **Full `.reloc`, 497,068 bytes.** It is a DLL so it must relocate; the lifter
  gets relocation data instead of inferring it.
- **479 imports across 19 DLLs.** Small, fixed surface.
- **Built against STLport**, not the Microsoft STL — both `client.dll` and
  `launcher.exe` carry a `STLPORT_` section. Expect heavy template expansion;
  a large share of the 7.6 MB is container code, not game logic. Classification
  will matter.
- **The gameplay rules are Python 2.3 bytecode, not machine code.** This is the
  single most important fact about this target. `client.dll` imports the Python
  C API from `pythonMXO.dll` (`Py_Initialize`, `Py_InitModule4`, `PyRun_String`,
  `PyObject_CallObject`, `PyUnicodeUCS2_*`).
- **`.ltp` format is solved**; `tools/ltp_extract.py` reads it and has a
  `--selfcheck` that runs without game files. Not one archive with an index:
  single-entry records concatenated, each with an 18-byte header
  (`LTPb`, u32 20, u16 8, u16 0x08bc, u16 0x3252, two per-record u16s), then
  u32 csize / u32 usize / u32 namelen / name / raw-deflate payload. A redundant
  `LTPd` directory and one `LTPh` follow the data records — stop the walk there.
- **382 modules: 325 are the Python 2.3 stdlib (ignore), 57 are the game.**
  `Monolith.ltp` holds the 57: root (`combat`, `combatvalidate`,
  `combat_defines`, `serverrules` 53 KB, `clientrules`, `experience`,
  `abilities`, `augments`, `recipes`, `crafting_server`, `missionvalidate`,
  `generic_free_attacks`, `tempmods`, `emotes`, `trgs`, `clhooks`, `rshooks`,
  `ltfxmap`), `ability/` (24: hacker/soldier/spy/npc trees), `interlock/` (7,
  including **`combat_resolution`**), `Missions/` (4). Manifest committed at
  `analysis/python_modules.json`; extracted bytecode lives in `work/py/` and is
  **never committed** — it is the game's own code.
- **The community's "combat formulas are buried in the client exe" claim is
  half right**: the formulas are in the client, but as decompilable bytecode,
  not as functions anyone needed to disassemble. Nobody appears to have looked.
- The recomp is still needed: the engine under the scripts is 7.6 MB of C++,
  and `Py_InitModule4` is the seam. Every module the engine registers is a C++
  subsystem the scripts drive — recovering that interface is the real P2/P3
  target, and the script call sites give the priority order.
- **All 56 attempted modules decompile cleanly** with `uncompyle6` (10,026
  lines). `ltfxmap.pyc` is deferred: 253 KB of effect-table literal that takes
  uncompyle6 longer than the other 56 combined. `tools/decompile_ltp.py` gives
  it a longer budget and `--skip-slow` defers it.
  **`python -m uncompyle6` does not work** — it is a package with no
  `__main__`; drive `uncompyle6.main.decompile_file` instead.
- **70 engine subsystems recovered** into `analysis/engine_api.json` by
  `tools/engine_api.py`. Ranked by module breadth (not call count — see below):
  `discovery` 25 mods/21 methods, `AbilityInv` 18/41, `constants` 17/12,
  `CharMvt` 13/21, `Interlock` 12/28, `consolevar` 11/24, `combat` 7/34, then
  `Inventory`, `AI`, `PlayerCharacter`, `abilitylib`, `physics`, `Organization`,
  `MissionTeam`, `rewardselection`, `StateMachine`, `Chat`, `Construct`.
  **This is the P2/P3 lift-priority list.**
- **Rank engine globals by module breadth, never by call count.** `subject`,
  `directObject` and `indirectObject` are ability-function *parameters*
  (ability signatures use a subject/directObject convention) and they top the
  raw call counts. `mission` at 480 calls lives in only 2 modules. The tool
  flags any name that is bound as a local or parameter elsewhere.
- **There is a third archive**: `resource/string_tables/Strings.ltp`, 22 modules,
  same LTPb format. It holds `stringtable_client` (2.2 MB uncompressed — every
  displayed string in the game) and `st_neighborhoodnames`, plus `st_aim`,
  `st_hardlinenames`, `st_floornames`, `st_elevatornames`, `st_poi_*` and the
  `st_cin_*` cinematic subtitle tables. **404 modules recovered across three
  archives**: 57 game + 22 strings + 325 stdlib.
- `RewardSelection` remains unaccounted for as a script, and `rewardselection`
  appears in the engine-globals list — so it is a C++ subsystem, not a missing
  module. Lead closed.
- `Strings.ltp` decompiles 21/22; `stringtable_client` (2.2 MB) times out. It is
  content, not logic — leave it as bytecode, nothing needs its source.

## P1 is complete — `docs/RECON.md` is the primary reference
- **The mission system is fully mapped.** `Missions/Mission.py` is a base class
  with **12 callbacks** (`OnStartOfMission`, `OnStartOfStage`, `OnGoalComplete`,
  `OnPhaseComplete`, `OnMissionEvent`, `OnObjectTargeted`, `OnPlayerEnterZone`,
  `OnModalDialogueAccepted`, `OnToggledViewActivated`, `OnInventoryItemsSwapped`,
  `OnShortcutAdded`, `OnMovementUpdated`). Structure is
  **mission → phase → stage → goal**, team-based throughout. Two complete
  missions ship (465 and 871 lines), so we have worked examples of the idiom.
- **The `mission` engine object is the quest verb set — 21 methods.** Flow
  (`activateStage`, `completeGoal`, `isGoalComplete`, `sendMissionEvent`,
  `scheduleEvent`), narrative (`playNarration`, `broadcastMessage`,
  `sendModalMessage`, `toggleView`), world (`sealDoor`, `unlockDoor`,
  `getDoorZoneIDs`, `teleportToMissionElement`, `setExclusiveWaypoint`,
  `playLocalFX`), NPCs (`killNPC`, `respawnNPC`, `unkillNPC`), player
  (`grantExperience`, `addTempAbility`, `bootPlayer`).
  **Implementing those 21 + the 12 callbacks is most of implementing quests.**
- Mission keys are structured (base + part); the same comparison decides team
  membership and PvP opposition (`missionvalidate.py`).
- Rewards are **encoded** and resolved against loot tables —
  `rshooks.RunMissionRewards(encodedReward, subject)` plus the
  `rewardselection` engine object.
- `sponsors.xml` names the internal authoring tool: **Missile**
  (`class="Missile.Model.ContactSetting"`). Contacts are per neighbourhood and
  per organisation with a `min-rep`/`max-rep` availability window. It refers to
  "procedural text", so MxO generated mission text from a grammar — consistent
  with the `sentence`/`subject`/`directObject` convention everywhere else.
- **`discovery.getGaussianRandomClamped`** — the RNG is not uniform. A server
  must match the distribution, not just the range.
- **Combat findings are in `docs/COMBAT.md`.** Interlock is 1,015 lines over 7
  modules; `ability/` is 3,064 over 24. Combat resolves **winner/loser per
  exchange**, not attacker-rolls-to-hit. HDS's `CombatFormulas.cs` (MIT) was
  reconstructed from `.rdata` *debug strings* — it got the per-tactic
  vulnerability system right, but models the tactic modifier as
  `1.0 + vulnerability` when the real `getTacticalMod(mine, theirs)` is a
  preferred-matchup flat bonus. HDS also lacks exploits, ganging penalties and
  ranged-vs-melee asymmetry.
- Combat tuning constants are **not** in `combat_defines.py` (3 constants);
  they are in `ability/defines.py` (35) and the per-discipline modules.
- **The client ships the server's rule code.** `serverrules.py` is 1,397 lines
  / 82 defs; `clientrules.py` is 552 / 42. **35 functions exist in both** (the
  predict/authorise pair), **47 are server-only** (anything that grants,
  consumes or reveals: `ComputerHack`, `CompileCode`, `DecryptCode`,
  `InvokeVendor`, `HardlineUpload`, `SendMissionText`, `MissionTeam_*`), **7
  client-only** (presentation). `serverrules` has `evalContest(A, aMod, D,
  dMod)` — the contest primitive — and `clientrules` does not.
  Caveat to keep repeating: strong evidence, not proof, that the live server ran
  that exact code, and these are retail 1.0.
- `rshooks.py` (301 lines) is the reward machinery: `RunMissionRewards`,
  `RunReward`, `RunVendorList`, `RunPocketChange`, `RunCollectorPrizeList`,
  `TableImport`, `GoUpALevel`, `VendorBuyback`, `RecycleInfo`.
- **The interaction system is a grammar.** Rule functions take one param named
  `sentence`; ability functions take `subject`, `directObject`,
  `indirectObject`. Actor / verb / objects, evaluated by a rule. This shape
  will drive the server API.
- **RTTI is a dead end on this target** — 7 type descriptors, 1 class. Unlike
  Force Commander, no free class recovery. IDA is the reference instead.
- **idalib works**: run `py-activate-idalib.py` from
  `C:/Program Files/IDA Professional 9.1/idalib/python/` under `py -3.11`, then
  `py -3.11 tools/ida/ida_funcs.py <bin> <out.json>`. `idat -A -S` is NOT
  needed and would be wrong — the toolbox script calls `idapro.open_database`.
  On a 9.5 MB image `auto_wait()` blocks for a long time with no output; that
  is normal, do not assume it has stalled.
- **`launcher.exe` is not the game** — 4 MB of its 4.8 MB is `.rsrc`, only
  574 KB of code.
- **Retail 1.0 ships no `matrix.exe`.** Only `launcher.exe` + `client.dll`.
  Unresolved whether the launcher hosts the DLL or the real host arrives via
  the patcher (`p_dlls/patchctl.dll`, `patchw32.dll`).
- **Discs are InstallShield 9** (`ISc(`, version `0x01009500`), four volumes
  across three CDs, 2,716 files / 124 dirs. `tools/assets/isextract.py` handles
  it unmodified, including volume ordering.
- Two packmaps fail extraction, truncated mid-chunk:
  `packmaps/prefabs_6m.pkb`, `packmaps/worlds_2g.pkb`. Art only, not chased.
- **There is no console version of MxO.** Windows-only, no cancelled port.
  *Enter the Matrix* / *Path of Neo* are Shiny, different engines, unrelated,
  and undecompiled. Closed lead — do not re-investigate.
- **The emulators were built from packet captures** taken while the official
  servers ran (mxoemu ships `Proxy/` and `Sniffer/`). That is why wire formats
  are well known and *rules* — damage, hit/miss, interlock, missions — are not.

## P2/P3 done — the lift is clean
- **IDA 9.1 via idalib finds 56,287 functions.** Median 28 bytes, 50.9% under
  32 — STLport template expansion, not game code. 2,101 FLIRT library, 856
  thunks, 75.2% byte coverage of `.text`.
- **`tools/ida_to_catalog.py` merges `ida_funcs.json` + `ida_xrefs.json`** into
  the toolbox catalog schema: 56,287 functions, 31,550 with call edges, 74,257
  edges. We lift from IDA, not disasm32 — the reference *is* the better catalog,
  so there is nothing to score against it. Everything is emitted as
  `entry_kind: "start"` (IDA has no alias concept; claiming otherwise would make
  a future score meaningless).
- **`run_lift.py --all`: 56,287 lifted, 0 errors, 4,099,690 lines, 113 files,
  43 s.** Largest clean lift in the collection. "0 errors" means the lifter did
  not fault; it does **not** mean the C is correct and nothing has been run.
- **No `true_extent()` branch walk needed.** IDA gives real extents, so
  `f['end']` is used directly — unlike Force Commander, where disasm32's
  reachability bound decoded a megabyte as one body. Residual risk: IDA's
  `end_ea` ends the *main chunk*, so tail-chunked functions are under-read
  (fails loudly as a missing tail, rather than silently swallowing a neighbour).
- **Never hardcode the export RVAs.** The first `run_lift.py` did and got
  `SetMasterDatabase` wrong (`0x1F1BF0` for `0x1F1DB0`). `ROOT_EXPORTS` is names
  only; addresses come from the PE export directory at runtime. Note
  `analyze_pe` yields `ExportEntry` **objects**, not dicts.
- **IDA is NOT ground truth here — it misses ~20,000 functions.** This was
  nearly a silent defect. `disasm32.py` (40 min, 2.4 GB, but it finished) finds
  **78,105** functions and **95.9%** byte coverage against IDA's 56,287 and
  75.2%. Scored with IDA as reference it looks terrible — 63.7% precision,
  20,241 "invented". It is IDA that is wrong:
  - Where both agree a function exists, **they agree on its end 99.39% of the
    time** (46,332/46,616 exact). One tool is seeing code the other never
    looked at, not disagreeing about the same code.
  - disasm32's log: **~19,000 functions reachable only via data pointers** —
    i.e. virtual methods nothing calls directly. IDA's auto-analysis never
    promotes those. Same vtable-only pattern as Rise of Legends.
  - The extras fill **60% of the 1.9 MB IDA leaves unattributed**.
- **Always use `ida_to_catalog.py --augment analysis/functions.json`.** Without
  it the lift silently stubs ~20,000 real functions. Merged catalog: **76,528
  functions, 85,297 edges** (56,287 IDA + 20,241 gap fill, 853,418 bytes).
  Merged lift: **76,528 functions, 154 files, 0 errors, 49 s, 4,854,290 lines.**
- **Do not sum disasm32's function sizes to measure coverage.** Its `end` is a
  reachability bound, so ranges overlap and run into neighbours; the naive sum
  gave 6.28 MB against a 1.9 MB gap (330%, impossible). Merge to a union and
  intersect with the gap.
- Correct export addresses: `RunClientDLL` 0x1120, `TermClientDLL` 0x1140,
  `ErrorClientDLL` 0x1170, `InitClientDLL` 0x1270, `SetMasterDatabase` 0x1F1DB0,
  `DllMain` entry RVA 0x5C3736. Image base `0x10000000`.

## P3b done — it compiles
- **`cl /c /MP` over every generated file: EXITCODE=0, 156 objects, 197 MB**,
  MSVC 2022 x86 via `vcvars32.bat` from the **BuildTools** install
  (`C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/...`).
  MinGW is also present at `/c/msys64/mingw64` but is 64-bit; use MSVC x86.
- `run_lift.py` emits `recomp_funcs.h` + `recomp_dispatch.c`. It originally
  collected the entry list and threw it away, which left indirect calls nothing
  to resolve through.
- **`gen_imports.py` sorts 479 imports three ways**: stdcall 226 (derived argc
  — never typed by hand; a wrong count shifts the *caller's* frame and dies
  somewhere unrelated), cdecl 237 (pop 0; 181 MSVCR71 + 56 pythonMXO), **data
  16**.
- **Data imports are the trap.** `pythonMXO` exports 693 from `.text` and 115
  from `.data` — `PyType_Type`, `PyString_Type`, `PyExc_*`, `_Py_NoneStruct`,
  `_Py_TrueStruct`, `_Py_ZeroStruct`. The client *dereferences* these. A
  function shim there fails neither at build nor at the call site. They get
  storage + their own table (`mxo_import_data`). MSVCR71 adds `_iob`,
  `__pioinfo`, `_adjust_fdiv`.
- **Classify exports by section characteristics (`0x20|0x20000000`), never by
  name.** `binkw32.dll` has ~30 custom-named *executable* sections (`BINK`,
  `BINK16`, `BINK4444`, `BINK32R`, …) at `0x60000020`. A `.text`-only test
  mislabels 8 of its exports as data.
- **Export names are not C identifiers.** Both kinds appear here: C++ mangling
  (`?terminate@@YAXXZ`) and stdcall decoration (`_BinkOpenDirectSound@4`).
  `c_ident()` sanitises with collision handling; the real name stays in the
  `IMPORT_STUB` string.
- Generated/vendored and git-ignored: `src/runtime/imports_gen.c`,
  `src/runtime/recomp_types.h`, `src/recomp/gen/`, `build/`.

## P4 — it runs; current state and blocker
- **Build+run**: `cl /c /MP` all of `src/recomp/gen/*.c` + `src/runtime/*.c`,
  then `link /SUBSYSTEM:CONSOLE /LARGEADDRESSAWARE:NO build/obj/*.obj
  kernel32.lib user32.lib` → `build/mxo_host.exe` (121 MB). Run
  `build/mxo_host.exe --init` (also `--run`, `--term`, `--va 0x...`).
- **Three things the host must do, each of which failed separately first:**
  1. Map `client.dll` at `0x10000000` (`recomp_load_image`).
  2. **Build a simulated TIB and set `g_fs_base`.** `InitClientDLL`'s first
     call (`sub_10322960`) opens with MSVC's SEH prologue and reads `fs:[0]`.
     With `g_fs_base` = 0 that faults immediately. TIB at `0x0E800000`:
     `[0]=0xFFFFFFFF` (SEH chain end), `[4]`=stack top, `[8]`=stack limit,
     `[0x18]`=self, `[0x24]`=tid.
     **Do not grep the lifted code for `g_fs_base`** — the lifter emits the
     `FS_BASE` macro, and grepping the wrong token briefly ruled this out.
  3. **Self-patch the IAT**: write each slot's own VA into itself, so
     `call [slot]` becomes `ICALL(slot_va)` for `recomp_lookup_import`. 479
     slots; data slots instead get the address of their storage.
- `recomp_lookup` must be a **binary search** — 76,528 entries, and this binary
  makes a great many indirect calls.
- **Real shims unblock startup**: `malloc`/`free`/`calloc`/`realloc`/`memmove`,
  `_aligned_malloc`/`_aligned_free` (never free an aligned block with `free()`),
  and the five interlocked functions. Took the run from 9 → **264 indirect
  calls**. Shim args: `ARG(n) = MEM32(g_esp + 4 + 4n)`; `STDRET(0)` cdecl,
  `STDRET(argc)` stdcall.
- **`memset`/`memcpy`/`memcmp`/`strlen`/operator `new`/`delete` are NOT
  imported** — MSVC 7.1 emits them as intrinsics, so they are already inside
  the lifted code. `gen_imports.py` now hard-fails on a `HAND_WRITTEN` name that
  is not imported as code, because the stub silently stays otherwise.
- **Current blocker:** dies in a `__thiscall` ctor zeroing a ~0x24-byte object
  through a garbage `this`, reached from `eh vector constructor iterator`
  (`??_L@YGXPAXIHP6EX0@Z1@Z` at `0x105C3C98`), which *also* makes an ICALL to
  `0x105C3CE2` — an address inside itself. Two symptoms, one cause: the iterator
  is getting bad arguments, i.e. a stack desync upstream (the signature of a
  wrong `STDRET`). **Next:** a `RECOMP_TRACE` build for a real backtrace, and
  harvest unresolved VAs back into the catalog as seeds.
- "It runs" = recompiled code executes into the client's own startup. It does
  **not** render, reach `RunClientDLL`, or open a window.

### P4 iteration log — 264 → 750 indirect calls
- **Seeds fixed the first blocker.** A direct `call <imm>` to an address not in
  the catalog becomes `RECOMP_ICALL(const) /* not lifted */`; at runtime it
  resolves to nothing, the frame is popped, 0 is returned, and execution
  continues with garbage — surfacing as a crash elsewhere. `run_lift.py` now
  scrapes its own output for that marker into `analysis/missing_targets.json`
  and `--seeds` injects them. Converges in two passes. Found 2 real
  (`0x105C3C80`, `0x105C3CE2` — MSVC EH funclets inside
  `??_L@YGXPAXIHP6EX0@Z1@Z`); `0x716FC971` is out-of-image and correctly dropped.
- **Shim rounds**, each moving the blocker: path/identity
  (`GetModuleFileNameA`, `GetCurrentDirectoryA`, `GetEnvironmentVariableA`,
  `GetLastError`, `strncpy`, `fopen`, …) → 593; crash-reporter module walk
  (`CreateToolhelp32Snapshot`, `Module32First/Next`, `CloseHandle`, `Sym*`) →
  706; `_msize` + `SymLoadModule64` → 750.
  **Forward toolhelp to the real API** — 32-bit host, 1:1 mapping, so the
  client's `MODULEENTRY32` pointer is directly usable. A stubbed snapshot
  returns 0 and the client calls `Module32First` on it *without checking*.
- **Guard the allocator shims.** The first traced build died with
  `STATUS_HEAP_CORRUPTION (0xC0000374)` in ntdll, not an AV. `free`, `realloc`,
  `_aligned_free`, `_msize` now refuse pointers they did not hand out (leak
  instead — a leak is visible and harmless over seconds, a bad free is neither).
  `_msize` reading a foreign heap header was the likely culprit. Corruption
  gone, zero bad frees.
- **`build/mxo_trace.exe`** = `-DRECOMP_TRACE` + `recomp_trace.c`, giving a
  40-deep entry-ring backtrace. **The simulated stack makes the host's own call
  stack useless**, so this ring is the only backtrace available. Full traced
  rebuild ≈ 6 min.
- **Current blocker**: `sub_102C2210` is an inlined `strlen` (46 bytes) faulting
  on a garbage pointer. Chain: `0x101F48A0` → `0x10001BC0` → `0x100017A0` →
  `0x10001BC0` → `0x100017A0` → `0x102C2210`. `0x10001BC0` (112 B) and
  `0x100017A0` (232 B) alternate through the whole trace — STLport string
  helpers. `0x101F48A0` is near `SetMasterDatabase` (`0x101F1DB0`), so the path
  is database/resource setup.
- Batch-file trap: `%ERRORLEVEL%` after a pipe reports the *last* command in the
  pipe (e.g. `findstr`), not `cl`. A reported `COMPILE=1` there was spurious.

### STARTUP WORKS — `InitClientDLL` completes
- **`_initterm` was the whole blocker, and it must re-enter lifted code.** It
  walks a table of function pointers calling each — that is how C++ static
  constructors run. Stubbed, it does nothing, so every global a constructor
  would have built stays zero and the symptom appears arbitrarily far away (a
  NULL string four calls into `InitClientDLL`). Implemented in `shims_impl.c`
  via `recomp_lookup` + `PUSH32(RECOMP_RETADDR)` + call.
  **Result: 12,189 initialisers run.**
- **`DllMain` must run before any export** (`--no-dllmain` to skip). Calling
  `InitClientDLL` alone skips the entire CRT startup path. `DllMain` =
  stdcall(hinst, DLL_PROCESS_ATTACH=1, NULL).
- **`--boot`** = `DllMain` → `InitClientDLL` → `RunClientDLL`. Init returns
  `eax=0`, which is **LT_OK in LithTech, i.e. success**, not failure.
- **64,377 indirect calls, exit 0.** `RunClientDLL` then faults 1 call in at
  `0x103065E0` (null field inside a live object) — it wants a window/renderer.
  Remaining stubs are exactly that surface: `CreateEventA`, `GetStockObject`,
  `GetSystemMetrics`, `LoadLibraryA`, `QueryPerformanceCounter`,
  `InitializeCriticalSection`/`Enter`/`Leave`, `TlsAlloc`, `GetFileType`,
  `SetUnhandledExceptionFilter`, `GetSystemTimeAsFileTime`, `_snprintf`.
- **121 CRT bodies in `shims_crt.c`.** Doubles return in **ST(0)**, not EAX —
  the lifter models x87 flat and only emits `_st[0]`, so `g_st[0] = result`.
  The `_CIxxx` forms take their argument in ST(0) with nothing on the stack.
  `_ftol` returns EAX:EDX.
- **A stub is a lie with a plausible return value.** Zero is a valid-looking
  answer for most of the CRT. Every failure this phase was silent and faulted
  far from its cause.
- Deliberately still stubs (need more than a forward): the `printf` family
  (varargs off a simulated stack), `qsort` (callback into lifted code),
  `setjmp`/`longjmp`, and the C++ EH machinery.
- Host dumps simulated registers + faulting address on exception. `eax=0` /
  "reading 0x00000000" is what turns "crash in a strlen" into "who produced
  the NULL".
### P5 done — Win32 layer + a real window
- **`client.dll` never creates a window.** No `CreateWindowEx`, `RegisterClass`,
  `GetMessage` or `DefWindowProc` in the import table — only window
  *manipulation*. The LithTech **engine host** owns the window and the message
  pump. `host.c` creates a 1024×768 window, publishes it via `host_hwnd()`, and
  pumps the queue around each entry point.
- **~215 of 479 imports now have real bodies** (`shims_win32.c` adds 71).
- **The printf family cannot be forwarded** — varargs off a *simulated* stack,
  so the host's `va_list` cannot be pointed at them. `fmt_from_stack()` walks
  the format string, pulls each arg from `g_esp`, and hands one directive at a
  time to the host `snprintf` (keeps flags/width/precision, avoids
  reimplementing float formatting). Doubles read 2 slots, everything else 1.
- **NEVER forward `LoadLibraryA` or `GetModuleHandleA` for foreign modules.**
  A real `HMODULE` lets the client walk that module's real PE export table and
  call a real **host** address — arriving as `ICALL(0x7610EC20)`, unresolvable
  (the dispatch table knows only lifted VAs and IAT slots), and a host function
  reached that way expects args on the *host* stack anyway. Both decline and log
  now. `GetModuleHandleA(NULL)` must return the **guest image base**
  (`0x10000000`). The client asks for `unicows.dll` (MS Layer for Unicode) ×6
  and `kernel32.dll` ×1.
- `MessageBoxA` routes to the console — a modal box blocks an automated run.
- `SetUnhandledExceptionFilter` returns 0 (declined) so faults surface through
  the host's VEH with the lifted VA.
- `timeGetTime` needs `#include <mmsystem.h>` + `winmm.lib`, or it links as
  cdecl `_timeGetTime` instead of the stdcall-decorated symbol.

### P6 — the next wall, and it is architectural
`RunClientDLL` faults along `0x10001120 → 0x10006640 → 0x10306720 →
0x103065E0`, dereferencing a **null field inside a live `.data` object**. Not a
bug: the client is reaching for **LithTech engine interfaces** that only the
engine host supplies, and nothing supplies them yet. `SetMasterDatabase` and the
exported `ILTCool` class are the visible edge of that surface.

- **Heredoc trap that bit repeatedly:** writing C string escapes through
  `python - <<'PY'` eats a backslash level, turning `\n` into a real newline and
  producing `error C2001: newline in constant`. Use `chr(92) + 'n'`, or patch
  with `sed` on a backslash-free pattern.

## Working rules
- Score function recovery (`disasm/score_recovery.py`) against an IDA export
  **before** lifting anything. On 7.64 MB of `.text` this is not optional.
  Split false positives into *split* vs *invented*; they need opposite fixes.
- Facts go in `analysis/` as JSON, prose goes in `docs/`, status goes in the
  README. One home each.
- When a community claim is repeated but unverified, label it unverified and
  write down how to check it. The `MXO_EMULATOR_MASTER_REFERENCE.md` quote about
  combat formulas living in the client is the live example — its source repo
  404s, and the claim is checkable by lifting.

## Open questions
1. Target retail 1.0 (these discs) or the final live client the emulators use?
   The protocol moved over four years of patches. Choose deliberately.
2. Where is the game's host executable?
3. How much of the game is Python?
4. Are the `Cool*` / `Xprt` DLLs a second protocol (chat/IM/presence) that a
   server must also speak? `libltcool.dll` ties them to LithTech.
5. Do combat formulas actually live in the client? Checkable. Unchecked.
