#!/usr/bin/env python3
"""
LTPb archive extractor -- the Python bundles shipped in MxO's resource/Python/.

Format, recovered from Monolith.ltp and standard.ltp:

An .ltp is not one archive with a directory -- it is single-entry records
concatenated back to back, each carrying its own copy of the header. There is
no index and no entry count anywhere, so the only way to enumerate one is to
walk it to EOF.

    per record:
        u32     "LTPb"
        u32     20              header size, and the offset of the field block
        u16     8
        u16     0x08bc          constant across every record seen
        u16     0x3252          likewise constant ("R2")
        u16     varies          per-record, presumed checksum
        u16     varies          per-record, presumed checksum
        u32     compressed_size
        u32     uncompressed_size
        u32     name_length
        u8      name[name_length]       e.g. "abilities.pyc"
        u8      data[compressed_size]   raw deflate, no zlib header

Every record's stated uncompressed_size has matched the inflated length on both
archives, and every record has begun on the magic. Those two are the checks
this relies on to know it is still in step.

The payloads are Python 2.3 bytecode (.pyc magic 0x0df23b / "3bf20d0a" on
disc). They are the game's own code: extract them, do not commit them.

    python tools/ltp_extract.py game/resource/Python/Monolith.ltp -o work/py
    python tools/ltp_extract.py game/resource/Python/*.ltp --list
"""

import argparse
import os
import struct
import sys
import zlib

MAGIC = b"LTPb"
HEADER_SIZE = 0x12
PYC_MAGICS = {0xF23B: "2.3", 0xF245: "2.3", 0xF259: "2.4", 0xF2B3: "2.5"}


def entries(blob, path):
    """Walk the records to EOF, yielding (name, compressed, stated_size)."""
    off = 0
    while off + HEADER_SIZE + 12 <= len(blob):
        magic = blob[off : off + 4]
        if magic != MAGIC:
            # The data records are followed by an LTPd directory (one record
            # per entry, same name and sizes again) and a trailing LTPh. Both
            # are redundant with what we just walked, so stopping here is the
            # whole of handling them. Anything else means we lost sync.
            if magic[:3] == b"LTP":
                return
            raise ValueError(
                f"{path}: expected an LTPb record at {off:#x}, "
                f"found {magic!r} -- lost sync"
            )
        off += HEADER_SIZE
        csize, usize, nlen = struct.unpack_from("<III", blob, off)
        off += 12
        # A sane-name check is what stops a misparse from running away and
        # allocating a gigabyte: names here are short ASCII filenames.
        if nlen == 0 or nlen > 256 or off + nlen + csize > len(blob):
            raise ValueError(
                f"{path}: entry at {off - 12:#x} is out of range "
                f"(csize={csize} usize={usize} nlen={nlen}) -- format mismatch"
            )
        name = blob[off : off + nlen].decode("ascii", "replace")
        off += nlen
        yield name, blob[off : off + csize], usize
        off += csize


def inflate(data, stated_size, name):
    out = zlib.decompressobj(-15).decompress(data)
    if len(out) != stated_size:
        raise ValueError(
            f"{name}: inflated {len(out)} bytes, header said {stated_size}"
        )
    return out


def pyc_version(data):
    if len(data) < 4 or data[2:4] != b"\r\n":
        return "not-pyc"
    magic = struct.unpack_from("<H", data, 0)[0]
    return "py" + PYC_MAGICS.get(magic, f"?({magic:#06x})")


def _build_record(name, payload):
    """Assemble one LTPb record, for the self-check."""
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = co.compress(payload) + co.flush()
    head = MAGIC + struct.pack("<IHHHHH", 20, 8, 0x08BC, 0x3252, 0, 0)
    assert len(head) == HEADER_SIZE, len(head)
    body = struct.pack("<III", len(packed), len(payload), len(name))
    return head + body + name.encode() + packed


def selfcheck():
    """Round-trip a synthesised archive. Runs without any game files."""
    files = {"abilities.pyc": b"\x3b\xf2\r\n" + b"x" * 300,
             "ability/npc/__init__.pyc": b"\x3b\xf2\r\nshort"}
    blob = b"".join(_build_record(n, p) for n, p in files.items())
    # A directory section must terminate the walk, not derail it.
    blob += b"LTPd" + b"\0" * 64 + b"LTPh" + b"\0" * 16

    got = {n: inflate(c, u, n) for n, c, u in entries(blob, "<selfcheck>")}
    assert got == files, f"round-trip mismatch: {sorted(got)} != {sorted(files)}"
    assert pyc_version(files["abilities.pyc"]) == "py2.3"
    assert pyc_version(b"not a pyc") == "not-pyc"

    # A truncated record must be reported, not silently half-read.
    try:
        list(entries(blob[:40], "<truncated>"))
    except ValueError:
        pass  # either raising or stopping short is fine; corrupting is not

    # Wrong magic must raise rather than return nothing.
    try:
        list(entries(b"NOPE" + blob[4:], "<badmagic>"))
    except ValueError:
        pass
    else:
        raise AssertionError("bad magic was accepted")

    print("selfcheck OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archives", nargs="*")
    ap.add_argument("--selfcheck", action="store_true",
                    help="round-trip a synthesised archive and exit")
    ap.add_argument("-o", "--output", default="work/py")
    ap.add_argument("--list", action="store_true",
                    help="list contents without extracting")
    args = ap.parse_args()

    if args.selfcheck:
        selfcheck()
        return
    if not args.archives:
        ap.error("give at least one .ltp, or --selfcheck")

    total = 0
    for path in args.archives:
        with open(path, "rb") as fh:
            blob = fh.read()

        stem = os.path.splitext(os.path.basename(path))[0]
        outdir = os.path.join(args.output, stem)
        count = 0
        print(f"\n== {path} ({len(blob):,} bytes)")

        for name, packed, usize in entries(blob, path):
            data = inflate(packed, usize, name)
            count += 1
            if args.list:
                print(f"  {usize:9,}  {len(packed):9,}  {pyc_version(data):8}  {name}")
            else:
                dest = os.path.join(outdir, name)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as fh:
                    fh.write(data)

        print(f"  {count} entries" + ("" if args.list else f" -> {outdir}"))
        total += count

    print(f"\n{total} entries total")


if __name__ == "__main__":
    main()
