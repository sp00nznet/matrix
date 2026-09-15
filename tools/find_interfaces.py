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

# Both of these used to be retail constants. With a second target they have to
# be discovered: the image rebases (0x10000000 -> 0x62000000) and the register
# function lands at a different address.
REGISTER = None          # auto-detected; --register overrides
IMAGE_BASE = None        # read from the PE

# `push <imm>` as the lifter emits it, and the call that consumes them.
PUSH_RE = re.compile(r"PUSH32\(esp, (0x[0-9A-Fa-f]+)u?\);")
CALL_RE = None           # built in main(), once REGISTER is known
FUNC_RE = re.compile(r"^void (sub_[0-9A-Fa-f]+)\(void\)")


def pe_image_base(path):
    with open(path, "rb") as fh:
        blob = fh.read(0x400)
    lfanew = struct.unpack_from("<I", blob, 0x3C)[0]
    return struct.unpack_from("<I", blob, lfanew + 0x34)[0]


def detect_register(gen_dir, blob, secs, base):
    """Find the registration function without being told its address.

    Every interface registration pushes a pointer to a name like
    "ILTRenderDevice.Default" immediately before the call. So: scan for a push
    of a pointer that resolves to such a string, look at the call on the next
    line, and take whichever target that happens for most often. On both
    targets the winner is unambiguous -- it is called dozens of times and
    nothing else shares the pattern.
    """
    import collections
    push = re.compile(r"PUSH32\(esp, (0x[0-9A-Fa-f]+)u?\);")
    call = re.compile(r"RECOMP_CALL\((sub_[0-9A-Fa-f]+)\);")
    votes = collections.Counter()
    for name in sorted(os.listdir(gen_dir)):
        if not name.endswith(".c"):
            continue
        # A countdown, not a flag. The call does not immediately follow the
        # name push -- the holder is pushed in between -- and a plain flag gets
        # cleared by that second push before the call is ever seen. That is why
        # the first version of this detected the wrong function and found zero
        # holders.
        armed = 0
        for line in open(os.path.join(gen_dir, name), encoding="utf-8",
                         errors="replace"):
            m = call.search(line)
            if m:
                if armed:
                    votes[m.group(1)] += 1
                armed = 0
                continue
            p = push.search(line)
            if p:
                s = read_cstr(blob, secs, int(p.group(1), 16), base)
                if s and (s.startswith("ILT") or s.startswith("INet")
                          or s.startswith("CLT")):
                    armed = 4
                elif armed:
                    armed -= 1
    return votes


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


def read_cstr(blob, secs, va, limit=128, base=None):
    rva = va - (IMAGE_BASE if base is None else base)
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
    ap.add_argument("--register", default=None,
                    help="register function symbol; auto-detected if omitted")
    ap.add_argument("-o", "--output", default="analysis/engine_interfaces.json")
    ap.add_argument("--emit-c", default="src/runtime/interfaces_gen.c",
                    help="also write the holder table as C for the runtime")
    args = ap.parse_args()

    global IMAGE_BASE, REGISTER, CALL_RE
    blob, secs = load_sections(args.dll)
    IMAGE_BASE = pe_image_base(args.dll)

    votes = detect_register(args.gen, blob, secs, IMAGE_BASE)
    if votes:
        print("functions called just after an interface-name push:")
        for sym, n in votes.most_common(6):
            print("   %6d  %s" % (n, sym))
        print()
    REGISTER = args.register or (votes.most_common(1)[0][0] if votes else None)
    if not REGISTER:
        raise SystemExit("could not identify the register function; pass --register")
    CALL_RE = re.compile(r"RECOMP_CALL\(" + REGISTER + r"\);")
    print("image base %#010x, register function %s\n" % (IMAGE_BASE, REGISTER))

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
    if args.emit_c:
        os.makedirs(os.path.dirname(args.emit_c), exist_ok=True)
        hdr = (
            "/* Auto-generated by tools/find_interfaces.py -- DO NOT EDIT.\n"
            " * The engine interface holders client.dll waits to have filled in.\n"
            " */\n"
            "#include \"engine_iface.h\"\n\n"
            "const mxo_iface_t mxo_ifaces[] = {\n"
        )
        with open(args.emit_c, "w") as fh:
            fh.write(hdr)
            for e in found:
                fh.write('    { %su, "%s" },\n' % (e["holder_va"], e["interface"]))
            fh.write("};\nconst uint32_t mxo_iface_count = %d;\n" % len(found))
        print("-> %s" % args.emit_c)
    print("-> %s" % args.output)


if __name__ == "__main__":
    main()
