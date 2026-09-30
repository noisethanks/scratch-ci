#!/usr/bin/env bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "$SCRIPT_DIR"
echo "📦 Setting up dev environment..."

echo "🔨 Building plugin (DEV=1, against external/Hyprland)..."
make -C "$SCRIPT_DIR" DEV=1 all || { echo "❌ Plugin build failed"; exit 1; }

echo "🚀 Launching Hyprland nested instance..."
HYPRLAND_BIN="$SCRIPT_DIR/external/Hyprland/build/Hyprland"

if [[ ! -x "$HYPRLAND_BIN" ]]; then
    echo "❌ Hyprland binary not found at $HYPRLAND_BIN"
    exit 1
fi

export XDG_CONFIG_HOME="$SCRIPT_DIR/dev_env"
"$HYPRLAND_BIN" -c "$SCRIPT_DIR/dev_env/hypr/hyprlandd.lua" &
HYPRLAND_PID=$!

echo "⏳ Waiting for Hyprland instance to initialize..."
sleep 3

LOGDIR=$(ls -td "$XDG_RUNTIME_DIR/hypr"/*/ 2>/dev/null | head -n1 || true)

if [[ -z "${LOGDIR}" ]]; then
    echo "⚠️  Log directory not found in $XDG_RUNTIME_DIR/hypr/"
    echo "Checking alternative locations..."
    LOGDIR=$(find /tmp -type d -name "hypr" -user "$USER" 2>/dev/null | head -n1)
    if [[ -n "$LOGDIR" ]]; then
        LOGDIR=$(ls -td "$LOGDIR"/*/ 2>/dev/null | head -n1 || true)
    fi
fi

if [[ -n "${LOGDIR}" ]]; then
    export HYPRLAND_INSTANCE_SIGNATURE="$(basename "$LOGDIR")"

    # Try both hyprlandd.log (debug) and hyprland.log
    if [[ -f "$LOGDIR/hyprlandd.log" ]]; then
        LOGFILE="$LOGDIR/hyprlandd.log"
    else
        LOGFILE="$LOGDIR/hyprland.log"
    fi

    LINK="$SCRIPT_DIR/dev_env/hyprtail-latest.log"

    ln -sf "$LOGFILE" "$LINK"
    echo "✅ Linked latest log → $LINK"
    echo "   Log file: $LOGFILE"
else
    echo "⚠️  Could not find Hyprland log directory"
    echo "   Hyprland is running with PID $HYPRLAND_PID"
    echo "   You can manually check logs with: journalctl --user -b 0 | grep -i hyprtail"
fi

echo "🎉 Hyprtail dev session started!"
