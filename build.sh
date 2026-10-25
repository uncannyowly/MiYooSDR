#!/bin/sh
# Cross-compiles both Miyoo binaries and packages an SD-card installation zip.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src gta1-miyoo-toolchain make miyoo
file "$ROOT/build/miyoo/miyoosdr"
file "$ROOT/build/miyoo/miyoo-pcap"
python3 "$ROOT/tools/package.py" "${1:-0.4.0}"
