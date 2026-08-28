#!/usr/bin/env sh
# One-shot bootstrap for a fresh clone:
#   1. initialize submodules (krkrz etc.)
#   2. download + verify + extract the game data from the data source release
#      (the location is recorded in data-source.json)
set -eu

cd "$(dirname "$0")"

git submodule sync --recursive
git submodule update --init --recursive
python tools/fetch_data_parts.py --dest data

echo "setup complete: submodules ready, game data in data/"
