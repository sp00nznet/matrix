# P1 reconnaissance: how The Matrix Online client is put together

This is the primary P1 document. It describes the client's architecture as
recovered from the retail 1.0 discs, and it is the reference the lift (P2/P3)
and the server (S1+) both work from.

`docs/COMBAT.md` covers combat and the client/server rule split in detail and is
not repeated here. `docs/SERVERS.md` covers the prior-art emulators.

Everything below is interface — names, arities, counts, addresses. The game's
own code and text stay in `work/`, which is git-ignored.

---

## The shape of the thing

```
launcher.exe ──loads──> client.dll ──hosts──> pythonMXO.dll (Python 2.3)
   4.8 MB              9.5 MB                  1.1 MB
   574 KB code         7.64 MB .text           713 KB .text
   4 MB .rsrc          10 exports              CPython 2.3

                       resource/Python/Monolith.ltp   57 game modules
                       resource/Python/standard.ltp  325 stdlib modules
                       resource/string_tables/Strings.ltp  22 string modules
                       resource/Lua/client.lua
```

This is a **LithTech** game. `client.dll` exports the LithTech client-DLL
contract and nothing else of consequence:

| Export | RVA | Role |
|---|---|---|
| `RunClientDLL` | `0x1120` | per-frame entry |
| `TermClientDLL` | `0x1140` | shutdown |
| `ErrorClientDLL` | `0x1170` | error path |
| `InitClientDLL` | `0x1270` | startup |
| `SetMasterDatabase` | `0x1F1DB0` | hands the client its object database |
| `ILTCool` ×5 | — | ctor, copy ctor, assignment, dtor, vtable |

Four functions inside `0x150` bytes of each other are the entire startup
surface. That is the bring-up target for P4.

`ILTCool` being an *export* settles what the eleven `Cool*.dll` files are:
`libltcool.dll`, `CoolBos`, `CoolBucky`, `CoolHttp`, `CoolIcq`, `CoolPeer`,
`CoolSec`, `CoolSecNss`, `CoolSocket`, `CoolSos`, `CoolTih`, `CoolWrapper`,
plus `Xpcs`/`Xprt`/`Xptl`. They are a LithTech engine interface, not a
third-party library bolted on. `CoolIcq` and `CoolPeer` still suggest
IM/presence, so the working assumption remains that a server may have to speak
more than the world protocol. Unconfirmed.

---

## The client is a Python application wearing a C++ engine

This is the single most important structural fact about the target.

`client.dll` imports the Python 2.3 C API from `pythonMXO.dll` — `Py_Initialize`,
`Py_InitModule4`, `PyRun_String`, `PyObject_CallObject`, `PyImport_ImportModule`,
`PyUnicodeUCS2_*`. The gameplay rules are not compiled into the 7.64 MB of
`.text`. They are bytecode in `resource/Python/`.

`tools/ltp_extract.py` reads the `.ltp` container; `tools/decompile_ltp.py`
runs uncompyle6 over the result. 56 of 57 game modules decompile cleanly to
**10,026 lines**. (`ltfxmap` is a quarter-megabyte effect table and gets its
own budget.)

### The 57 game modules

| Package | Modules | Contents |
|---|---:|---|
| root | 23 | `combat`, `combatvalidate`, `combat_defines`, `serverrules`, `clientrules`, `rshooks`, `clhooks`, `experience`, `abilities`, `augments`, `recipes`, `crafting_server`, `missionvalidate`, `generic_free_attacks`, `tempmods`, `emotes`, `trgs`, `obj`, `NPCTypes`, `ltfxmap`, `errtofile`, `followvalidate` |
| `ability/` | 24 | `defines`, `effects`, `general`, `utility`; `hacker/` (virii, virii_defines, health, crafting, simulacra, misc), `soldier/` (aikido, karate, pistol, rifle, smg, misc), `spy/` (sneaks, misc), `npc/specialmoves` |
| `interlock/` | 7 | `combat_resolution`, `combat_modifiers`, `combat_abil_level`, `combat_innerstrength`, `combat_free_attacks`, `combat_utility` |
| `Missions/` | 4 | `Mission` base class plus two shipped missions |

