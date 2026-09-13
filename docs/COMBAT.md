# Interlock combat, recovered

The interlock combat system — the thing every emulator has had to guess at — is
**1,015 lines of Python across seven modules**, plus 3,064 lines of ability
definitions across twenty-four more. All of it decompiles cleanly from
`resource/Python/Monolith.ltp`.

This document records what is there and how it compares to the best existing
reconstruction. It deliberately does not reproduce the game's code: what follows
is the interface — function names, arities, call structure — which are facts
about the binary. The source itself stays in `work/`, git-ignored.

Reproduce with:

```bash
python tools/ltp_extract.py game/resource/Python/Monolith.ltp -o work/py
python tools/decompile_ltp.py work/py/Monolith -o work/py_src/Monolith --skip-slow
python tools/engine_api.py work/py_src/Monolith -o analysis/engine_api.json
```

---

## What the `interlock/` package contains

Seven modules. The function inventory is the map:

**`combat_resolution.py`** — the core, 395 lines.

```
generateVulnerabilities(player)
generateExploits(player1, player2)
generateExploitsForPlayers(player1, player2)
calcDamageWithGun(itemBaseDamage, itemDeltaDamage, tactical_setting)
calcDamageResistFromLevelDifference(winner_level, loser_level, loser_current_toughness)
calcDamageForLoser(winner, loser, result)
calculateDamage(winner, loser, result)
determineAttacker(player1, player2, combat_range)
determineSuccessfulOutcome(winner, loser, result)
determineResultForStandardCombat(attacker, defender, failed_to_withdraw, result)
determineResultOfWithdrawalRequest(attacker, defender, result)
isAbilityMartialArt(ability)
```

**`combat_modifiers.py`** — the modifier stack, 136 lines.

```
getTacticalMod(tacticalSetting, opponentTacticalSetting)
getTacticSpecificDamageBoostPercent(tacticalSetting)
getTacticSpecificArmorPiercingPercent(tacticalSetting)
getBaseRangedValue(player)
getRangedVsMeleeAttackModifier(attacker, defender, result)
getRangedVsMeleeDefenseModifier(attacker, defender, result)
getOpponentCountModifier(player)
getMultiOnOnePenalty(player)
getAttackerMods(attacker, defender, result)
getDefenderMods(attacker, defender, result)
```

**`combat_abil_level.py`** (126) — ability level selection, mastery, weapon vs
martial-art resolution, and `SetCombatTargetSettings`.
**`combat_utility.py`** (197) — predicates that gate the special cases:
`finishingMoveAllowed`, `killingMoveAllowed`, `possibleDisarmMove`,
`shortMoveAllowed`, `determineBulletTime`, `isPlayerBeingGanged`,
`isPlayerCombatProne`, `isPreferredTactical`, `isPreferredFinishingTactical`.
**`combat_innerstrength.py`** (71) — five IS-cost functions, split by
self-defence / martial art / gun and by winner / loser.
**`combat_free_attacks.py`** (84) — ability, weapon and melee free attacks.
**`combat_abil_level.py`**, **`combat_innerstrength.py`** and
**`combat_modifiers.py`** are where the tuning numbers live.

The shape of the system is legible straight off the function list: combat
resolves as **winner/loser per exchange**, not attacker-rolls-to-hit.
`determineAttacker` picks who is attacking from the two players and the range,
`determineSuccessfulOutcome` decides the exchange, and only then does
`calcDamageForLoser` run. Vulnerabilities and exploits are *generated per
player and per pairing* (`generateVulnerabilities`, `generateExploits`) rather
than being static stats.

---

## Compared to the best existing reconstruction

Hardline Dreams ships `hds/resources/Data/CombatFormulas.cs` (MIT), which is
the most serious public attempt at these rules. Its header is candid about
method: the formulas are described as derived from *strings* in `client.dll` —
it cites a combat-result structure and debug field names at `.rdata` addresses,
listing `DamageTaken`, `DamageAbsorbed`, `Modifier`, `CombatResult`,
`TacticalValue`, and per-tactic `DefenseVulnerability` / `PowerVulnerability` /
`SpeedVulnerability`.

That is reading the *labels* on the struct and inferring the arithmetic. It got
a lot right.

**Confirmed by the Python:**

* The vulnerability system is real, and it is per-tactic.
  `generateVulnerabilities(player)` exists.
