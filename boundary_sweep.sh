#!/usr/bin/env bash
# sweep cursor across the internal boundary and log position, no physical
# mouse or host window placement involved. Run against nested instance
# (HYPRLAND_INSTANCE_SIGNATURE set to it).
OUT="${1:-boundary_sweep.csv}"
echo "x,y" > "$OUT"
for x in $(seq 1200 10 1360); do   # adjust range around your actual boundary x
    wlrctl pointer move 10 0
    READING=$(hyprctl -j cursorpos)
    RX=$(echo "$READING" | jq -r '.x')
    RY=$(echo "$READING" | jq -r '.y')
    echo "$RX,$RY" >> "$OUT"
done
