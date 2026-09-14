#!/usr/bin/env python3
"""
Recover the LithTech engine interfaces client.dll expects to be handed.

P6 turns on a single mechanism. `client.dll` does not create its engine
interfaces; it declares **holders** for them and waits for the engine to fill
each one in. A holder is a small object -- vtable at +0, interface pointer at
+4 -- and the interface pointer stays NULL until something installs it.

That is why `RunClientDLL` faults: it reaches a holder whose +4 is still the
zero its constructor left, and calls through it.

Each holder is registered by a static initialiser that looks like

    push 0                      ; flags
    push <name>                 ; .rdata, e.g. "INetMgr.Default"
    push <holder>               ; .data
    call sub_101F1A00           ; register(holder, name, flags)

so the whole set is recoverable statically from the lifted C: find the calls,
walk back for the two pushed pointers, and read the name out of the image.

    python tools/find_interfaces.py -o analysis/engine_interfaces.json

The result is the P6 work list -- every interface the host must supply before
the client can run -- and it is metadata about the binary (addresses and
interface names), not a copy of it.
"""

import argparse
import json
import os
import re
import struct

REGISTER = "sub_101F1A00"
IMAGE_BASE = 0x10000000

# `push <imm>` as the lifter emits it, and the call that consumes them.
PUSH_RE = re.compile(r"PUSH32\(esp, (0x[0-9A-Fa-f]+)u?\);")
CALL_RE = re.compile(r"RECOMP_CALL\(" + REGISTER + r"\);")
FUNC_RE = re.compile(r"^void (sub_[0-9A-Fa-f]+)\(void\)")


def load_sections(path):
    """(rva, size, raw) for each section, so an RVA can be read from the file."""
    blob = open(path, "rb").read()
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    nsec = struct.unpack_from("<H", blob, pe + 6)[0]
    opt = struct.unpack_from("<H", blob, pe + 20)[0]
    secs = []
    off = pe + 24 + opt
    for i in range(nsec):
        s = off + i * 40
        vsize, va, rsize, raw = struct.unpack_from("<IIII", blob, s + 8)
        secs.append((va, max(vsize, rsize), raw))
    return blob, secs


def read_cstr(blob, secs, va, limit=128):
    rva = va - IMAGE_BASE
    for sva, size, raw in secs:
        if sva <= rva < sva + size:
            off = raw + (rva - sva)
            end = blob.find(b"\0", off, off + limit)
            if end < 0:
                return None
            s = blob[off:end]
            # An interface name is printable ASCII; anything else means the
            # pointer was not a string and this is not a registration we want.
            if s and all(32 <= c < 127 for c in s):
                return s.decode("ascii")
            return None
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default="src/recomp/gen")
    ap.add_argument("--dll", default="game/client.dll")
    ap.add_argument("-o", "--output", default="analysis/engine_interfaces.json")
    args = ap.parse_args()

    blob, secs = load_sections(args.dll)
    found, seen = [], set()

    for name in sorted(os.listdir(args.gen)):
        if not name.endswith(".c"):
            continue
        cur = "?"
        # Keep a short trailing window of pushes; the registration's operands
        # are the last few before the call.
        window = []
        for line in open(os.path.join(args.gen, name), encoding="utf-8",
                         errors="replace"):
            m = FUNC_RE.match(line)
            if m:
                cur, window = m.group(1), []
                continue
            p = PUSH_RE.search(line)
            if p:
                window.append(int(p.group(1), 16))
                if len(window) > 6:
                    window.pop(0)
            if CALL_RE.search(line) and len(window) >= 2:
                # pushed right-to-left: ..., flags, name, holder
                holder, iname = window[-1], window[-2]
                text = read_cstr(blob, secs, iname)
                if text and (holder, text) not in seen:
                    seen.add((holder, text))
                    found.append({"interface": text,
                                  "holder_va": "0x%08X" % holder,
                                  "registered_by": cur,
                                  "file": name})
                window = []

    found.sort(key=lambda e: e["interface"])
    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    json.dump({"count": len(found), "interfaces": found},
              open(args.output, "w"), indent=1)

    print("%d engine interface holder(s)\n" % len(found))
    for e in found:
        print("  %-34s holder %s  %s" % (e["interface"], e["holder_va"],
                                         e["registered_by"]))
    print("\n-> %s" % args.output)


if __name__ == "__main__":
    main()
