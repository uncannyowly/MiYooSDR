#!/bin/sh
# MiYooSDR launcher (OnionOS, Miyoo Mini Plus).
# Live radio modes stop Wi-Fi management while the app sweeps (so nothing scans
# or retunes the radio), then bring Wi-Fi back the way Onion starts it. Modes
# that do not use the radio (replay, review, --fake, --version) leave Wi-Fi alone.
# Extra arguments go to the binary (e.g. --dump 50 for a headless test).

APPDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APPDIR" || exit 1
LOG="$APPDIR/miyoosdr.log"
RTL="${MIYOOSDR_RTL_PROC:-/proc/net/rtl8188fu/wlan0}"
IW=/mnt/SDCARD/.tmp_update/bin/iw
WPA=/mnt/SDCARD/miyoo/app/wpa_supplicant
unset LD_PRELOAD
export LD_LIBRARY_PATH="/config/lib:/customer/lib:$LD_LIBRARY_PATH"
chmod +x "$APPDIR/miyoosdr" 2>/dev/null

# The binary reads the options and says whether they need the radio (0), do
# not (1), or are wrong (2, with a message), before anything is touched.
"$APPDIR/miyoosdr" --needs-radio "$@"
case $? in
0) ;;
1)
	"$APPDIR/miyoosdr" "$@"
	status=$?
	# The app resumes Onion's menu if it paused it; this covers a crash.
	pkill -CONT MainUI 2>/dev/null
	exit $status
	;;
*) exit 2 ;;
esac

log() { echo "$(date +%T) $*" >> "$LOG"; }

had_wpa=0; pgrep wpa_supplicant >/dev/null 2>&1 && had_wpa=1
was_up=0; ifconfig wlan0 2>/dev/null | grep -q UP && was_up=1

# free the audio device so the app's signal-to-sound can use MI_AO directly
AUDIOSERVER=""
for a in /mnt/SDCARD/.tmp_update/bin/audioserver /customer/app/audioserver; do
	[ -x "$a" ] && AUDIOSERVER="$a" && break
done
had_audio=0; pgrep audioserver >/dev/null 2>&1 && had_audio=1

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
	# give Onion its audio back
	if [ $had_audio = 1 ] && [ -n "$AUDIOSERVER" ] && ! pgrep audioserver >/dev/null 2>&1; then
		"$AUDIOSERVER" >/dev/null 2>&1 &
	fi
	log "restore: done"
	sync
}

: > "$LOG"
log "start $*"
if [ ! -d "$RTL" ]; then
	log "no RTL8188FU wlan0 (Mini Plus only)"
	exit 1
fi
# A signal exits, and the EXIT trap restores once; setup never resumes after it.
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# switch the driver's power saving off while still associated (that is when
# it takes effect); then stop Wi-Fi management so nothing scans or retunes
# while we sweep. The app restores power saving when it exits.
"$APPDIR/miyoosdr" --pm-active >> "$LOG" 2>&1
sleep 0.3
pkill -9 wpa_supplicant 2>/dev/null
pkill -9 udhcpc 2>/dev/null
ifconfig wlan0 up
$IW dev wlan0 disconnect >> "$LOG" 2>&1
echo "1 0 0" > "$RTL/monitor"
cat "$RTL/rf_info" >> "$LOG" 2>&1

# hand the audio device to the app (signal-to-sound). Onion gets it back on exit.
if [ $had_audio = 1 ]; then
	killall -9 audioserver 2>/dev/null
	sleep 0.2
fi

"$APPDIR/miyoosdr" "$@" >> "$LOG" 2>&1
status=$?
log "exit code $status"
exit $status
