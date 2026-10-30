#!/bin/sh
# Type one line into the client's chat and send it:  sh tools/say.sh "?m ok"
I="powershell -NoProfile -File $(dirname "$0")/input.ps1"
$I tap Return >/dev/null; sleep 1
$I type "$1" >/dev/null; sleep 1
$I tap Return >/dev/null
