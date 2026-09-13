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
- **The client embeds Python 2.x.** `client.dll` imports the Python C API from
  `pythonMXO.dll` — `Py_Initialize`, `Py_InitModule4`, `PyRun_String`,
  `PyObject_CallObject`, `PyUnicodeUCS2_*`. Ships `resource/Python/Monolith.ltp`
  and `standard.ltp`, `resource/Lua/client.lua`. **Script is not compiled code:
  every line of it is a line nobody has to lift.** Highest-value lead.
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
