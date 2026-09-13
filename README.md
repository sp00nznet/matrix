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

## Project status: **P1 started, and it opened with a bang.**

The game's combat, ability, interlock and mission logic is **not compiled into
`client.dll` at all.** It ships as Python 2.3 bytecode, in an archive format
that took about forty lines to unpack. See
[The rules are in the box](#the-rules-are-in-the-box).

| | |
|---|---:|
| target | `client.dll` |
| `.text` | **7,642,490 bytes** |
| imports | 479 across 19 DLLs |
| protection | **none** |
| game Python modules recovered | **57** |
| functions recovered | — |
| lift errors | — |

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
`interlock/combat_resolution.pyc`, and it decompiles.

`serverrules.pyc` is 53,735 bytes — the largest game module after the effects
table — and sits beside `clientrules.pyc`, `crafting_server.pyc` and
`rshooks.pyc`. A retail client that ships something named *serverrules* is a
strong lead, not yet a conclusion; what those modules actually contain is the
next thing to establish and it has not been established yet.

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

### Track A — recompile the client

| Phase | What | State |
|---|---|---|
| **P0** | Unpack discs, identify binaries, rule out DRM | ✅ done |
| **P1a** | Python surface: unpack the `.ltp` archives | ✅ **57 game modules** |
| **P1b** | Decompile those modules; recover the engine API they call | next |
| **P1c** | Recon the rest: string map, RTTI, the `Cool*` stack | |
| **P2** | Function recovery over 7.64 MB of `.text`, scored before lifting | |
| **P3** | Lift to C; import bridges for 479 imports across 19 DLLs | |
| **P4** | Bring-up: window, loop, renderer | |

P1b is the pivot. The Python names the engine functions it calls; that list is
the priority order for P2 and P3, and it is a far better one than "start at the
entry point and work outwards" on a 7.6 MB binary.

**P2 gets scored before anything is lifted.** `disasm/score_recovery.py`
against an IDA export, splitting false positives into *split* and *invented* —
they need opposite fixes, and finding the gaps at runtime 30,000 calls deep is
how this goes wrong. On a target this size that discipline is not optional.

### Track B — the server

| Phase | What | State |
|---|---|---|
| **S0** | Survey prior art, settle the licence position | ✅ done — `docs/SERVERS.md` |
| **S1** | Read the protocol out of the lifted client rather than off the wire | |
| **S2** | Auth + character select — enough to reach the world | |
| **S3** | World: districts, statics, movement, doors, hardlines | |
| **S4** | Combat, from the client's own formulas | |
| **S5** | **Missions.** The part nobody has finished. | |

Track B leans on Track A from S1 onward. S2 and S3 could be started from the
MIT prior art alone; S4 and S5 are precisely the parts that could not be built
from packet captures, and they are the reason to do this at all.

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
