#!/bin/sh
# Cross-compiles build/miyoo/miyoosdr in the same toolchain image as the Scorched/GTA1 ports.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src gta1-miyoo-toolchain make miyoo
file "$ROOT/build/miyoo/miyoosdr"
