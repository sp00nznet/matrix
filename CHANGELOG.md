# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: SemVer.
Nothing is tagged yet; everything so far is pre-0.1.0.

## [Unreleased]

### Added
- Guest threads as fibers (`src/runtime/guest_threads.c`): `_beginthreadex`, `CreateThread`,
  cooperative `Sleep`/`WaitForSingleObject`/`select`, TLS on FLS, `RaiseException` (SetThreadName).
- Host passes `InitClientDLL` its real eight arguments (recovered from `launcher.exe`).
- MSLU binding (`src/runtime/shims_mslu.c`): the client's own window, via 36 runtime-resolved
  W-functions, and a host thunk for its window procedure.
- Native passthrough (`src/runtime/native_call.c`): real host COM runs on the simulated stack,
  arity measured from the callee's own `ret`. Direct3D 9 and DirectInput 8 initialise for real.
- Shims: `GetSystemInfo`, `IsProcessorFeaturePresent`, registry reads, version resources,
  `_vsnprintf`, MSLU `GetProcAddress`.
- `docs/HOST.md`: the launcher contract and the runtime findings behind this round.
- Final community client (`MxO_7.6005`, base `0x62000000`) as a second target:
  89,340 functions lifted, 0 errors; `InitClientDLL` completes.
- Server track: private fork of `hdneo/mxo-hd` at `sp00nznet/mxo-hd-recomp`; builds on .NET 8.
- P7 socket layer (`src/runtime/shims_net.c`): 20 WSOCK32 functions, loopback redirect, packet log.
- P6 engine-interface mechanism (`src/runtime/engine_iface.c`): synthetic vtables, `MXO_IFACES`, `MXO_WATCH`.
- P5 Win32 layer and host window; P4 bring-up to a completing `InitClientDLL` on retail.
- P0–P3: disc extraction, `.ltp` extraction and Python decompile tooling, IDA + disasm32
  merged catalog, full lift (76,528 functions) and MSVC build.

### Changed
- Time CRT shims use the 32-bit `time_t` forms MSVCR71 expects.
- Counted `printf` forms return -1 on truncation, as MSVCR71 does.
- Host stdout is unbuffered and UCRT invalid-parameter fail-fasts are reported.
- `ROADMAP.md` moved from `docs/` to the repo root.

### Fixed
- `InitClientDLL` returning 0 was misread as success; it is failure. README corrected.
- Guest re-entry during a native call no longer overwrites host or guest frames.

### Removed
- `CLAUDE.md` and all binary-derived symbol maps/catalogs in `analysis/` untracked;
  only run statistics remain. Regenerate locally with the pipeline.
