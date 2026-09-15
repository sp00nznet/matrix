# Finding `matrix.exe` — resolved: there is no such binary

P6 is blocked on a binary the retail discs never carried. `launcher.exe` names
it — `matrix.exe`, the LithTech engine host — and downloads it from
`update.beta.thematrixonline.com`, a host that has been dead since 2009.

> **Outcome: `matrix.exe` does not exist as a shipped file.** A full post-patch
> client (`MxO_7.6005`, 2,666 files) does not contain it either, and no binary
> in it besides `client.dll` carries the engine interface names. The engine
> interfaces are registered *inside* `client.dll`; see the correction at the end
> of `ROADMAP.md`. The rest of this page is the search as it was conducted, kept
> because ruling the retail media out was necessary to get there.

## What archive.org has (checked)

| Item | Size | What it actually is |
|---|---:|---|
| `the_matrix_online` | 2.1 GB | `Disk1/2/3.bin`+`.cue` — **the same retail 3-CD set we already have** |
| `matrix-pc-redump` | 21.4 GB | Mostly *Enter the Matrix* (a different game); its three MxO discs are the same retail set |
| `the-matrix-online-2005-sega-wb-monolith-3-CD` | 2.6 GB | Again the 3-CD retail set |

All three are the retail media. **None contains `matrix.exe`**, which is
consistent with what `launcher.exe` says: the engine was never pressed to disc.

Searches for the emulator-community client distributions (`mxoemu`,
`hardline dreams`, `mxo client`, `matrix online emulator`) return nothing
relevant on archive.org. Those projects distribute their client from their own
sites rather than through the Archive.

## The live lead: the Wayback Machine has the patcher's own manifest

The CDX index has captures of the patch host:

```
20140503044936  http://update.beta.thematrixonline.com:80/patchctl/recoverycf.zcf  200  10127
20190524231920  http://update.beta.thematrixonline.com/                            200   1143
```

`recoverycf.zcf` is the interesting one. Our install already contains
`_patchcf.prev.zcf`, so `.zcf` is the patcher's **control/manifest format** —
it should name the files the patcher fetched and where from, `matrix.exe`
included, which would tell us both the exact filename and whether any of it is
still reachable in the Wayback Machine.

**Not yet retrieved:** Internet Archive services were temporarily offline during
this attempt and returned their status page instead of the file. Retry:

```bash
curl -L "http://web.archive.org/web/20140503044936if_/http://update.beta.thematrixonline.com/patchctl/recoverycf.zcf" -o recoverycf.zcf
```

`patch.thematrixonline.com` returns 410 Gone and has no useful captures.

## Ranked options

1. **The Wayback manifest** (above). Cheapest, and it may point at more captures.
   Blocked only on the Archive coming back.
2. **The emulator community distributions.** `mxoemu.info` and
   `mxo.hardlinedreams.com` both distribute a working client so people can
   connect to their servers — that client is post-patch and includes the engine.
   This is the most likely actual source.
3. **Any "installed/patched" copy** rather than disc images. The distinguishing
   test is simply whether `matrix.exe` is present.

## Why this matters beyond P6

Whatever copy we obtain also settles the roadmap's largest open risk. These
discs are February 2005 and pre-launch — the update host is literally
`update.beta.` — while the emulators target the final 2009 client. Sourcing a
patched client makes our target the same one they target, which retires the
version question and unblocks P6 in a single step.

The retail discs remain the right starting point and are not wasted: the lift,
the Python rules, the 28-interface list and the protocol surface all came from
them, and none of it needed `matrix.exe`.
