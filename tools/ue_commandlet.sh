#!/bin/zsh
# Runs one UE Python commandlet: niced, with a wall-clock limit, log to $LOG.
#   tools/ue_commandlet.sh <script.py> [extra UE args...]
# Env: LOG (default /tmp/ue_commandlet.log), LIMIT seconds (default 900),
#      UE (default the 5.8 install), PROJECT (default AncientSettlement).
set -u
SCRIPT="$1"; shift
UE="${UE:-/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd}"
PROJECT="${PROJECT:-$HOME/Documents/UProjects/AncientSettlement/AncientSettlement.uproject}"
LOG="${LOG:-/tmp/ue_commandlet.log}"
LIMIT="${LIMIT:-900}"
nice -n 15 "$UE" "$PROJECT" -run=pythonscript -script="$SCRIPT" "$@" \
    -unattended -nosplash -NoSound -nullrhi -stdout > "$LOG" 2>&1 &
PID=$!
( sleep "$LIMIT"; kill -TERM "$PID" 2>/dev/null && echo "killed after ${LIMIT}s" >> "$LOG" ) &
WATCH=$!
wait "$PID"
CODE=$?
kill "$WATCH" 2>/dev/null
echo "exit=$CODE" >> "$LOG"
exit $CODE

