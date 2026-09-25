# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: SemVer.
Nothing is tagged yet; everything so far is pre-0.1.0.

## [Unreleased]

### Added
- Final community client (`MxO_7.6005`, base `0x62000000`) as a second target:
  89,340 functions lifted, 0 errors; `InitClientDLL` completes.
- Server track: private fork of `hdneo/mxo-hd` at `sp00nznet/mxo-hd-recomp`; builds on .NET 8.
- P7 socket layer (`src/runtime/shims_net.c`): 20 WSOCK32 functions, loopback redirect, packet log.
- P6 engine-interface mechanism (`src/runtime/engine_iface.c`): synthetic vtables, `MXO_IFACES`, `MXO_WATCH`.
- P5 Win32 layer and host window; P4 bring-up to a completing `InitClientDLL` on retail.
- P0–P3: disc extraction, `.ltp` extraction and Python decompile tooling, IDA + disasm32
  merged catalog, full lift (76,528 functions) and MSVC build.

### Changed
- `ROADMAP.md` moved from `docs/` to the repo root.

### Removed
- `CLAUDE.md` and all binary-derived symbol maps/catalogs in `analysis/` untracked;
  only run statistics remain. Regenerate locally with the pipeline.
