#!/usr/bin/env python3
"""
Decompile the Python 2.3 bytecode unpacked by ltp_extract.py.

Runs uncompyle6 in a subprocess per module so that one pathological file cannot
stall the batch -- which is not hypothetical: ltfxmap.pyc is a quarter-megabyte
of effect-table literal and takes uncompyle6 far longer than the other 56 modules
put together. It gets a longer budget than the rest rather than a special case.

    pip install uncompyle6
    python tools/decompile_ltp.py work/py/Monolith -o work/py_src/Monolith

Output is the game's own source. It belongs in work/, which is git-ignored, and
it does not get committed. What gets committed is the manifest this writes
alongside it: names, line counts, and which modules failed.
"""

import argparse
import json
import os
import subprocess
import sys

# ponytail: per-file subprocess + timeout instead of threads. uncompyle6 has no
# interruption hook, so a stuck parse can only be killed from outside.
DEFAULT_TIMEOUT = 90
SLOW_TIMEOUT = 1800
SLOW = {"ltfxmap.pyc"}


# uncompyle6 is a package with no __main__, so `python -m uncompyle6` does not
# run it. Drive the library entry point instead.
RUNNER = (
    "import sys;"
    "from uncompyle6.main import decompile_file;"
    "decompile_file(sys.argv[1], sys.stdout)"
)


def decompile(pyc, out, timeout):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        proc = subprocess.run(
            [sys.executable, "-c", RUNNER, pyc],
            stdout=fh, stderr=subprocess.PIPE, timeout=timeout,
            env={**os.environ, "PYTHONIOENCODING": "utf-8"},
        )
    return proc.returncode, proc.stderr.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="directory of extracted .pyc files")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--manifest", default="analysis/python_decompile.json")
    ap.add_argument("--skip-slow", action="store_true",
                    help="skip the known-slow modules entirely")
    args = ap.parse_args()

    results = []
    for root, _, files in os.walk(args.source):
        for name in sorted(files):
            if not name.endswith(".pyc"):
                continue
            pyc = os.path.join(root, name)
            rel = os.path.relpath(pyc, args.source).replace("\\", "/")
            slow = name in SLOW
            if slow and args.skip_slow:
                results.append({"module": rel, "status": "skipped"})
                print(f"  skip  {rel}")
                continue

            out = os.path.join(args.output, rel[:-1])  # .pyc -> .py
            try:
                rc, err = decompile(pyc, out, SLOW_TIMEOUT if slow else DEFAULT_TIMEOUT)
            except subprocess.TimeoutExpired:
                results.append({"module": rel, "status": "timeout"})
                print(f"  TIME  {rel}")
                continue

            lines = sum(1 for _ in open(out, encoding="utf-8", errors="replace"))
            # uncompyle6 reports partial failures in-band as a comment banner
            # rather than a non-zero exit, so the text has to be checked too.
            body = open(out, encoding="utf-8", errors="replace").read()
            broken = "Unsupported" in body or "decompile failed" in body.lower()
            status = "ok" if rc == 0 and not broken else "partial"
            results.append({"module": rel, "status": status, "lines": lines})
            print(f"  {status:7} {lines:6,}  {rel}" + (f"  {err.strip()[:70]}" if rc else ""))

    tally = {}
    for r in results:
        tally[r["status"]] = tally.get(r["status"], 0) + 1
    total = sum(r.get("lines", 0) for r in results)

    os.makedirs(os.path.dirname(args.manifest), exist_ok=True)
    with open(args.manifest, "w") as fh:
        json.dump({"tally": tally, "total_lines": total, "modules": results}, fh, indent=1)

    print(f"\n{tally}  {total:,} lines -> {args.output}")
    print(f"manifest: {args.manifest}")


if __name__ == "__main__":
    main()
