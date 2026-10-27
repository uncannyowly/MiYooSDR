#!/bin/sh
# Opens MiYooSDR's recording browser without live mode, so Wi-Fi stays connected.
cd "$(dirname "$0")/../MiYooSDR" || exit 1
exec sh ./launch.sh --review
