#!/usr/bin/env python3
"""Generate the import-bridge layer for client.dll (pipeline Phase 3b).

The lifted code reaches every import through its IAT slot VA. This emits a C
shim per import plus a {iat_va -> shim} table the runtime installs before entry.

    python gen_imports.py                 # -> src/runtime/imports_gen.c

## The three kinds of import, and why the distinction is load-bearing

`client.dll` imports 479 symbols and they do not all work the same way.

**stdcall (226).** The callee pops the arguments, so a shim must pop exactly the
right number. That count is not cosmetic: get it wrong and the simulated stack
silently desynchronises, every value read after the call is garbage, and the
crash lands nowhere near the cause. So we do not type them --
`pcrecomp/tools/pe/stdcall_argc.py` derives each one from the `_Name@N`
decoration in the Windows SDK's 32-bit import libraries.

**cdecl (252).** The *caller* cleans up, so these pop nothing and the resolver
cannot derive a count for them -- correctly, because there is nothing to derive.
181 come from `MSVCR71.dll` and 71 from `pythonMXO.dll`, and both are cdecl by
convention. `STDRET(0)` is right for all of them.

**Data (the trap).** Some of what `client.dll` imports from `pythonMXO.dll` is
not code at all. `pythonMXO` exports 693 symbols from `.text` and **115 from
`.data`** -- `PyType_Type`, `PyString_Type`, `PyExc_AttributeError`,
`_Py_NoneStruct`, `_Py_TrueStruct`, `_Py_ZeroStruct` and friends. The client
*dereferences* those slots; it never calls them.

Installing a function shim in a data slot would not fail at build time and would
not fail at the call site. It would hand the client a code address where it
expects a `PyTypeObject`, and the first type check reads whatever the shim's
prologue happens to encode. So data imports are classified by looking at which
section each export lands in, emitted into their own table, and given real
storage rather than a stub.

That classification is done by reading the dependency DLLs in `game/`, so it is
a fact about this install rather than a list anyone has to maintain.
"""
import argparse
import collections
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, '..', 'tools', 'tools', 'pe'))

from pe_analyze import analyze_pe, build_iat_map      # noqa: E402
from stdcall_argc import ArgcResolver                 # noqa: E402

# Two targets now: the retail discs and the final community client. Neither the
# binary nor the folder its dependencies live in can be assumed.
DLL = os.path.join(_HERE, 'game', 'client.dll')
OUT = os.path.join(_HERE, 'src', 'runtime', 'imports_gen.c')

# Where to look for each dependency, so data-vs-code can be decided by reading
# the DLL rather than by maintaining a list. Anything not found here is assumed
# to be code, which is the safe default for the system DLLs (none of them export
# data that this binary imports).
SEARCH = ('', 'p_dlls')

# Imports with real bodies in src/runtime/shims_impl.c get an extern declaration
# instead of a stub. The bridge table still points at them, so promoting a stub
# to a real body is a one-line change here.
#
# A name in here that this binary does not import is a silent failure: the stub
# stays, the hand-written body is never called, and it looks exactly like the
# shim not working. main() refuses to generate in that case. That check earned
# its keep immediately -- memset, memcpy, memcmp, strlen and operator
# new/delete all looked like obvious entries and none of them are imported.
# MSVC 7.1 emits them as intrinsics, so they are already inside the lifted code.
ONE_TARGET_ONLY = {'RajkoD3DCreate9', 'Direct3DCreate9'}

