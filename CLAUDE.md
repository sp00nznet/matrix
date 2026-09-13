# The Matrix Online Static Recompilation — Project Memory

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
