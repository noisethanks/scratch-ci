#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HYPRLAND="$SCRIPT_DIR/../external/Hyprland/build/Hyprland"
CONFIG="$SCRIPT_DIR/hypr/hyprlandd.lua"
W=720  # must match mode width in hyprlandd.lua

# Snapshot addresses of windows that exist before launch
mapfile -t before_arr < <(hyprctl -j clients 2>/dev/null | jq -r '.[].address')
declare -A before_set
for a in "${before_arr[@]}"; do before_set["$a"]=1; done

"$HYPRLAND" -c "$CONFIG" &
NESTED_PID=$!

# Wait for 2 new windows, then move them side-by-side
(
    new=()
    for _ in $(seq 1 40); do
        sleep 0.5
        mapfile -t after < <(hyprctl -j clients 2>/dev/null | jq -r '.[].address')
        new=()
        for a in "${after[@]}"; do
            [[ -z "${before_set[$a]+x}" ]] && new+=("$a")
        done
        [[ "${#new[@]}" -ge 2 ]] && break
    done

    if [[ "${#new[@]}" -lt 2 ]]; then
        echo "launch.sh: timed out waiting for 2 nested output windows" >&2
        exit 1
    fi

    x=0
    for addr in "${new[@]}"; do
        hyprctl dispatch movewindowpixel "exact $x 0,address:$addr"
        x=$((x + W))
    done
) &

wait "$NESTED_PID"