HAND_WRITTEN = {
    # guest_threads.c: guest threads as fibers
    '_beginthreadex', '_beginthread', 'CreateThread', 'ResumeThread', 'SuspendThread',
    'ExitThread', 'TerminateThread', 'GetExitCodeThread', 'SetThreadPriority',
    'GetCurrentThread', 'RaiseException',
    'GetSystemInfo', 'IsProcessorFeaturePresent', 'RegOpenKeyA', 'RegCloseKey',
    'RegQueryValueExA', 'GetFileVersionInfoSizeA', 'GetFileVersionInfoA', 'VerQueryValueA',
    '_vsnprintf',
    'RajkoD3DCreate9', 'Direct3DCreate9', 'DirectInput8Create',   # native_call.c
    'malloc', 'free', 'calloc', 'realloc', 'memmove',
    '_aligned_malloc', '_aligned_free',
    'InterlockedExchange', 'InterlockedExchangeAdd',
    'InterlockedIncrement', 'InterlockedDecrement',
    'InterlockedCompareExchange',
    'GetModuleFileNameA', 'GetCurrentDirectoryA', 'GetEnvironmentVariableA',
    'GetLastError', 'SetLastError', 'GetCurrentProcess', 'GetCurrentProcessId',
    'GetCurrentThreadId', 'GetTickCount',
    'strncpy', 'fopen', 'fclose', 'fread', 'fwrite', 'fseek', 'ftell', 'feof',
    'SymInitialize', 'SymCleanup', 'SymGetOptions', 'SymSetOptions',
    'CreateToolhelp32Snapshot', 'Module32First', 'Module32Next', 'CloseHandle',
    '_msize', 'SymLoadModule64',
    '_initterm', '__dllonexit', 'DisableThreadLibraryCalls',
    # --- socket layer, bodies in shims_net.c ---
    'WSAStartup', 'WSACleanup', 'WSAGetLastError', 'socket', 'bind', 'closesocket',
    'sendto', 'recvfrom', 'select', 'ioctlsocket', 'getsockname', 'getsockopt',
    'setsockopt', 'gethostbyname', 'inet_addr', 'inet_ntoa', 'htonl', 'htons',
    'ntohl', 'ntohs',
    # --- Win32 layer, bodies in shims_win32.c ---
    'InitializeCriticalSection', 'DeleteCriticalSection', 'EnterCriticalSection', 'LeaveCriticalSection', 'TlsAlloc', 'TlsFree',
    'TlsGetValue', 'TlsSetValue', 'CreateEventA', 'SetEvent', 'WaitForSingleObject',
    'Sleep', 'QueryPerformanceCounter', 'QueryPerformanceFrequency', 'GetSystemTimeAsFileTime', 'GetLocalTime', 'GetFileType',
    'timeGetTime', 'SetUnhandledExceptionFilter', 'LoadLibraryA', 'FreeLibrary', 'GetModuleHandleA', 'GetSystemMetrics',
    'GetDesktopWindow', 'GetForegroundWindow', 'GetDC', 'ReleaseDC', 'GetClientRect', 'GetWindowRect',
    'AdjustWindowRect', 'ClientToScreen', 'ScreenToClient', 'ShowWindow', 'UpdateWindow', 'MoveWindow',
    'SetWindowPos', 'BringWindowToTop', 'SetForegroundWindow', 'SetFocus', 'IsIconic', 'ValidateRect',
    'PostQuitMessage', 'TranslateMessage', 'MessageBoxA', 'GetCursorPos', 'SetCursorPos', 'SetCursor',
    'ShowCursor', 'DestroyCursor', 'ClipCursor', 'GetAsyncKeyState', 'SystemParametersInfoA', 'EnumDisplaySettingsA',
    'DrawTextA', 'GetStockObject', 'SelectObject', 'DeleteObject', 'DeleteDC', 'CreateCompatibleDC',
    'CreateCompatibleBitmap', 'SetBkColor', 'SetBkMode', 'SetTextColor', 'BitBlt', 'GetDIBits',
    'CreateFontA', 'sprintf', '_snprintf', 'printf', 'fprintf',
    # --- CRT layer, bodies in shims_crt.c ---
    'strchr', 'strrchr', 'strstr', 'strpbrk', 'strtok', 'strncat',
    'strncmp', 'strcspn', 'strcoll', 'strerror', '_strdup', '_stricmp',
    '_strnicmp', '_strupr', 'memchr', 'strtod', 'strtoul', 'wcslen',
    'wcscmp', 'wcsncpy', 'wcstoul', '_wcsicmp', '_wcslwr', '_wcsupr',
    'mbstowcs', 'wcstombs', '_itow', '_wtoi', 'isalnum', 'isalpha',
    'iscntrl', 'isdigit', 'islower', 'isprint', 'ispunct', 'isspace',
    'isupper', 'isxdigit', 'tolower', 'toupper', 'acos', 'asin',
    'atan', 'ceil', 'cos', 'exp', 'fabs', 'floor',
    'log', 'log10', 'sin', 'sqrt', 'tan', 'atan2',
    'fmod', 'pow', 'ldexp', 'frexp', '_copysign', '_finite',
    '_isnan', '_fpclass', '_controlfp', '_CIacos', '_CIasin', '_CIcosh',
    '_CIexp', '_CIsinh', '_CItanh', '_CIpow', '_CIfmod', '_ftol',
    'atoi', 'atof', '_ecvt', '_fcvt', 'ferror', 'fflush',
    'fgets', 'fputc', 'fputs', 'getc', 'putc', 'puts',
    'rewind', 'setvbuf', 'ungetc', 'fgetpos', 'fsetpos', 'remove',
    'rename', 'tmpfile', 'tmpnam', '_fsopen', '_wfopen', '_access',
    '_chdir', '_getcwd', '_findclose', '_splitpath', '_get_osfhandle', 'time',
    'clock', 'localtime', 'gmtime', 'mktime', 'difftime', 'strftime',
    '_strdate', '_strtime', '_ftime', 'getenv', 'rand', 'srand',
    'setlocale', '_errno', '_sleep', '_mkdir', '_heapchk', 'exit',
    'abort',
}


# IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE. Classify by these, never by
# section *name*: binkw32.dll puts its decoders in about thirty custom-named
# executable sections (BINK, BINK16, BINK4444, BINK32R, ...), all with
# characteristics 0x60000020. A name test that only accepts ".text" calls every
# one of those exports data, and the generator would then hand the client
# storage where it expects Bink's entry points -- eight silently broken calls.
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_MEM_EXECUTE = 0x20000000


def is_code_rva(info, rva):
    for s in info.sections:
        if s.virtual_address <= rva < s.virtual_address + s.virtual_size:
            ch = getattr(s, 'characteristics', 0) or 0
            return bool(ch & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE))
    return True         # unknown: assume code, which keeps it a normal shim


def data_exports(dll_name):
    """Names this dependency exports from a non-executable section."""
    for sub in SEARCH:
        path = os.path.join(sub, dll_name)
        if os.path.exists(path):
            break
    else:
        return set()
    try:
        info = analyze_pe(path)
    except Exception:                                   # noqa: BLE001
        return set()
    out = set()
    for e in info.exports:
        name = e['name'] if isinstance(e, dict) else getattr(e, 'name', None)
        rva = e['rva'] if isinstance(e, dict) else getattr(e, 'rva', 0)
        if name and not is_code_rva(info, rva):
            out.add(name)
    return out


def c_ident(name, used):
    """A unique, valid C identifier for an export name.

    Export names are not C identifiers. This binary imports both kinds that
    break: MSVC C++ mangling (`?terminate@@YAXXZ`) and stdcall decoration
    (`_BinkOpenDirectSound@4`). Emitting either verbatim produces `unknown
    character '0x40'` by the hundred.

    Every character outside [A-Za-z0-9_] becomes '_', and a collision -- two
    different mangled names can flatten to the same thing -- gets a numeric
    suffix. The real name is kept in the IMPORT_STUB string and the comment, so
    a stub report still prints what was actually called.
    """
    base = ''.join(ch if (ch.isalnum() or ch == '_') else '_' for ch in name)
    if not base or base[0].isdigit():
        base = '_' + base
    ident, n = base, 1
    while ident in used:
        n += 1
        ident = '%s_%d' % (base, n)
    used.add(ident)
    return ident


HEADER = '''/* Auto-generated by gen_imports.py -- DO NOT EDIT.
 *
 * %d imports across %d DLLs.
 *   %d stdcall  (callee pops; count derived from the SDK import libraries)
 *   %d cdecl    (caller pops; nothing to derive)
 *   %d data     (dereferenced, never called -- see gen_imports.py)
 */
#include "imports.h"

'''


