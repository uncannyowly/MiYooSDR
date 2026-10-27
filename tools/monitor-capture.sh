#!/bin/sh
# Run detached from SSH: capture briefly, then restore the established Onion
# wlan0 + wpa_supplicant setup. Does not replace modules or modify boot files.
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TOOL="$DIR/miyoo-pcap"
WPA=${MIYOO_WPA:-/mnt/SDCARD/miyoo/app/wpa_supplicant}
NET=${MIYOO_NET:-/sys/class/net/wlan0}
LOCK=${MIYOO_LOCK:-/tmp/miyoosdr-monitor-restoring}
restore() {
    # An occupied guard may mean the watchdog is currently restoring Wi-Fi.
    # Do not report success or let normal cleanup kill that recovery process.
    mkdir "$LOCK" 2>/dev/null || return 1
    echo "Restoring managed Wi-Fi"
    # Type 1 means wlan0 never left managed mode (803 is monitor mode), so a
    # failing mode switch must not keep Wi-Fi down.
    if { [ "$(cat "$NET/type" 2>/dev/null)" != 1 ] && ! "$TOOL" wlan0 mode managed; } ||
        ! ifconfig wlan0 up; then
        echo "Managed-mode recovery failed; leaving the timer available to retry" >&2
        rmdir "$LOCK"
        return 1
    fi
    if ! pgrep wpa_supplicant >/dev/null; then
        if ! "$WPA" -B -D nl80211 -iwlan0 -c /appconfigs/wpa_supplicant.conf; then
            echo "Wi-Fi client restart failed; leaving the timer available to retry" >&2
            rmdir "$LOCK"
            return 1
        fi
    fi
    if ! pgrep udhcpc >/dev/null; then
        udhcpc -i wlan0 -s /etc/init.d/udhcpc.script >/dev/null 2>&1 &
    fi
    echo "Recovery commands issued; association and DHCP may take a few seconds"
}
if [ "${1:-}" = --restore ]; then restore; exit; fi
# Separate checks: under set -e a failing test left of && would not stop the script.
[ -x "$TOOL" ] || { echo "missing $TOOL" >&2; exit 1; }
[ -x "$WPA" ] || { echo "missing $WPA" >&2; exit 1; }
# This experiment is intentionally limited to an already-connected wlan0.
pgrep wpa_supplicant >/dev/null
pgrep udhcpc >/dev/null
[ "$(cat "$NET/type")" = 1 ]
[ ! -e "$LOCK" ] || { echo "Recovery lock exists; inspect the previous run first"; exit 1; }
# Independent recovery still runs if the capture process stalls. The lock keeps
# the watchdog and normal cleanup from starting duplicate network clients.
(sleep 25; sh "$0" --restore) &
watchdog=$!
cleanup() {
    result=$?
    trap - EXIT INT TERM HUP
    if restore; then
        kill "$watchdog" 2>/dev/null || true
    else
        echo "Recovery not confirmed here; leaving the independent timer running" >&2
        [ "$result" -ne 0 ] || result=1
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
killall wpa_supplicant
killall udhcpc
sleep 1
"$TOOL" wlan0 mode monitor
cat /proc/net/rtl8188fu/wlan0/monitor
cat /sys/class/net/wlan0/type
"$TOOL" wlan0 capture 8 "$DIR/stock-monitor.pcap"
