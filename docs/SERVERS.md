# Prior art: who has tried to run a Matrix Online server

The Matrix Online shut down on 2009-07-31. Since then two server emulators have
gone the distance, plus a scattering of smaller efforts. This document is the
survey: who did what, what they actually achieved, what licence it carries, and
what each one still guesses at. The last question is the one that matters to us,
because it is the gap a recompiled client closes.

**Status of this document:** first pass, written from public repository metadata
and project documentation. Claims sourced to a project's own README are labelled
as such. Nothing here has yet been confirmed against the client binary; that is
what P1 is for, and the confirmations get recorded in this file as they land.

---

## The two that matter

| | **mxoemu** ("Reality") | **mxo-hd** (Hardline Dreams) |
|---|---|---|
| Author | rajkosto | Neo / Morpheus (hdneo) |
| Language | C / C++ | C# (.NET Core 3.1+) |
| Licence | **AGPL-3.0** | **MIT** |
| Repo | `rajkosto/mxoemu` | `hdneo/mxo-hd` |
| Last push | 2010-10-10 | 2026-04-02 |
| Stars | 62 | 52 |
| Public source | Partial — the GitHub tree is old | Yes, current |
| Live service | Yes, `mxoemu.info`, still updated | Yes, community servers |

These are not rivals so much as two halves of the same effort. Hardline Dreams'
own account is that it builds on rajkosto's research — the encryption work and
the GoProps object system — and rewrites the server in C# for easier
development.

### mxoemu / Reality (AGPL-3.0)

Repository components:

```
Reality/        the game server proper
Proxy/          sits between a real client and a real server
Sniffer/        packet capture (bundles WinPcap's WpdPack)
mxohax/         client-side utilities
Dependencies/   CryptoPP, the Sockets library (GPL)
```

The `Proxy` and `Sniffer` directories say something important about method:
this protocol was recovered by **watching live traffic while the official
servers were still up**. That is a hard deadline nobody gets a second run at,
and it explains the shape of what is known versus unknown — anything a player
did between 2005 and 2009 in front of a capture is known; anything else is not.

`Reality/Binaries/` ships `privkey.dat`, `pubkey.dat`, `signPriv.dat` and
`signPub.dat`, so the auth handshake is public-key signed and the emulator
substitutes its own keypair. `Reality/SQL/reality.sql` is the schema.

**The licence trap.** GitHub's repository sidebar reports no licence for
mxoemu, because the licence file is `Reality/Source/LICENCE.TXT` rather than a
root `LICENSE`. It is the GNU Affero General Public License v3. Anyone who
skimmed the sidebar and concluded "unlicensed, therefore all rights reserved"
or "no licence, therefore free" got it wrong in both directions. AGPL-3.0 is
specifically designed for this case: run a modified AGPL server as a network
service and you owe source to everyone who connects to it.

For this project that means: **read it, never copy it.** See `LICENSE`.

### mxo-hd / Hardline Dreams (MIT)

```
hds/            C# server
  auth/         AuthServer, AuthClientSession, CharacterPack, WorldList
  ...
SQL/reality_hd.sql
data/           the static game data, as CSV and XML
docs/SETUP.MD
```

Per its README, implemented to some degree: authentication and character
select, three world districts with static object spawning, doors, hardline
teleport, signposts, friends lists, crews and factions, reputation across the
three organisations, mob spawning with partial combat, NPC vendors, ability
loadouts, inventory and equipping, player-to-player visibility and chat. The
mission system is described as early-stage. Hyperspace jumping is described by
its own authors as rough.

`data/` is the interesting part and is MIT: `gameobjects.csv`, `mob.csv` and its
several parsed variants, `abilityIDs.csv`, `mxoClothing.csv`, `newrsiIDs.csv`,
`vendor_items.csv`, `staticObjects_*.csv` for the districts, `animations.txt`,
`fxlisthex.txt` / `fxlistreal.txt`, `NPC_COLLECTION.xml`, and
`data/missions/mission_{zion,machinist,merovingian}_1.xml`.

Those mission XML files are attributed to MxOSource and, per community
discussion, **are not actually consumed by the server yet**. Three hand-written
missions that nothing reads is a fair summary of where quest support stands
across the whole scene.

**A diligence item, flagged honestly and not yet resolved:** the schema is
called `reality_hd.sql`, and `reality.sql` is mxoemu's. A shared schema name and
a stated debt to rajkosto's research are not proof of copied AGPL code — a
database schema is close to a set of facts, and re-deriving one from the same
protocol would land in a similar place. But we should not adopt mxo-hd's schema
wholesale on the assumption that MIT covers everything in it. Ours gets derived
from the client's own object model.

---

## Smaller and dead-end leads

* **MxOSource** — credited as the origin of the mission XML files that ship in
  mxo-hd's `data/missions/`. No independent repository located.
* **`sthhrs-labs/sthhrs-Matrix-Online-hdv-remap`** — a
  `MXO_EMULATOR_MASTER_REFERENCE.md` from this repo surfaces repeatedly in
  search results, but the repository returns 404 from the GitHub API. Deleted or
  made private. Search snippets only; **treat as unverified**. The one claim
  worth carrying forward is quoted in the next section, and it is a claim we can
  check ourselves rather than take on faith.
