# Roadmap

Where the project is going, as of 2026-10-30. The earlier plan of record (the "convergence"
plan that got the client talking to the server) is preserved in [docs/devlog.md](docs/devlog.md);
it is done.

## Done

- Final client lifted (89,340 functions) and `launcher.exe` lifted for its login engine.
- The client boots, logs in to the server fork, loads Mega City and plays.
- The game's two scripted missions (Mission185, Mission986) run on the server under
  IronPython and play to completion.

## Next

1. **Mission events, for real.** Find the client messages still stood in for by `?m`
   commands: dialog replies, panel opened, hotbar shortcut, zone entry; and the server-side
   ones the scripts call: modal dialog (with its text), toggle view, door seal and unlock,
   teleport to a mission element, narration, temporary abilities. Method: the traced build's
   `--firsthit` plus `?m rpc` probes ([running.md](docs/running.md)).
2. **Combat from the client's Python.** Serve the interlock and ability rules
   ([COMBAT.md](docs/COMBAT.md)) in place of the fork's reconstructed formulas.
3. **A mission generator.** The original generator (Missile) was server-side and is lost.
   Build templates from what the client does contain (`sponsors.xml`, mission UI fields,
   the reward code in `rshooks.py`) and from public descriptions of the mission types and
   contact chains ([missions.md](docs/missions.md)).
4. **Host hardening.** Unmask SSE in CPUID now that the lifter handles it; re-run the retail
   1.0 client on the current host; a headless mode for unattended runs.
5. **Setup script.** A one-step setup that checks prerequisites and takes the user's own
   game files, per the repo rules' quick-start route.

## Not planned

- Distributing game files or generated code.
- Reconstructing the original procedural missions exactly; the data does not survive.
