# Running a session

Everything here assumes the build from the README's Getting started, the client in
`game_final/`, and the server fork built at `../mxo-hd-recomp`.

## Session scripts (`tools/`)

| Script | What it does |
|---|---|
| `play.sh [secs]` | Kills any old server and client, starts MySQL (`docker start mxo-db`), the server and the client, and waits until the client has entered the world (about 20 s). The server is restarted every time: a killed client leaves its character "already in use". |
| `mission.sh N` | From a fresh session: dismisses the start-up tips, opens the phone, calls Tyndall and accepts row N of his list. Rows 3 and 4 are the game's scripted missions (185, 986). |
| `say.sh "text"` | Types one chat line. Chat starting with `?` is a server command. |
| `input.ps1` | `click X Y`, `tap KEY`, `key KEY ms`, `type "text"` into the client window (window coordinates, 1050×839). |
| `screenshot.ps1 out.png` | Captures the client window (DPI-aware). |

`play.sh` also honours `MXO_EXE` (e.g. `mxo_final_trace`) and `MXO_ARGS` (extra host arguments).

## Host environment variables

| Variable | Effect |
|---|---|
| `MXO_LOGIN=user:pass` | Log in through the client's own login code before `InitClientDLL` |
| `MXO_RUN_SECONDS=N` | Exit after N seconds |
| `MXO_SERVER`, `MXO_PACKET_LOG`, `MXO_PACKET_BYTES` | Server address, and the datagram log (`work/packets.log`) |
| `MXO_WATCHDOG` | Report where the client is if the frame loop stalls |
| `MXO_COUNT_VA=va,...` | Count indirect calls to these addresses, printed at exit |
| `MXO_NEWCALLS=file` | Log each function the first time it is called indirectly, with a timestamp |
| `MXO_WATCH=va,...` / `@va` | Dump guest memory at phase boundaries (`@` dereferences first) |
| `MXO_DUMP_ESI=N` | Dump N words of the object in `esi` on a fault |

Traced build only (`tools\build.bat final trace`): `--firsthit LO HI` (first entry to every
function in a range, timestamped), `--calltrace FILE`, `--argtrace VA`, `--watch`.

**Finding the code an event reaches:** run the traced build with `--firsthit`, note the
time, trigger the event (send a packet with `?m rpc`, press a key), and read the functions
first entered after that time.

## Server environment variables

| Variable | Effect |
|---|---|
| `MXO_PUBKEY_HASH=MD5` | Sign the server key with MD5, as the final client expects |
| `MXO_MISSION_SCRIPTS` | The decompiled `Missions/` directory from your `Monolith.ltp` |
| `MXO_MISSION_STUBS` | Constants read from the string and FX tables (below) |
| `MXO_VERBOSE` | More logging |

## Mission script inputs

The mission scripts are the game's own code and are never committed. To produce them from
your copy of the game:

```
python tools/ltp_extract.py game_final/resource/Python/Monolith.ltp game_final/resource/string_tables/Strings.ltp -o work/py
python tools/decompile_ltp.py work/py/Monolith -o work/py_src/Monolith --skip-slow
py -3.13 tools/pyc_constants.py work/py/Strings/stringtable_client.pyc -o work/mission_stubs/stringtable_client.py
py -3.13 tools/pyc_constants.py work/py/Monolith/ltfxmap.pyc -o work/mission_stubs/ltfxmap.py
```

## Diagnostics after a run

- `game_final/ClientErrors.txt`: the client's own log (set `LogToFile = 8` in `autoexec.cfg`).
- `work/play_run.log`: host output, including fault dumps with the lifted address.
- `work/server.log`: server output; `[missions]` lines are the mission host.
- Server `UnknownRPC.txt` and `[rpc] unknown` lines: client messages the server doesn't handle.
