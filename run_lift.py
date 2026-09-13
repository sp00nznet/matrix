#!/usr/bin/env python3
"""The Matrix Online lift driver (pipeline Phase 3).

Lifts `client.dll` from the IDA-derived catalog produced by
`tools/ida_to_catalog.py`. Uses the shared `tools/lift/generate.py` rather than
forking it.

    python run_lift.py                    # export closure, 2000 functions
    python run_lift.py --max 8000
    python run_lift.py --roots 0x10123456
    python run_lift.py --all

## Why this is shorter than the other projects' drivers

Two of the things a driver here normally has to do are unnecessary on this
target, and both are worth stating so nobody adds them back on reflex.

**No `true_extent()` branch walk.** Force Commander's driver walks branches from
each entry to discover where a function really ends, because `disasm32.py`
records a *reachability bound* rather than an extent -- trusting it decoded a
megabyte of unrelated code as one body, and clamping to the next entry cut
functions in half at false starts. IDA records real function ranges, so
`f['end']` is an extent and is used directly. The residual risk is different and
smaller: IDA's `end_ea` is the end of a function's *main chunk*, so a function
with separated tail chunks is under-read rather than over-read. That fails
loudly at runtime (a missing tail) instead of silently (a swallowed neighbour),
which is the better direction to be wrong in.

**No closure cap for correctness, only for time.** 56,287 functions is more than
any other target here, but half of them are under 32 bytes -- STLport container
accessors, not game logic. The closure from the five exports is the startup
path; everything else gets a stub that names its own address, so a run says what
to add next.

## Roots

`client.dll` is a DLL and its entry point is `DllMain`, which does almost
nothing. The real roots are the LithTech client contract, which is the whole
reason bring-up on this target is tractable:

    InitClientDLL  RunClientDLL  TermClientDLL  ErrorClientDLL  SetMasterDatabase

Those five plus `DllMain` are the default roots.
"""
import argparse
import collections
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.join(_HERE, '..', 'tools', 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32           # noqa: E402
from generate import (linear_disassemble_function,          # noqa: E402
                      lift_function_linear, write_chunk)
from lift32 import Lifter                                   # noqa: E402
from pe_analyze import analyze_pe, build_iat_map            # noqa: E402

DLL = os.path.join(_HERE, 'game', 'client.dll')
CATALOG = os.path.join(_HERE, 'analysis', 'functions_ida.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')

# The LithTech client-DLL contract. Names only: the addresses are read from the
# PE export directory at runtime rather than written down here.
#
# The first version of this file did write them down, and got SetMasterDatabase
# wrong -- 0x1F1BF0 for a function that lives at 0x1F1DB0. The catalog lookup
# caught it, but only because a missing root happens to be checkable. There is
# no reason to transcribe a table the binary already carries.
ROOT_EXPORTS = (
    'InitClientDLL',
    'RunClientDLL',
    'TermClientDLL',
    'ErrorClientDLL',
    'SetMasterDatabase',
)


def closure(byaddr, roots, limit):
    """Breadth-first call-graph closure from `roots`, at most `limit` functions.

    Breadth-first on purpose: the first N functions reached from the entry
    points are the startup path, which is what a first bring-up needs. A
    depth-first walk would spend the whole budget down one arbitrary branch.
    """
    seen, order = set(), []
    q = collections.deque(r for r in roots if r in byaddr)
    seen.update(q)
    while q and len(order) < limit:
        a = q.popleft()
        order.append(a)
        for t in byaddr[a].get('calls_to', ()):
            if t in byaddr and t not in seen:
                seen.add(t)
                q.append(t)
    return order


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', default=DLL)
    ap.add_argument('--catalog', default=CATALOG)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--roots', default='', help='extra root VAs, comma separated')
    ap.add_argument('--max', type=int, default=2000, help='closure cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--skip-library', action='store_true',
                    help='stub FLIRT-identified CRT/STLport functions')
    ap.add_argument('--split', type=int, default=400)
    args = ap.parse_args()

    if not os.path.exists(args.catalog):
        sys.exit(f'no catalog at {args.catalog}\n'
                 f'  run tools/ida_to_catalog.py first')

    info = analyze_pe(args.dll)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d'
          % (info.image_base, cs, ce, len(iat)))

    cat = json.load(open(args.catalog))
    funcs = [f for f in cat['functions'] if cs <= f['address'] < ce]
    byaddr = {f['address']: f for f in funcs}
    print('[*] catalog: %d functions, %d call edges'
          % (len(funcs), cat.get('stats', {}).get('total_edges', 0)))

    # analyze_pe yields ExportEntry objects; older callers saw plain dicts.
    def _f(e, k):
        return e[k] if isinstance(e, dict) else getattr(e, k)

    by_name = {_f(e, 'name'): info.image_base + _f(e, 'rva')
               for e in info.exports}
    absent = [n for n in ROOT_EXPORTS if n not in by_name]
    if absent:
        print('[!] export(s) not present in this binary: %s' % ', '.join(absent))

    entry = info.image_base + info.entry_point_rva
    roots = [by_name[n] for n in ROOT_EXPORTS if n in by_name]
    roots.append(entry)
    roots += [int(x, 0) for x in args.roots.split(',') if x.strip()]

    print('[*] roots: %s, DllMain@0x%08X'
          % (', '.join('%s@0x%08X' % (n, by_name[n])
                       for n in ROOT_EXPORTS if n in by_name), entry))
    missing = [r for r in roots if r not in byaddr]
    if missing:
        print('[!] %d root(s) not in catalog: %s'
              % (len(missing), ', '.join('0x%08X' % m for m in missing)))

    if args.all:
        chosen = sorted(byaddr)
        print('[*] lifting ALL %d functions' % len(chosen))
    else:
        chosen = closure(byaddr, roots, args.max)
        print('[*] closure: %d of %d (cap %d)' % (len(chosen), len(funcs), args.max))
    chosen_set = set(chosen)

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    blob = open(args.dll, 'rb').read()
    text = [s for s in info.sections if s.name == '.text'][0]
    code = blob[text.raw_offset:
                text.raw_offset + min(text.virtual_size, text.raw_size)]

    # Pass the known function set. Without it lift32 emits a direct
    # RECOMP_CALL(sub_XXXXXXXX) for every call immediate it decodes, including
    # the nonsense ones: padding between functions decodes as `call` with
    # targets outside the image, and the generated C then fails to compile on
    # an undeclared function. With the set, an unknown target becomes
    # RECOMP_ICALL(imm), which reports at runtime instead of breaking the build.
    lifter = Lifter(iat_map=iat, lifted=set(byaddr))
    os.makedirs(args.out, exist_ok=True)

    entries, chunk, idx, errors, stubbed_lib = [], [], 0, 0, 0
    t0 = time.time()

    for addr in sorted(chosen_set):
        f = byaddr[addr]
        name = 'sub_%08X' % addr
        end = min(f['end'], ce)

        if args.skip_library and f.get('is_library'):
            chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n'
                          % (name, addr), addr, name))
            entries.append((addr, name))
            stubbed_lib += 1
            continue

        if end <= addr:
            chunk.append(('void %s(void) { }\n' % name, addr, name))
            entries.append((addr, name))
            continue

        try:
            insns, leaders = linear_disassemble_function(md, code, cs, addr, end)
            if not insns:
                chunk.append(('void %s(void) { }\n' % name, addr, name))
            else:
                chunk.append((lift_function_linear(lifter, name, insns,
                                                   leaders, addr), addr, name))
            entries.append((addr, name))
        except Exception as e:                               # noqa: BLE001
            chunk.append(('/* ERROR %s: %s */\nvoid %s(void) {}\n' % (name, e, name),
                          addr, name))
            entries.append((addr, name))
            errors += 1

        if len(chunk) >= args.split:
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []
            print('[*]   %d/%d (%d err)' % (len(entries), len(chosen_set), errors),
                  flush=True)

    # Every catalogued function we did not lift becomes a stub that names its
    # own address and stops. It is still registered in the dispatch table, so an
    # indirect call into it resolves and reports which address to add next,
    # instead of failing as an unresolved VA with no clue attached.
    for a in sorted(byaddr):
        if a not in chosen_set:
            name = 'sub_%08X' % a
            chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n'
                          % (name, a), a, name))
            entries.append((a, name))
            if len(chunk) >= args.split:
                write_chunk(args.out, idx, chunk)
                idx += 1
                chunk = []
    if chunk:
        write_chunk(args.out, idx, chunk)
        idx += 1

    print('[*] %d functions in %d files, %d lifted, %d stubs, %d errors, %.0fs'
          % (len(entries), idx, len(chosen_set) - stubbed_lib,
             len(entries) - len(chosen_set) + stubbed_lib, errors, time.time() - t0))
    print('[*] -> %s' % args.out)


if __name__ == '__main__':
    main()
