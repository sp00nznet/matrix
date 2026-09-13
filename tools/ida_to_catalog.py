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
import bisect
import json
import os


def augment(out, cand_path, code_start, code_end):
    """Add recursive-descent starts that land in IDA's unattributed gaps.

    IDA attributes only 75.2% of this binary's .text. `disasm32.py` reaches
    95.9%, and the difference is not noise: its own log reports ~19,000
    functions reachable *only through data pointers*, which on an MSVC C++
    binary means virtual methods that nothing calls directly. IDA's auto
    analysis is conservative about those, so they never become functions, and a
    lift driven by IDA alone stubs every one of them.

    Where both tools agree a function exists they agree on its end 99.4% of the
    time, so this is not two tools disagreeing about the same code -- it is one
    tool seeing code the other never looked at.

    The candidate's `end` is a reachability bound rather than an extent, and
    summing those bounds double-counts wildly (2.6 MB of "new" code against a
    1.9 MB gap, because the bounds run on into functions IDA already has). So
    each added entry is clamped to the next known start, which is the only
    bound available that cannot swallow a neighbour.
    """
    with open(cand_path) as fh:
        cand = json.load(fh)

    known = {f["address"]: f for f in out}
    # Merged interval list of everything IDA already attributes.
    iv = sorted((f["address"], f["end"]) for f in out)
    merged = []
    for s, e in iv:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    starts = [m[0] for m in merged]

    def attributed(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and a < merged[i][1]

    extra = [f for f in cand["functions"]
             if f.get("entry_kind") == "start"
             and code_start <= f["address"] < code_end
             and f["address"] not in known
             and not attributed(f["address"])]

    # Clamp each new function to the next known start, IDA's or another new one.
    boundaries = sorted({f["address"] for f in out} |
                        {f["address"] for f in extra} |
                        {m[0] for m in merged} | {code_end})
    added = []
    for f in extra:
        a = f["address"]
        i = bisect.bisect_right(boundaries, a)
        limit = boundaries[i] if i < len(boundaries) else code_end
        end = min(f.get("end") or limit, limit, code_end)
        if end <= a:
            continue
        added.append({
            "address": a,
            "address_hex": f"0x{a:08X}",
            "name": f.get("name") or f"sub_{a:08X}",
            "end": end,
            "size": end - a,
            "num_blocks": 0,
            "num_instructions": f.get("num_instructions", 0),
            "is_thunk": f.get("is_thunk", False),
            "is_library": False,
            "entry_kind": "start",
            "source": "disasm32",
            "calls_to": [c for c in f.get("calls_to", []) if isinstance(c, int)],
        })

    out.extend(added)
    out.sort(key=lambda f: f["address"])
    return len(added), sum(f["size"] for f in added)


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
    ap.add_argument("--augment", metavar="FUNCTIONS_JSON",
                    help="add disasm32 starts that fall in IDA's unattributed "
                         "gaps (see augment() for why this is not optional in "
                         "practice on this target)")
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

    n_added = added_bytes = 0
    if args.augment:
        n_added, added_bytes = augment(out, args.augment,
                                       pe["code_start"], pe["code_end"])

    catalog = {
        "exe": fn["binary"],
        "image_base": fn["image_base"],
        "code_start": pe["code_start"],
        "code_end": pe["code_end"],
        "iat_count": len(pe.get("iat_map", {}) or {}),
        "source": "ida_funcs.json + ida_xrefs.json"
                  + (" + disasm32 gap fill" if args.augment else ""),
        "stats": {
            **fn.get("stats", {}),
            "total": len(out),
            "from_ida": len(out) - n_added,
            "from_disasm32": n_added,
            "disasm32_bytes": added_bytes,
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
    if n_added:
        print(f"from IDA: {s['from_ida']:,}   "
              f"from disasm32 gap fill: {n_added:,} "
              f"({added_bytes:,} bytes IDA left unattributed)")
    print(f"-> {args.output}")


if __name__ == "__main__":
    main()