Plus 22 in `Strings.ltp`: `stringtable_client` (2.2 MB — every displayed
string), `stringtable_ccr`, `st_neighborhoodnames`, `st_hardlinenames`,
`st_floornames`, `st_elevatornames`, `st_aim`, `st_poi_*`, `st_cin_*`.

---

## The engine API: 70 subsystems

Every name the scripts call but never import or define is an object the C++
injects into the interpreter. `tools/engine_api.py` recovers them into
`analysis/engine_api.json`. **This is the lift-priority list for P2/P3** —
these are the C++ subsystems that gameplay actually depends on, which is a far
better starting point than the entry point of a 7.64 MB binary.

Ranked by how many modules use each (breadth, not raw call count — see the
caveat below):

| Subsystem | modules | methods | What it is |
|---|---:|---:|---|
| `discovery` | 25 | 21 | engine services: printing, effects, object/type lookup, RNG, messaging |
| `AbilityInv` | 18 | 41 | ability application: damage, health, inner strength, temp mods |
| `constants` | 17 | 12 | shared enums |
| `CharMvt` | 13 | 21 | character movement |
| `Interlock` | 12 | 28 | the combat minigame |
| `consolevar` | 11 | 24 | console variables |
| `combat` | 7 | 34 | combat state |
| `Inventory` | 6 | 10 | |
| `AI` | 6 | 9 | |
| `PlayerCharacter` | 5 | 8 | |
| `abilitylib` | 4 | 7 | |
| `physics` | 4 | 3 | |
| `combat_prone` | 3 | 11 | |
| `Build`, `Chat`, `Organization`, `rewardselection`, `StateMachine`, `MissionTeam`, `Construct`, `DamageBox`, `Components`, `SimulacraSlave`, `Container` | 1–3 | | |

**Rank by module breadth, never by raw call count.** `subject`,
`directObject` and `indirectObject` top the call counts and are not subsystems
at all — they are the parameter names of ability functions. `tools/engine_api.py`
flags any name that is bound as a local or parameter elsewhere.

### Representative method sets

`Interlock` (28) — `attack`, `specialAttack`, `physicalDamageFromAbility`,
`specialityDamageFromAbility`, `isInCombat`, `isCombatTarget`, `giveHealth`,
`cantDie`, `createEvadeShield`/`hasEvadeShield`/`destroyEvadeShield`,
`randomizeQueue`, `setDefenseModeToPassive`/`setDefenseModeToAutoDefend`,
`disableMultiOnOnePenalty`, `disableFiringIntoMeleePenalty`,
`disableFiringAtMeleeRangePenalty`, and a family of
`setAllowedTo*` gates (change tactical setting, change targets, use weapons,
use items, withdraw, free attack, force combat, perform special moves).

`AbilityInv` (41) — `dealAbilityDamage`, `applyAbilityToArea`, `takeInnerStrength`,
`Health`/`MaxHealth`/`recalculateMaxHealth`, `InnerStrengthMax`/`InnerStrengthAvailable`,
`addTempMod`/`removeTempMod`/`hasTempMod`/`getTempMods`/`addTempModProcs`/
`addTempModToAllActive`, `addStateMod`, `createSpecificMod`, `addRecastTimer`,
`reapplyAbility`/`deactivateAbility`/`turnOffAbility`, `createSimulacrum`,
`createStaticObject`, `getSignalBoostBonus`, `sendChat`.

`discovery` (21) — `getGameObjectPropValue`, `getGameObjectType`,
`getGameObjectName`, `getTypeInfo`, `getAllObjects`, `abilityIDToGameObjectID`,
`getGaussianRandomClamped`, `playEffect`, `addHardline`, `addAccessNode`,
`sendMissionResult`, `sendModalMessage`, `clientSystemMessage`,
`abilityMessageToCaster`/`ToTarget`/`ToAll`, and the print family.

Note `getGaussianRandomClamped`: the RNG is not uniform. Any server
reimplementing these rules has to match the distribution, not just the range.

