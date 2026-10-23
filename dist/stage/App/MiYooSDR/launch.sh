#!/bin/sh
# MiYooSDR spike launcher (OnionOS, Miyoo Mini Plus).
# Stops Wi-Fi management while the app sweeps (so nothing scans or retunes the
# radio), then brings Wi-Fi back the way Onion starts it.
# Extra arguments go to the binary (e.g. --dump 50 for a headless test).

APPDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APPDIR" || exit 1
LOG="$APPDIR/miyoosdr.log"
IW=/mnt/SDCARD/.tmp_update/bin/iw
WPA=/mnt/SDCARD/miyoo/app/wpa_supplicant
unset LD_PRELOAD
export LD_LIBRARY_PATH="/config/lib:/customer/lib:$LD_LIBRARY_PATH"

log() { echo "$(date +%T) $*" >> "$LOG"; }

had_wpa=0; pgrep wpa_supplicant >/dev/null 2>&1 && had_wpa=1
was_up=0; ifconfig wlan0 2>/dev/null | grep -q UP && was_up=1

restore() {
	trap - EXIT INT TERM HUP
	log "restore: Wi-Fi (was_up=$was_up had_wpa=$had_wpa)"
	if [ $was_up = 0 ] && [ $had_wpa = 0 ]; then
		ifconfig wlan0 down
	fi
	if [ $had_wpa = 1 ]; then
		$WPA -B -D nl80211 -iwlan0 -c /appconfigs/wpa_supplicant.conf >> "$LOG" 2>&1
		udhcpc -i wlan0 -s /etc/init.d/udhcpc.script > /dev/null 2>&1 &
		$IW dev wlan0 set power_save off > /dev/null 2>&1
	fi
	log "restore: done"
	sync
}

: > "$LOG"
log "start $*"
if [ ! -d /proc/net/rtl8188fu/wlan0 ]; then
	log "no RTL8188FU wlan0 (Mini Plus only)"
	exit 1
fi
trap restore EXIT INT TERM HUP

# switch the driver's power saving off while still associated (that is when
# it takes effect); then stop Wi-Fi management so nothing scans or retunes
# while we sweep. The app restores power saving when it exits.
chmod +x "$APPDIR/miyoosdr"
"$APPDIR/miyoosdr" --pm-active >> "$LOG" 2>&1
sleep 0.3
pkill -9 wpa_supplicant 2>/dev/null
pkill -9 udhcpc 2>/dev/null
ifconfig wlan0 up
$IW dev wlan0 disconnect >> "$LOG" 2>&1
echo "1 0 0" > /proc/net/rtl8188fu/wlan0/monitor
cat /proc/net/rtl8188fu/wlan0/rf_info >> "$LOG" 2>&1

chmod +x "$APPDIR/miyoosdr"
"$APPDIR/miyoosdr" "$@" >> "$LOG" 2>&1
log "exit code $?"
