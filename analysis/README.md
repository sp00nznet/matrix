# analysis/ — derived facts, and which target each belongs to

Everything here is **metadata about a binary**. Under the repo rules, a symbol
map reconstructed from a proprietary binary is derived material, so **only run
statistics are tracked** (`recovery_score.json`, `phase3_codegen.json`,
`python_decompile*.json`). Every other file below is produced locally by the
pipeline from your own `client.dll` and is git-ignored (`analysis/*.json`).

Two targets are in play, so the naming matters:

| Prefix | Target |
|---|---|
| *(none)* | **retail 1.0** — the three discs, image base `0x10000000` |
| `final_` | **final community client** (`MxO_7.6005`), image base `0x62000000` |

| File | What it is |
|---|---|
| `client_pe.json` / `final_client_pe.json` | PE structure: sections, imports, exports, IAT map |
| `ida_funcs.json` / `final_ida_funcs.json` | IDA's function catalog — bounds, names, FLIRT/thunk flags |
| `ida_xrefs.json` / `final_ida_xrefs.json` | IDA's call graph, import usage, FPU density, fan-in |
| `functions_ida.json` | The **merged** catalog the lifter consumes (IDA + disasm32 gap fill) |
| `recovery_score.json` | disasm32 scored against IDA — see the P2 section of [docs/devlog.md](../docs/devlog.md) |
| `engine_interfaces.json` | The 28 LithTech interface holders and their addresses |
| `engine_api.json` | The 70 engine subsystems the game's Python calls into |
| `python_modules.json` | Manifest of the three `.ltp` archives (names and sizes only) |
| `python_decompile*.json` | Which modules decompiled, and their line counts |
| `missing_targets.json` | Entry points a direct call proves exist; fed back as lift seeds |
| `phase3_codegen.json` | Last lift run's statistics |
| `rtti.json` | RTTI recovery result (near-empty — this build shipped without it) |

**Not tracked:** `functions.json` / `final_functions.json` (disasm32's raw
catalogs) and `disasm.log`. They are tens of megabytes, per-target, and a
pipeline run regenerates them. The ignore rule is patterned rather than a list
of filenames, because the list version covered the first target's output and not
the second's.
