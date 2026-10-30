# Missions

## What the client ships

`Monolith.ltp` holds `Missions/Mission.py` (a base class) and two complete missions:

| Script | What it is | Goals |
|---|---|---|
| `Mission185.py` | The original Construct tutorial: camera, movement, targeting, inventory, hotbar, map, dojo combat, hacking, hardline exit | 28 |
| `Mission986.py` | The later new-player flow: red pill, calibration, loot, equip, interlock, conning, firearms, level-up, exit | 36 |

A mission is **phase → stage → goal**. The script is a class of 12 callbacks
(`OnStartOfMission`, `OnStartOfStage`, `OnGoalComplete`, `OnPhaseComplete`,
`OnMissionEvent`, `OnObjectTargeted`, `OnPlayerEnterZone`, `OnModalDialogueAccepted`,
`OnToggledViewActivated`, `OnInventoryItemsSwapped`, `OnShortcutAdded`,
`OnMovementUpdated`) and drives the game through a global `mission` object with 21 verbs
(`completeGoal`, `activateStage`, `scheduleEvent`, `sendModalMessage`, `toggleView`,
`sealDoor`, `unlockDoor`, `teleportToMissionElement`, `grantExperience`,
`addTempAbility`, and others).

Every other mission in the live game came from a server-side generator (an internal tool
called Missile, named in `resource/missions/definitions/sponsors.xml`). It was never
shipped, so those missions cannot be recovered from the client.

## How the server runs them

The server fork's `hds/world/Missions/ScriptedMissions.cs` loads the decompiled scripts
under IronPython (both are Python 2), implements the 21 verbs, and calls the callbacks.
Both missions appear in Tyndall's list as "Script: Mission 185" and "Script: Mission 986".

No mission definition data survives, so two rules are inferred from the scripts:

- Goal N belongs to stage N; completing the current stage's goal starts stage N+1.
- The script's highest `*_GOAL` constant completes the mission.

About 18 goals are completed by no script code at all: the engine completed them when the
player did something. Without the goal-type data, the server matches each to an action by
the goal's name: `TALKING` → using an NPC, `DOJO` → a door, `LOOT` → accepting loot,
`COMBAT` → attacking, `CELLPHONE` → targeting. This mapping is a guess.

## What is wired to real client actions

| Client action | Message | Reaches |
|---|---|---|
| Moving, turning the camera | view-update stream | `OnMovementUpdated` |
| Targeting | `0x151` | `OnObjectTargeted`, target goals |
| Using an object (NPC, door, container, computer) | `0xC7` / `0xC8` | interact and door goals |
| Moving or equipping an item | `0x65` / `0x63` | `OnInventoryItemsSwapped` |
| Attacking | `0x40` / `0x41` | combat goals |
| Accepting loot | `0x11F` | loot goals |

Still stood in for with chat commands (`?m ...`), because the message has not been found:

| Event | Stand-in |
|---|---|
| Answering a mission dialog | `?m ok` / `?m no` |
| Opening a panel (inventory, actions, map) | `?m view N` |
| Adding a hotbar shortcut, entering a zone | `?m goal N` |

Mission dialogs are shown with the client's alert pop-up (chat type `0x17`); its text is
not right yet. Door sealing, teleports, narration and temporary abilities only print a chat
line. `grantExperience` sends real XP.

## Chat commands

```
?m status          phase, stage, goals done, dialogs open
?m ok | ?m no      answer the newest dialog (left / right button)
?m view N          the player opened panel N
?m goal N          complete goal N
?m event N         fire mission event N now
?m rpc HEX...      send raw message bytes to the client (debugging)
?m chat HH text    send a chat message of type HH (debugging)
```

## Content from outside the client

Public sources (the MxO wiki, Data Node One mirrors, player transcripts) describe the
mission types (assassination, courier, retrieval, rescue, escort, infiltration,
recruitment), the organisation contacts (Tyndall, Agent Gray, Flood, plus exiles met in
person), reputation gates (16, 17, 25, 38) and Tyndall's eight-mission training chain. They
are enough to write templates for a new generator; no original templates have surfaced.
