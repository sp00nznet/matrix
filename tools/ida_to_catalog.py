#!/usr/bin/env python3
"""
Merge IDA's function and xref exports into the catalog format pcrecomp lifts.

The toolbox pipeline normally lifts `analysis/functions.json` as produced by
`disasm/disasm32.py`, and `disasm/score_recovery.py` scores that against a
reference. On this target the reference is the better catalog: IDA finds 56,287
functions in `client.dll` with real bounds, FLIRT library classification and
thunk detection, and recursive descent has nothing to add to that. So lift from
IDA directly and skip the middle step.

    py -3.11 tools/ida/ida_funcs.py game/client.dll analysis/ida_funcs.json
    py -3.11 tools/ida/ida_xrefs.py game/client.dll analysis/ida_xrefs.json
    python tools/ida_to_catalog.py -o analysis/functions_ida.json

`ida_funcs` supplies bounds and flags; `ida_xrefs` supplies the call edges the
closure walk in run_lift.py needs. Everything emitted here is a fact about the
binary -- addresses, sizes, edges -- which is why the result is committed while
lifted C never is.

Every entry is emitted as `entry_kind: "start"`. IDA's catalog has no notion of
the alias entries disasm32 produces for shared epilogues and switch arms, and
claiming otherwise would make any future score against this file meaningless.
"""

import argparse
import json
import os


def load(path, what):
    if not os.path.exists(path):
        raise SystemExit(
            f"missing {what}: {path}\n"
            f"  run the matching tools/ida/ export first (see this file's docstring)"
        )
    with open(path) as fh:
        return json.load(fh)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--funcs", default="analysis/ida_funcs.json")
    ap.add_argument("--xrefs", default="analysis/ida_xrefs.json")
    ap.add_argument("--pe", default="analysis/client_pe.json")
    ap.add_argument("-o", "--output", default="analysis/functions_ida.json")
    args = ap.parse_args()

    fn = load(args.funcs, "ida_funcs export")
    pe = load(args.pe, "pe_analyze json")
    xr = load(args.xrefs, "ida_xrefs export")

    # ida_xrefs keys its per-function map by EA; tolerate str or int keys and
    # either a bare callee list or a dict carrying callees among other fields.
    raw = xr.get("functions", xr)
    edges = {}
    for key, val in (raw.items() if isinstance(raw, dict) else
                     ((f.get("ea"), f) for f in raw)):
        if key is None:
            continue
        callees = val.get("callees", []) if isinstance(val, dict) else val
        edges[int(key)] = [int(c) for c in callees]

    out = []
    for f in fn["functions"]:
        ea = f["ea"]
        out.append({
            "address": ea,
            "address_hex": f"0x{ea:08X}",
            "name": f["name"],
            "end": f["end"],
            "size": f["size"],
            "num_blocks": 0,          # not exported by IDA; unused by the lifter
            "num_instructions": 0,
            "is_thunk": f.get("thunk", False),
            "is_library": f.get("library", False),
            "entry_kind": "start",
            "calls_to": edges.get(ea, []),
        })

    catalog = {
        "exe": fn["binary"],
        "image_base": fn["image_base"],
        "code_start": pe["code_start"],
        "code_end": pe["code_end"],
        "iat_count": len(pe.get("iat_map", {}) or {}),
        "source": "ida_funcs.json + ida_xrefs.json",
        "stats": {
            **fn.get("stats", {}),
            "total": len(out),
            "with_edges": sum(1 for f in out if f["calls_to"]),
            "total_edges": sum(len(f["calls_to"]) for f in out),
        },
        "functions": out,
    }

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w") as fh:
        json.dump(catalog, fh)

    s = catalog["stats"]
    print(f"{s['total']:,} functions  "
          f"{s['with_edges']:,} with call edges  "
          f"{s['total_edges']:,} edges")
    print(f"library={s.get('library', 0):,} thunks={s.get('thunks', 0):,}")
    print(f"-> {args.output}")


if __name__ == "__main__":
    main()
