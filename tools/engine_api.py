#!/usr/bin/env python3
"""
Recover the engine API surface that the game's Python calls into.

Every name the scripts import but do not define is a C++ subsystem inside
client.dll, reached through Py_InitModule4. That list is the priority order for
lifting: a 7.6 MB binary is a bad place to start at the entry point and work
outwards, and this says which functions the gameplay actually depends on.

    python tools/engine_api.py work/py_src/Monolith -o analysis/engine_api.json

Emits, per engine module: the attributes called on it, with call counts and the
game modules that call them. Names and counts only -- facts about the binary's
interface, not a copy of anyone's code.

Decompiled Python 2 does not parse with the Python 3 ast module (print
statements, mostly), so this is a token-level scan rather than an AST walk. That
costs precision on nothing that matters here: we want identifiers, not
semantics.
"""

import argparse
import collections
import io
import json
import os
import re
import tokenize

# Python 2.3 modules the scripts reach for. Anything imported that is neither
# one of these nor present in the archive is an engine built-in or lives in
# another archive -- both are leads worth printing.
STDLIB = {
    "random", "traceback", "math", "time", "sys", "os", "re", "types",
    "getopt", "whrandom", "string", "copy", "struct", "operator",
}

IMPORT_LINE_RE = re.compile(r"^\s*(?:import|from)\s+.+$", re.M)
IMPORT_ITEM_RE = re.compile(r"([\w.]+)(?:\s+as\s+(\w+))?")
FROM_RE = re.compile(r"^\s*from\s+([\w.]+)\s+import\s+(.+)$", re.M)
DEF_RE = re.compile(r"^\s*(?:def|class)\s+(\w+)", re.M)
PARAM_RE = re.compile(r"^\s*def\s+\w+\s*\(([^)]*)\)", re.M | re.S)
ASSIGN_RE = re.compile(r"^\s*([\w,\s]+?)\s*(?:=[^=]|\+=|-=)", re.M)
FOR_RE = re.compile(r"^\s*for\s+([\w,\s]+?)\s+in\s", re.M)
EXCEPT_RE = re.compile(r"^\s*except\s+[\w.]+\s*,\s*(\w+)", re.M)


def bound_names(text):
    """Every name a module binds itself: imports, defs, params, assignments."""
    bound = set()
    for line in IMPORT_LINE_RE.findall(text):
        m = FROM_RE.match(line)
        if m:
            # `from X import *` binds names we cannot see; record the module so
            # star-imported constants do not masquerade as engine globals.
            bound.add(m.group(1).split(".")[0])
            for item in m.group(2).split(","):
                im = IMPORT_ITEM_RE.search(item)
                if im:
                    bound.add(im.group(2) or im.group(1).split(".")[0])
            continue
        for item in line.split("import", 1)[-1].split(","):
            im = IMPORT_ITEM_RE.search(item)
            if im:
                bound.add(im.group(2) or im.group(1).split(".")[0])
    bound.update(DEF_RE.findall(text))
    for params in PARAM_RE.findall(text):
        for p in params.split(","):
            p = p.split("=")[0].strip().lstrip("*")
            if p.isidentifier():
                bound.add(p)
    for group in list(ASSIGN_RE.findall(text)) + list(FOR_RE.findall(text)):
        for n in group.split(","):
            n = n.strip()
            if n.isidentifier():
                bound.add(n)
    bound.update(EXCEPT_RE.findall(text))
    return bound


def local_modules(root):
    """Module names that ship in the archive -- these are not engine modules."""
    names = set()
    for dirpath, _, files in os.walk(root):
        rel = os.path.relpath(dirpath, root).replace("\\", "/")
        if rel != ".":
            names.add(rel.split("/")[0])
        for f in files:
            if f.endswith(".py"):
                names.add(f[:-3])
    return names


