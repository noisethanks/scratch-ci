#!/usr/bin/env bash
# Build and test hyprtail against the Hyprland installed from Arch's
# packages, the way hyprpm and Arch/CachyOS users get it: `make all` in the
# default pkg-config mode against /usr/include/hyprland, then the host-free
# tests, then the nm import check against the real /usr/bin/Hyprland. That
# binary is the distro's LTO build, the kind users run, so the check closes
# the gap a non-LTO Nix build leaves (docs/CI.md "Arch jobs and the LTO gap").
#
# Expects an Arch system with base-devel, hyprland and glslang installed.
# The caller decides which Hyprland that is: ci.yml pins an Arch Linux
# Archive snapshot, upstream.yml uses the live repos. Never runs Hyprland.
#
# Usage: scripts/ci/arch.sh   (from anywhere; works from the repo root)

set -euo pipefail
cd "$(dirname "$0")/../.."

version="$(pacman -Q hyprland)"
echo "Hyprland package: ${version}"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    echo "Hyprland package: \`${version}\`" >>"$GITHUB_STEP_SUMMARY"
fi

make all

# hyprpm loads whatever hyprpm.toml's `output` names, so it must be what
# `make all` produced.
output="$(sed -n 's/^output *= *"\(.*\)"/\1/p' hyprpm.toml)"
if [ -z "$output" ] || [ ! -f "$output" ]; then
    echo "error: hyprpm.toml output '${output}' was not produced by 'make all'" >&2
    exit 1
fi

make test-unit SANITIZE=1
make test-compat
scripts/ci/check-imports.sh "$output" /usr/bin/Hyprland