* Damage absorption is a distinct step from raw damage — `calcDamageForLoser`
  and the toughness parameter in `calcDamageResistFromLevelDifference`.
* Level scaling is real and it is specifically a **level-difference** effect:
  `calcDamageResistFromLevelDifference(winner_level, loser_level,
  loser_current_toughness)`.
* Tactics matter to damage, and armour piercing is tactic-specific
  (`getTacticSpecificDamageBoostPercent`, `getTacticSpecificArmorPiercingPercent`).

**Where it diverges — and this is the useful part:**

* HDS models the tactic modifier as a **vulnerability multiplier**: pick the
  target's vulnerability for the attacker's tactic, return `1.0 + vuln`. The
  actual `getTacticalMod(tacticalSetting, opponentTacticalSetting)` takes
  **both** tactics and returns a flat bonus when the attacker's tactic is
  "preferred" against the opponent's, otherwise zero. It is a
  rock-paper-scissors matchup test, not a per-target scalar. Different
  function, different shape, different tuning.
* HDS has no equivalent of the **exploit** system
  (`generateExploits(player1, player2)`), which is generated per *pair* of
  combatants.
* HDS has no `getOpponentCountModifier` / `getMultiOnOnePenalty` — ganging
  rules. `isPlayerBeingGanged` exists too.
* HDS has no ranged-vs-melee asymmetry; the Python has separate attack and
  defence modifiers for it.
* Inner Strength cost in HDS is a flat per-ability value. The Python computes
  it from tactic **and** weapon class, with different functions for the winner
  and the loser of an exchange.

None of this makes HDS wrong to have tried — it is a good reconstruction from
labels alone. It does mean a server using it will not reproduce 2005 combat,
and now there is something better to check against.

---

## Can this be tested against a running server? Yes — three ways

Ordered by cost.

**1. Static diff (cheapest, do first).** HDS's tuning values are in
`data/abilityIDs.csv` and the constants in `CombatFormulas.cs`. The Python's
equivalents are in `ability/defines.py` (35 module constants), the
discipline modules, and `combat_modifiers.py`. Where both name the same
quantity, they either agree or they do not, and disagreement is a finding
without running anything. This needs no client and no server.

**2. Differential harness (the real test).** The recovered modules are Python
and they are *runnable*. Stand the seven interlock modules up against stub
objects for the engine globals they touch — `Interlock`, `AbilityInv`,
`combat`, `PlayerCharacter`, `CharMvt` — feed both it and a port of HDS's
`CombatFormulas` the same combatant state, and diff the outputs. This is the
same differential-testing method `pcrecomp`'s `lift/difftest.py` uses against
Unicorn, applied one level up. It gives a per-field disagreement list rather
than a yes/no.

The engine stubs are the work here, and `analysis/engine_api.json` says exactly
how much: `Interlock` is 28 methods, `combat` 34, `AbilityInv` 41. Not small,
but bounded and enumerated.

**3. Live A/B against HDS.** Run an HDS server, connect a real client, fight a
mob, and compare the damage numbers to what the recovered Python predicts for
the same inputs. This is the most convincing and the most work, and it is worth
doing only after (2) narrows down where they disagree.

### The sidecar

Your sidecar idea is the natural endpoint of (2), and it is a good one. The
recovered rules are already an executable rules engine. A server — HDS, or
ours — can delegate combat resolution to it over a socket rather than
reimplementing it, which means the rules stay the client's own rather than
becoming somebody's transcription of them.

Two caveats to design around rather than discover later:

* **Python 2.3.** The modules use `whrandom` and Python 2 syntax. Either run
  them under a real 2.x, or port them — and if you port, the port is the thing
  that needs differential testing against the original.
* **Retail 1.0 vs the final client.** These rules are from the February 2005
  discs. HDS targets the last live client, four years of balance patches later.
  Expect the *structure* to hold and the *numbers* to have moved. A disagreement
  in a constant is weak evidence of a bug; a disagreement in the shape of a
  function is strong evidence.

---

## What this does not settle

`getTacticalMod` returning a flat bonus tells us how the client computes a
modifier. It does not tell us whether the **server** used the same number, or
whether the client was merely predicting for responsiveness and taking the
server's answer when it arrived.

`clientrules.py` and `serverrules.py` sit in the same archive, which suggests
the split is explicit and named. Reading those two is the next task, and it is
the one that determines how much of this a server can simply trust.