def scan(path):
    """Yield (object, attribute) pairs for every `foo.bar` in the file."""
    with open(path, "rb") as fh:
        try:
            toks = list(tokenize.tokenize(fh.readline))
        except (tokenize.TokenError, IndentationError, SyntaxError):
            return
    prev = prevprev = None
    for tok in toks:
        if tok.type == tokenize.NAME and prev and prev.string == "." \
           and prevprev and prevprev.type == tokenize.NAME:
            yield prevprev.string, tok.string
        prevprev, prev = prev, tok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("-o", "--output", default="analysis/engine_api.json")
    args = ap.parse_args()

    shipped = local_modules(args.source)
    files = []
    missing_imports = collections.Counter()

    for dirpath, _, fnames in os.walk(args.source):
        for f in sorted(fnames):
            if not f.endswith(".py"):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, args.source).replace("\\", "/")
            text = open(path, encoding="utf-8", errors="replace").read()
            files.append((rel, path, bound_names(text)))

    # A module imported by the scripts but absent from the archive is either an
    # engine built-in or lives in another archive. Either way it is a lead.
    for rel, path, _ in files:
        text = open(path, encoding="utf-8", errors="replace").read()
        for line in IMPORT_LINE_RE.findall(text):
            for item in re.sub(r"^\s*from\s+|\s*import\s+", " ", line).split(","):
                im = IMPORT_ITEM_RE.search(item)
                if not im:
                    continue
                mod = im.group(1).split(".")[0]
                if mod not in shipped and mod not in STDLIB:
                    missing_imports[mod] += 1

    # obj.attr usage, split by whether the module bound `obj` itself.
    injected = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    resolved = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    for rel, path, bound in files:
        for obj, attr in scan(path):
            sink = resolved if obj in bound else injected
            sink[obj][attr][rel] += 1

    def pack(d, other=None):
        out = {}
        for obj, attrs in d.items():
            mods = sorted({m for c in attrs.values() for m in c})
            entry = {
                "total_calls": sum(sum(c.values()) for c in attrs.values()),
                "module_count": len(mods),
                "modules": mods,
                "attributes": {
                    a: sum(c.values())
                    for a, c in sorted(attrs.items(), key=lambda kv: -sum(kv[1].values()))
                },
            }
            if other is not None and obj in other:
                # The same name is a parameter or local somewhere else, so the
                # unbound uses here are far more likely to be a decompiler
                # artefact or a genuine local than a C++ subsystem. `subject`
                # and `directObject` are ability-function parameters, not
                # engine objects, and they top the raw call counts.
                entry["also_bound_in"] = len(
                    {m for c in other[obj].values() for m in c})
            out[obj] = entry
        # Breadth beats volume: a name used across many modules is a shared
        # subsystem; 480 calls inside two mission scripts is a local.
        return dict(sorted(out.items(),
                           key=lambda kv: (-kv[1]["module_count"],
                                           -kv[1]["total_calls"])))

    out = {
        "shipped_modules": sorted(shipped),
        "imported_but_absent": dict(missing_imports.most_common()),
        "engine_globals": pack(injected, resolved),
        "script_local": pack(resolved),
    }
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    json.dump(out, open(args.output, "w"), indent=1)

    eg = out["engine_globals"]
    print(f"{len(files)} modules scanned\n")
    print(f"Imported but not in this archive ({len(missing_imports)}):")
    for m, n in missing_imports.most_common():
        print(f"  {n:4}x  {m}")
    print(f"\nENGINE GLOBALS ({len(eg)}) -- injected by client.dll, never imported.")
    print("Ranked by module breadth. '~' marks a name that is a parameter or")
    print("local elsewhere, so its unbound uses are probably not a subsystem.\n")
    print("  mods  calls  methods  name")
    for name, e in list(eg.items())[:32]:
        flag = "~" if "also_bound_in" in e else " "
        print(f" {flag}{e['module_count']:4} {e['total_calls']:6} "
              f"{len(e['attributes']):8}  {name}")
    print(f"\n-> {args.output}")


if __name__ == "__main__":
    main()
