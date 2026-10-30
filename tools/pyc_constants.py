"""Write the module-level constants of a Python 2.3 .pyc out as Python source.

stringtable_client (2.2 MB) and ltfxmap (253 KB) never finish decompiling, but
mission scripts only need their names: `ID_TUTORIAL_... = 123`, `FX_... = 456`.
Those are LOAD_CONST / STORE_NAME pairs at module level, so read them straight
from the bytecode.

    py -3.13 tools/pyc_constants.py work/py/Strings/stringtable_client.pyc -o work/mission_stubs/stringtable_client.py

The output is derived from the game's own files: it goes in work/, never git.
"""
import argparse

import xdis
from xdis.load import load_module
from xdis.op_imports import get_opcode_module


def constants(pyc):
    version, _, magic, co, ispypy, _, _ = load_module(pyc)
    opc = get_opcode_module(version)
    pending = None
    out = {}
    for ins in xdis.bytecode.get_instructions_bytes(co.co_code, opc, co.co_varnames,
                                                     co.co_names, co.co_consts):
        if ins.opname == "LOAD_CONST":
            pending = ins.argval
        elif ins.opname == "STORE_NAME" and pending is not None:
            if isinstance(pending, (int, float, str)):
                out[ins.argval] = pending
            pending = None
        else:
            pending = None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pyc")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    c = constants(a.pyc)
    with open(a.output, "w", encoding="latin-1") as fh:
        fh.write("# Constants read from %s by tools/pyc_constants.py\n" % a.pyc.replace("\\", "/"))
        for k, v in c.items():
            fh.write("%s = %r\n" % (k, v))
    print("%d constants -> %s" % (len(c), a.output))


if __name__ == "__main__":
    main()