---

## The mission system

This is the part no emulator has finished, and the contract is small and fully
recovered.

### Missions are event-driven classes

`Missions/Mission.py` is a base class with **twelve callbacks**. A mission
subclasses it and overrides what it needs:

```
OnStartOfMission(teamids)
OnStartOfStage(phasenumber, stagenumber, teamids)
OnGoalComplete(phasenumber, goalnumber, teamids)
OnPhaseComplete(phasenumber, teamids)
OnMissionEvent(phasenumber, eventid, teamids)
OnObjectTargeted(phasenumber, teamleader, itemname)
OnPlayerEnterZone(phasenumber, teamleader, zoneid)
OnModalDialogueAccepted(phasenumber, teamleader, messageid, leftchosen)
OnToggledViewActivated(phasenumber, teamleader, viewid)
OnInventoryItemsSwapped(phasenumber, sender, destSlot, destID, destData,
                        srcSlot, srcID, srcData)
OnShortcutAdded(phasenumber, sender, shortcutType, classData, instanceData, invSlot)
OnMovementUpdated(phasenumber, sender, posX, posY, posZ, angleFacing)
```

The structure is **mission → phase → stage → goal**, and it is team-based
throughout: every callback carries `teamids` or `teamleader`.

Two complete missions ship in the retail client — 465 and 871 lines — which
means we have worked examples of the idiom, not just the interface.

### The mission verb set

The engine injects a `mission` object with **21 methods**. These are the
scripting verbs a quest can use, and implementing them is most of implementing
quests:

| | |
|---|---|
| flow | `activateStage`, `completeGoal`, `isGoalComplete`, `sendMissionEvent`, `scheduleEvent` |
| narrative | `playNarration`, `broadcastMessage`, `sendModalMessage`, `toggleView` |
| world | `sealDoor`, `unlockDoor`, `getDoorZoneIDs`, `teleportToMissionElement`, `setExclusiveWaypoint`, `playLocalFX` |
| NPCs | `killNPC`, `respawnNPC`, `unkillNPC` |
| player | `grantExperience`, `addTempAbility`, `bootPlayer` |

### Mission identity and teams

`missionvalidate.py` handles mission keys: `MissionKeyBase`, `MissionKeyPart`,
`ValidMissionKeyMatch`, `ValidMissionKeyBaseMatch`, `IsMissionTeamMember`,
`IsPvPOpponent`, `IsNonReservedMatchingKey`. The `MissionTeam` engine object
exposes `MissionKey` and `MissionTypeFlags`.

So a mission key is structured (base + part), and the same key comparison
decides both "are we on the same mission team" and "are we PvP opponents".

### Rewards

`rshooks.py` (301 lines) plus the `rewardselection` engine object
(`DetermineReward`, `DetermineEncodedReward`, `DeterminePopulateReward`,
`Decode_Reward`, `Apply_Rewards`, `Get_Item`, `Lastroll`). Rewards are
*encoded* — `RunMissionRewards` takes an `encodedReward` — and resolved
against loot tables loaded by `TableImport`/`ParseTable`/`ParseItem`.

### Mission contacts

`resource/missions/definitions/sponsors.xml` is plain XML and carries its own
documentation comment. It defines mission contacts per neighbourhood and per
organisation, with fields for contact id, organisation id, display name,
string-table id, procedural-text abbreviation, face resource id, and — for
organisation contacts — a `min-rep`/`max-rep` window that gates availability
and retires the contact once the player outgrows it.

Its root element is `class="Missile.Model.ContactSetting"`, which names the
internal authoring tool: **Missile**. The comment also refers to
`stringtable_client.sta`, tying contact names to the string table recovered
from `Strings.ltp`.

The phrase "procedural text" matters — MxO generated mission text procedurally
from a grammar, which is consistent with the `sentence` / `subject` /
`directObject` / `indirectObject` convention running through the rule and
ability APIs.

---

## What is still open

1. **Which client version to target.** These are the February 2005 retail
   discs; the emulators target the final live client, four years of patches
   later. Structure should hold, tuning will have moved.
