#!/usr/bin/env bash
# nm import check (CI, SPEC §10-adjacent; see docs/CI.md).
#
# hyprtail.so is never linked against a "libHyprland.so": Hyprland is a
# standalone executable that exports its own symbols dynamically, and a
# plugin's undefined references to them (CMonitor::, CPointerManager::, ...)
# are only resolved at `dlopen()` time, once the plugin is loaded into a
# running Hyprland process. A header-based compile does NOT guarantee the
# binary a user's Hyprland actually is still exports a symbol with the same
# mangled name: inlining, a visibility change, deduplication, or a genuine
# rename can all pass compilation and only break at load, silently, the
# first time `hyprctl plugin load` runs. This script catches that class of
# break in CI, against the exact Hyprland binary the row just built.
#
# It does NOT re-derive an allowlist of "known safe" external libraries
# (pixman, cairo, freetype, GL, wayland, ...): the plugin's own Makefile
# links it with no explicit -l flags at all (LDFLAGS is empty by default),
# so hyprtail.so's dynamic symbol table carries no NEEDED entries beyond the
# implicit C++ runtime ones. Every non-libc/libstdc++ symbol it calls is
# resolved, at dlopen time, against the global scope of whatever process
# loaded it -- i.e. Hyprland's own binary plus everything *it* dynamically
# links (pixman, cairo, freetype, wayland, GL, libdrm, ...). So "available"
# below is: Hyprland's own exported dynamic symbols, plus the exported
# dynamic symbols of every shared library Hyprland itself needs.
#
# Usage: check-imports.sh <plugin.so> [hyprland-binary]
#
# <hyprland-binary> defaults to /usr/bin/Hyprland. CI runs it against Arch's
# packaged (LTO) /usr/bin/Hyprland (scripts/ci/arch.sh) and, in the upstream
# Nix rows, against the row's non-LTO .Hyprland-wrapped ELF (bin/Hyprland is
# a wrapper script there). LTO can inline or dedupe a symbol a non-LTO build
# still exports, so only a distro binary is ground truth. The host is
# CachyOS, a different LTO build from Arch's: after a host Hyprland upgrade,
# run this with just <plugin.so> against the real /usr/bin/Hyprland. See
# docs/CI.md "Arch jobs and the LTO gap".

set -euo pipefail

PLUGIN="${1:?usage: check-imports.sh <plugin.so> [hyprland-binary]}"
HYPRLAND_BIN="${2:-/usr/bin/Hyprland}"

for bin in nm ldd; do
    command -v "$bin" >/dev/null || { echo "error: $bin not found" >&2; exit 1; }
done

[ -f "$PLUGIN" ] || { echo "error: plugin not found: $PLUGIN" >&2; exit 1; }
[ -f "$HYPRLAND_BIN" ] || { echo "error: Hyprland binary not found: $HYPRLAND_BIN" >&2; exit 1; }

strip_version() { sed 's/@.*//'; }

# Strictly undefined symbols only ($1 == "U"): weak undefined ("w"/"v")
# symbols are optional by ELF rules and resolve to 0 if absent, so they are
# not a real dependency and would be false positives here.
needed_symbols() {
    nm -D --undefined-only "$1" 2>/dev/null | awk '$1 == "U" { print $NF }' | strip_version
}

defined_symbols() {
    nm -D --defined-only "$1" 2>/dev/null | awk '{ print $NF }' | strip_version
}

available_tmp="$(mktemp)"
trap 'rm -f "$available_tmp"' EXIT

defined_symbols "$HYPRLAND_BIN" >>"$available_tmp"

# Every shared library Hyprland itself needs (pixman, cairo, freetype,
# wayland-client, libGL, libdrm, ...) contributes to the same runtime global
# scope the plugin resolves against.
while read -r lib; do
    [ -n "$lib" ] || continue
    [ -f "$lib" ] || continue
    defined_symbols "$lib"
done < <(ldd "$HYPRLAND_BIN" | awk '{ print $3 }') >>"$available_tmp"

sort -u -o "$available_tmp" "$available_tmp"

missing="$(comm -23 <(needed_symbols "$PLUGIN" | sort -u) "$available_tmp")"

if [ -n "$missing" ]; then
    echo "error: hyprtail.so needs symbols this Hyprland build does not export:" >&2
    if command -v c++filt >/dev/null; then
        echo "$missing" | c++filt >&2
    else
        echo "$missing" >&2
    fi
    exit 1
fi

echo "nm import check: ok, all undefined symbols resolve against $HYPRLAND_BIN"