* **RaGEZONE threads** — forum discussion, no independent codebase.

## Console versions: a lead that does not exist

Worth stating plainly so nobody spends a weekend on it. **The Matrix Online was
Windows-only.** There was no console release and no evidence of a cancelled
console port; the publishing drama was Ubisoft backing out of co-publishing, not
a dropped platform.

The other Matrix games — *Enter the Matrix* (2003) and *The Matrix: Path of Neo*
(2005), both Shiny Entertainment — did ship on consoles, but they are unrelated
codebases on a different engine and share nothing with MxO's client. No
disassembly or decompilation project exists for either. There is no console-side
work to fold in.

---

## What everybody is still guessing at

This is the section that justifies the project.

Both emulators were built from **captured packets plus player memory**. That
method recovers the wire format well and the rules behind it badly. A capture
shows you that the server sent a damage number; it does not show you the formula
that produced it. Where the rules live is the client and the server, and the
server is gone.

The recurring statement in the community, and the clearest articulation of the
gap, is this one — quoted from the unverified `MXO_EMULATOR_MASTER_REFERENCE.md`
described above, so treat it as a hypothesis to test rather than an established
fact:

> damage formulas, hit/miss calculations, and interlock state machine
> transitions are encoded in the client `.exe`, and without disassembling those
> specific functions, emulator developers are guessing at formulas based on
> player-observed behavior from 2005-2009.

The testable prediction is clean: **if that is true, those functions are in
`client.dll` and a lift will find them.** If it is false — if the client only
renders numbers the server computed — the lift will show that too, by finding no
such code. Either answer is worth having, and neither has been checked.

### Update: the hypothesis was half right, and the good half is better than expected

The quote says those functions need disassembling out of the client. They do
not. **They are Python 2.3 bytecode**, shipped in `resource/Python/Monolith.ltp`,
and they decompile. See the README for the module list; the relevant names are
`combat.pyc`, `combatvalidate.pyc`, `combat_defines.pyc`, the seven-module
`interlock/` package including **`combat_resolution.pyc`**, the 24-module
`ability/` tree, `experience.pyc`, `missionvalidate.pyc`, `Missions/`, and a
53 KB `serverrules.pyc`.

So the community's diagnosis of *where* the rules live was right, and its
estimate of *how hard they are to get* was off by a wide margin. Seventeen
years of guessing at damage formulas, next to an archive in the install
directory.

That deserves a caveat rather than a victory lap: these are the retail 1.0
scripts, the emulators target the final live client, and four years of balance
patches sit in between. And a rule present in the client is not automatically
the rule the server enforced — see the last item below.

Known-unknown list, struck through as each is answered:

- [x] ~~Damage, hit/miss and defence formulas~~ — located, not yet read
- [x] ~~Interlock state machine and transitions~~ — `interlock/`, 7 modules
- [x] ~~Ability trees, prerequisites and load-out validation~~ — `ability/`, 24
      modules across hacker / soldier / spy / npc
- [x] ~~XP and levelling curves~~ — `experience.pyc`
- [~] The mission/quest system — `Missions/` plus `missionvalidate.pyc` and
      `resource/missions/definitions/sponsors.xml` are present; whether they
      constitute a complete description of a mission is unknown
- [ ] The GoProps object-property model in full
- [ ] Hyperspace / hardline transit rules
- [ ] Reputation curves
- [ ] **Which side actually decides.** The open question is no longer where the
      formulas are but which of them the client *evaluates* versus merely
      *predicts* for responsiveness. `clientrules` and `serverrules` sitting in
      the same archive suggests the split is explicit and named, which would be
      a considerable gift. It also maps the cheat surface, which is worth
      knowing before writing a server that trusts any of it.

## Why the client is the right place to look

A static recompilation of `client.dll` gives us something no packet capture can:
**the other end of every conversation, in source form.** Every opcode the
client parses, every field it reads, every branch it takes on a server response.
The protocol stops being something to infer from samples and becomes something
to read.

It also sidesteps the licence problem entirely. We do not need to copy AGPL code
to learn the protocol when we can read the client that speaks it. Facts
recovered from the binary are ours to write up and ours to implement, under MIT,
from scratch.

That is the plan: **lift the client, read the rules out of it, then write a
server that implements the rules the client actually expects** — rather than one
that replays the packets somebody once saw.

---

## Sources

- [rajkosto/mxoemu](https://github.com/rajkosto/mxoemu) — repository, and
  `Reality/Source/LICENCE.TXT` for the AGPL-3.0 determination
- [hdneo/mxo-hd](https://github.com/hdneo/mxo-hd) — repository, README and
  `LICENSE`
- [mxoemu.info](https://mxoemu.info/) — the running service
- [mxo.hardlinedreams.com](https://mxo.hardlinedreams.com/) — Hardline Dreams
  project site and its MxO emulation history page
- [The Matrix Online — Wikipedia](https://en.wikipedia.org/wiki/The_Matrix_Online)
  — platform, publisher and shutdown date
- [The Death and Rebirth of The Matrix Online — Vice](https://www.vice.com/en_us/article/53g5dk/the-death-and-rebirth-of-the-matrix-online)
  — background on the emulation scene
- [MxO Archive](https://www.mxoarchive.net/) — archived official site and patch
  notes
