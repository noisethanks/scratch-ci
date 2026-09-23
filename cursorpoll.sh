#!/usr/bin/env bash
# cursorpos_trace.sh: logs global cursor position with real timestamps.
# Timestamped per-sample on purpose, see note above, don't trust nominal
# 50ms spacing when checking for a discontinuity, trust the logged t_ms.
OUT="${1:-cursorpos_trace.csv}"
echo "t_ms,x,y" > "$OUT"
START=$(date +%s%N)

while true; do
    NOW=$(date +%s%N)
    T_MS=$(( (NOW - START) / 1000000 ))
    READING=$(hyprctl -j cursorpos)
    X=$(echo "$READING" | jq -r '.x')
    Y=$(echo "$READING" | jq -r '.y')
    echo "$T_MS,$X,$Y" >> "$OUT"
    sleep 0.05
done