2. **The `Cool*` / `Xprt` stack** — is it a second protocol a server must
   speak? `ILTCool` is an engine interface; what rides on it is unconfirmed.
3. **`ltfxmap`** — deferred, not decompiled. 253 KB of effect table.
4. **`SetMasterDatabase`** — what the object database is and where it comes
   from. `resource/GameObjects/Cookbook.cbb` and `gocategories.gcb` are
   probably related and are not yet decoded.
5. **The procedural mission generator.** `sponsors.xml` implies one. Whether
   the generator lives in the client, in `Missile`, or only on the server is
   unknown, and it decides how much of "the missions" can be recovered at all
   versus regenerated.

---

## The engine interfaces: what P6 actually is (28 of them)

`client.dll` does not create its engine interfaces. It declares **holders** for
them — a small object with a vtable at `+0` and an interface pointer at `+4` —
and waits for the engine to fill each one in. The interface pointer stays NULL
until something installs it.

That is the whole of the P6 wall. `RunClientDLL` faults at `0x103065E0` reaching
a holder whose `+4` is still the zero its constructor left, and calling through
it. Confirmed at runtime: the holder at `0x10857788` has its vtable set
(`0x10780D38`) and `+4 = 00000000`, unchanged across `DllMain` and
`InitClientDLL`.

Each holder is registered by a static initialiser of the form
`register(holder, "Name", flags)`, so the set is recoverable statically —
[`tools/find_interfaces.py`](../tools/find_interfaces.py) finds the calls, walks
back for the pushed operands, and reads the name out of the image.
**28 holders**, in `analysis/engine_interfaces.json`:

| Group | Interfaces |
|---|---|
| **Networking** | `INetMgr.Default`, `INetShell.Default`, `ILTMultiProtocolMgr.Default`, `ILTDistrObjExecutive.Default`, `CLTRemoteCommCtxMgr.Default`, `ILTCommHandleMgr.Default`, `ILTReactorHandleMgr.Default` |
| **Login** | `ILTLoginMediator.Default` |
| **Game objects** | `ILTGOGameObjMgr.Default`, `ILTGOComponentFactory.Default`, `ILTObjectMgr.Default`, `ILTClassMgr.Default`, `ILTRsetManager.Default` |
| **World** | `ILTWorldRep_Metro.Default` / `.Internal` |
| **Render** | `ILTRenderDevice.Default`, `ILTRenderStyles.Default`, `ILTTexture.Default`, `ILTDrawPrim.Default` / `.Internal`, `ILTFontManager.Default`, `ILTWidgetManager.Default` |
| **Input** | `ILTInputMgr.Default`, `LTWinInputMgr.Default` |
| **Misc** | `ILTEventScheduler.Default`, `ILTPredictViewMgr.Default`, `ILTDiagnosticMgr.DObSys`, `ICompress.Default` |

Three things fall out of this list immediately.

**It confirms the convergence thesis, by name.** Seven of the 28 are networking,
and `ILTDistrObjExecutive` — a *distributed object executive* — is the
client-side half of exactly the object replication a server performs. The
server-facing surface is not something to infer; it is enumerated.

**It explains why the socket layer never fired.** `WSAStartup` is not called
during `InitClientDLL` because `INetMgr.Default` was never installed. The
network is behind this wall, not beside it.

**`ILTLoginMediator.Default` sharpens the auth question.** The client does have
a login interface, so login is not purely the launcher's business — though with
no TCP anywhere in `client.dll`, what that mediator speaks is still open.

`ILTWorldRep_Metro` is worth noting for its name: Mega City is the "Metro" world
representation, which is likely also the key to the `.pkb` world packs.

### What P6 now is, concretely

Not "reverse engineer the engine". It is: for each of 28 named holders, supply
an object whose vtable has the methods the client calls, and install the pointer
at `+4`. The client tells us which methods it needs by calling them — each one
that is missing is a reported unresolved address rather than a guess, exactly
like the import worklist.

Order is set by what `RunClientDLL` reaches first, and that is `INetMgr`.
