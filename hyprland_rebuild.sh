#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/external/Hyprland"

# Deliberately not `git pull`. No ABI stability across commits (see NOTES.md),
# build against whatever commit is currently checked out. To advance the pin:
# git checkout <new sha>, rebuild, re-run the validation ladder, then update
# the pin recorded in NOTES.md.
git submodule update --init --recursive

make clear   # also clears generated protocol headers outside build/, rm -rf build alone won't
make debug   # -DTESTS=true, so hyprtester is available on demand via `make test`

echo "Run with: ./build/Hyprland"
