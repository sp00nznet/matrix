#!/bin/sh
# Start a fresh server + client session and wait until the client is in the world.
#   sh tools/play.sh [seconds]      (then drive it with tools/input.ps1, tools/screenshot.ps1)
# The server is restarted every time: a client that was killed leaves its
# character "already in use according to the Object Server supervisor".
SECS=${1:-300}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRV=$ROOT/../mxo-hd-recomp/hds/bin/Debug/net8.0

taskkill //F //IM hds.exe >/dev/null 2>&1
taskkill //F //IM mxo_final.exe >/dev/null 2>&1
docker start mxo-db >/dev/null 2>&1
# Mission scripts: the game's own (decompiled, never committed) plus constants
# read from its bytecode by tools/pyc_constants.py.
export MXO_MISSION_SCRIPTS=$(cygpath -w "$ROOT/work/py_src/Monolith/Missions")
export MXO_MISSION_STUBS=$(cygpath -w "$ROOT/work/mission_stubs")
(cd "$SRV" && MXO_PUBKEY_HASH=MD5 ./hds.exe > "$ROOT/work/server.log" 2>&1 &)
until grep -q "running" "$ROOT/work/server.log" 2>/dev/null; do sleep 2; done

cd "$ROOT/game_final" || exit 1
rm -f pubkey.dat
: > ClientErrors.txt
(MXO_LOGIN=loluser:lolpass MXO_RUN_SECONDS=$SECS ../build/${MXO_EXE:-mxo_final}.exe client.dll --boot $MXO_ARGS \
    > "$ROOT/work/play_run.log" 2>&1 &)
# The PDA is the last HUD layout loaded on entering the world.
t=0
until grep -q "Loading the PDA Layout" ClientErrors.txt 2>/dev/null; do
    sleep 2; t=$((t + 2))
    [ $t -gt 400 ] && { echo "not in world after ${t}s"; exit 1; }
done
sleep 8
echo "in world after ~${t}s"
