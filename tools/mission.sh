#!/bin/sh
# From a fresh in-world session (tools/play.sh), accept mission N of Tyndall's list.
#   sh tools/mission.sh 3      # 1-based row in "Available Missions"
# Row 3/4 are the game's own scripts (Mission185 tutorial, Mission986) when the
# server runs with MXO_MISSION_SCRIPTS. Coordinates: 1050x839 window (screenshot.ps1).
ROW=${1:-3}
I="powershell -NoProfile -File $(dirname "$0")/input.ps1"
for k in 1 2 3; do                      # message of the day + tips
    $I click 522 517 >/dev/null; sleep 1
    $I click 522 525 >/dev/null; sleep 1
done
$I tap N >/dev/null; sleep 3            # Mission window: contacts
$I click 860 420 >/dev/null; sleep 1    # Tyndall
$I click 870 725 >/dev/null; sleep 5    # Call
$I click 788 $((498 + 20 * ROW)) >/dev/null; sleep 3
$I click 868 716 >/dev/null; sleep 5    # Download
$I click 807 727 >/dev/null; sleep 5    # Accept
$I click 807 727 >/dev/null; sleep 4    # (again: the first can land early)
$I click 800 729 >/dev/null; sleep 1    # Close the mission window