def main():
    global DLL, OUT, SEARCH
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dll', default=DLL)
    ap.add_argument('--deps', default=None,
                    help="folder holding the dependency DLLs "
                         "(default: the folder of the target binary)")
    ap.add_argument('-o', '--output', default=OUT)
    args = ap.parse_args()

    DLL, OUT = args.dll, args.output
    SEARCH = (args.deps,) if args.deps else (os.path.dirname(DLL) or '.',
                                             os.path.join(os.path.dirname(DLL), 'p_dlls'))

    info = analyze_pe(DLL)
    iat = build_iat_map(info)
    rows, unresolved = ArgcResolver().resolve_iat(iat)

    # Which imported names are data rather than code, per source DLL.
    dlls = {d for _, d, _, _ in unresolved} | {r[1] for r in rows}
    data_by_dll = {d: data_exports(d) for d in sorted(dlls)}

    stdcall, cdecl, data = [], [], []
    for va, dll, name, real, argc in rows:
        (data if real in data_by_dll.get(dll, ()) else stdcall).append(
            (va, dll, real, argc))
    for va, dll, name, real in unresolved:
        if real in data_by_dll.get(dll, ()):
            data.append((va, dll, real, 0))
        else:
            cdecl.append((va, dll, real, 0))

    # Refuse to generate if a hand-written name is not actually imported, or is
    # classified as data. Both leave the stub in place and look identical to the
    # shim simply not being called.
    imported = {r for _, _, r, _ in stdcall + cdecl}
    as_data = {r for _, _, r, _ in data}
    # Names only one of the two targets imports may be absent from the other.
    bogus = sorted((HAND_WRITTEN - imported - ONE_TARGET_ONLY) | (HAND_WRITTEN & as_data))
    if bogus:
        print('[!] HAND_WRITTEN names this binary does not import as code:',
              file=sys.stderr)
        for b in bogus:
            print('      %s' % b, file=sys.stderr)
        print('    The stub would stay and the hand-written body would never\n'
              '    be called, which looks exactly like the shim not working.',
              file=sys.stderr)
        raise SystemExit(1)

    lines = [HEADER % (len(iat), len(dlls), len(stdcall), len(cdecl), len(data))]

    # One identifier per export name, shared by the shim and the bridge table.
    used, ident = set(), {}
    for va, dll, real, argc in stdcall + cdecl + data:
        if real not in ident:
            ident[real] = c_ident(real, used)

    emitted = set()
    for group, note in ((stdcall, 'stdcall'), (cdecl, 'cdecl')):
        lines.append('/* ---- %s ---- */' % note)
        for va, dll, real, argc in group:
            if real in emitted:
                continue            # one export reached through two IAT slots
            emitted.add(real)
            sym = ident[real]
            if real in HAND_WRITTEN:
                lines.append('extern void imp_%s(void);  /* %s!%s (%d args) '
                             '- real body in shims_impl.c */' % (sym, dll, real, argc))
            else:
                lines.append('/* %s!%s (%d args) */' % (dll, real, argc))
                lines.append('static void imp_%s(void) { IMPORT_STUB("%s"); '
                             'RET(0); STDRET(%d); }'
                             % (sym, real.replace('\\', '\\\\').replace('"', '\\"'),
                                argc))
        lines.append('')

    # Data imports get storage, not shims. A zeroed object is wrong in the sense
    # that the real PyTypeObjects are not zero -- but it is wrong *visibly*: the
    # client reads a null type and reports, instead of reading a shim prologue
    # and continuing with nonsense. When the runtime loads a real pythonMXO.dll
    # it repoints these slots at the genuine exports.
    lines.append('/* ---- data imports: dereferenced, never called ----\n'
                 ' * Storage stands in until the runtime repoints each slot at a\n'
                 ' * real pythonMXO.dll export. Zero is a visibly wrong value,\n'
                 ' * which is the point: a null type object reports, a shim\n'
                 ' * address would be read as a live object.\n'
                 ' */')
    for va, dll, real, _ in data:
        lines.append('uint32_t imp_data_%s[64] = {0};  /* %s!%s */'
                     % (ident[real], dll, real))
    lines.append('')
    lines.append('const recomp_import_data_t mxo_import_data[] = {')
    for va, dll, real, _ in data:
        lines.append('    { 0x%08Xu, imp_data_%s, "%s", "%s" },'
                     % (va, ident[real], dll, real))
    lines.append('};')
    lines.append('const uint32_t mxo_import_data_count = %d;\n' % len(data))

    lines.append('/* IAT slot VA -> shim. Installed before entry. */')
    lines.append('const recomp_dispatch_entry_t mxo_import_bridges[] = {')
    for va, dll, real, _ in stdcall + cdecl:
        lines.append('    { 0x%08Xu, imp_%s },  /* %s!%s */'
                     % (va, ident[real], dll, real))
    lines.append('};')
    lines.append('const uint32_t mxo_import_bridge_count = %d;\n'
                 % len(stdcall + cdecl))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'w').write('\n'.join(lines) + '\n')

    print('imports: %d across %d DLLs' % (len(iat), len(dlls)))
    print('  stdcall %4d  (derived argc)' % len(stdcall))
    print('  cdecl   %4d  (pop 0)' % len(cdecl))
    print('  data    %4d  (storage, not shims)' % len(data))
    by = collections.Counter(d for _, d, _, _ in data)
    for d, n in by.most_common():
        print('      %-18s %d' % (d, n))
    print('-> %s' % OUT)


if __name__ == '__main__':
    main()
