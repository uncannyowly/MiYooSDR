# RTL8188FU packet capture investigation

## Verified on the Miyoo Mini Plus, 2026-10-25

The stock module already supports useful Wi-Fi monitor capture through Wireless
Extensions. A replacement driver is not required for the first packet-analysis
milestone. Spectrum capture remains a sequential PSD measurement; the application
also supports offline analysis of the saved PCAP files.

Device identification:

- Kernel: Linux 4.9.84, build #1136, ARMv7, SMP/PREEMPT.
- Driver: `v5.7.4.2_36687.20200421`, built Jan 10 2023.
- Original module: `/mnt/SDCARD/8188fu.ko`.
- Module SHA-256: `f8561a61db97c375a92e4f86d6fa665734c919e59311e2a9955a890d93eae628`.
- Vermagic: `4.9.84 SMP preempt mod_unload ARMv7 thumb2 p2v8`.
- The module is unstripped and has debug information. Its compiler reports GCC
  8.2.1; a recorded build path is `/home/raymanfeng/home3/ssd201/ssd202/linux-4.9.84`.

The local original-module copy and experimental captures are in ignored
`build/research/`; packet contents are not committed to the repository.

## How the stock capture path was found

`iw list` does not advertise monitor mode. However, symbol inspection and ARM
disassembly of the installed module show:

- `rtw_wx_set_mode` handles Wireless Extensions monitor mode (mode 6), assigns
  network-device type 803 (`ARPHRD_IEEE80211_RADIOTAP`), and sets the driver mode.
- `recv_frame_monitor` calls `fill_radiotap_hdr` followed by `rtw_recv_monitor`.
- The vendor `/proc/net/rtl8188fu/wlan0/monitor` status still said `Disable`
  during a successful capture. Its state is not authoritative for this path.

The helper uses `SIOCSIWMODE` and then a receive-only AF_PACKET socket. It
requires device type 803 before opening a capture file. It writes standard
little-endian PCAP with link type 127, retaining the driver's radiotap headers
and frame FCS. Timestamps come from Linux `SO_TIMESTAMPNS`, converted to PCAP
microseconds; these are kernel delivery timestamps, not hardware RF timestamps.

## Measured result

An eight-second test on channel 11 (2462 MHz) produced:

| Check | Result |
| --- | --- |
| Saved received frames | 3,647 |
| First-to-last timestamp span | 8.003792 seconds |
| Beacons | 202 |
| Frames without decoded radiotap/802.11 | 0 |
| Malformed radiotap length/version | 0 |
| Independently checked frame FCS | 3,647 |
| FCS mismatches | 0 |
| Socket drops reported by kernel | 0 |
| Frames with HT MCS metadata | 871 |
| Frames with legacy rate metadata | 2,776 |

The capture contains management, control, and data frames. Wi-Fi automatically
reassociated after the experiment, SSH became available again, device type
returned to 1, and the original driver version remained loaded.

Zero socket drops does not prove that all over-the-air frames were captured.
The driver or hardware can filter or lose frames before the packet socket.
Successful parsing and FCS checks validate frame integrity, not PHY metadata.

## Reproduce

Build with the existing project cross-compilation image:

```sh
docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/src" -w /src gta1-miyoo-toolchain \
  make build/miyoo/miyoo-pcap
```

Copy `build/miyoo/miyoo-pcap` and `tools/monitor-capture.sh` into
`/mnt/SDCARD/App/MiYooSDR/research/` on the device. Make the binary executable.
The capture script is specific to the tested Onion configuration: an already
connected `wlan0`, one `wpa_supplicant`, and one `udhcpc`. It stops those clients
and restarts them using the tested Onion paths. It does not preserve arbitrary
network-client configurations. Do not run it alongside the spectrum application.

Start the script detached on the Miyoo:

```sh
nohup sh /mnt/SDCARD/App/MiYooSDR/research/monitor-capture.sh \
  > /mnt/SDCARD/App/MiYooSDR/research/monitor-capture.log 2>&1 < /dev/null &
```

SSH will disconnect. The capture runs for eight seconds on the current channel,
then restores managed mode and networking. An independent 25-second timer also
attempts recovery if the main capture stalls. It cannot recover a kernel crash;
rebooting loads the unchanged original boot configuration.

After reconnecting, inspect the log and confirm `/sys/class/net/wlan0/type` is
`1`. The script leaves `/tmp/miyoosdr-monitor-restoring` as a guard against
accidentally rerunning before recovery is checked. Once networking is restored
and the experiment has finished, remove that empty directory with `rmdir` before
another run. Do not remove it during an active experiment.

Open `research/stock-monitor.pcap` on the Miyoo through the **SELECT + START**
recording browser, or retrieve it and open it in Wireshark. For a summary that
omits addresses and payloads:

```sh
uv run --with scapy tools/inspect-capture.py build/research/stock-monitor.pcap
```

The capture utility can also be invoked directly on the device:

```sh
./miyoo-pcap wlan0 mode monitor
./miyoo-pcap wlan0 capture 8 output.pcap
./miyoo-pcap wlan0 mode managed
```

Use the detached wrapper over Wi-Fi SSH so recovery is scheduled before changing
the mode. The wrapper stops before touching Wi-Fi if `miyoo-pcap` is missing, and
its restore brings Wi-Fi back even when `wlan0` never left managed mode. There is no channel hopping or transmit/injection operation in this
utility. Returning to managed operation resumes normal Wi-Fi transmissions.

## Playback on the Miyoo

MiYooSDR plays PCAPs alongside spectrum CSV recordings (since 0.5.0). The
recording browser, opened by the **MiYooSDR Replay** app or **SELECT + START**,
lists `.pcap` and `.csv` files from `rec/` and `research/`, relative to the
application directory, newest first. The capture script's default
`research/stock-monitor.pcap` therefore appears without moving the file.

The PCAP view follows capture timestamps, shows packet counts over time and a
rolling frame list, and displays selected-frame addresses, type, length, and
available radiotap rate, signal and channel values. A pauses or resumes; Left /
Right pause and step a packet; L1 / R1 change speed from 0.25× through 8×; Up /
Down seek by a second. B returns to the browser, MENU quits, and START saves a
screenshot. Playback stops at the end; A restarts it.

To start directly in offline review, preserving the active Wi-Fi connection and
skipping the launcher's audio teardown (Onion's menu is paused while the replay
has the screen, and comes back as it was):

```sh
cd /mnt/SDCARD/App/MiYooSDR
./launch.sh --replay research/stock-monitor.pcap
```

The viewer accepts classic PCAP with radiotap link type 127, in either byte order,
with microsecond or nanosecond timestamps and a limit of 100,000 records. It does
not accept PCAPNG. It displays captured frame information without decrypting
protected payloads, reconstructing spectrum or I/Q samples, or transmitting
frames. The driver's metadata limitations below still apply during playback.
The separate capture utility and detached recovery script are unchanged.

For a repeatable host rendering at 2.5 seconds after the first packet (the PC and
the Miyoo produce identical files):

```sh
make host
build/host/miyoosdr --replay build/research/stock-monitor.pcap \
  --replay-at 2.5 --shot packet-review.png
```

## Metadata issues to fix before trusting PHY analysis

Two captured frames report a legacy rate of 46.5 Mb/s, which is not a valid
802.11 legacy rate. The original module contains exactly one byte sequence
`02 04 0b 16 0c 12 18 24 30 48 5d 6c`, at file offset `0xb4664` in the hashed
module above. This matches the legacy radiotap rate table in the older public
driver source: its 48 Mb/s entry is `93` rather than `96` in 500 kb/s units.
That is consistent with the observed 46.5 Mb/s labels. Correct it in matching
source and validate with controlled 48 Mb/s traffic; the stock module has not
been patched.

Signal values range from -90 to 0 in the driver-provided dBm field. Treat 0 as
suspect until PHY-status availability is checked, especially for control frames.
No TSFT field was present in this capture. No CSI or raw I/Q has been demonstrated.
Do not infer decryption of protected payloads from monitor-mode capture.

## Next driver experiments

1. Obtain source matching the installed v5.7.4.2 module and reproduce its build.
   Public comparison trees inspected so far are older v4.3.23.6 and newer
   v5.11.5.4; neither is established as a drop-in source match.
2. Correct the rate table and trace receive descriptor/PHY-status parsing. Export
   only fields actually supplied for each frame; investigate missing receive
   timestamps, valid RSSI, CRC/error handling, and aggregation metadata.
3. If additional descriptor fields are needed, retain the raw RX descriptor and
   PHY-status bytes alongside frame data before the driver discards them.
4. Keep stock boot files intact. A candidate module needs matching configuration,
   symbol ABI, and a detached stock-module restoration procedure. Matching
   vermagic alone does not establish compatibility. Never force-load a module.
5. Investigate ADC/debug-port capture separately, using Realtek manufacturing-test
   routines and controlled RF input. The newer PHYDM source excludes RTL8188F
   from its logic-analyzer support mask; this is evidence of missing support in
   that code, not proof that every undocumented hardware path is absent.

Published Mini Plus kernel sources and `mmp_defconfig` were downloaded and
inspected. Preparing them in the existing application toolchain image stopped
because it lacks a native host GCC. The image's ARM GCC 8.3 did successfully
build the packet utility. No replacement kernel or driver has been built or
loaded during this investigation.

## Sources

- [ESP-SDR capture investigation](https://github.com/ESPARGOS/esp-sdr/blob/main/docs/how-it-works.md): RF test-library analysis, disassembly, and hardware experiments.
- [Older RTL8188FU comparison source](https://github.com/i2som/RTL8188FU), including `core/rtw_recv.c` and the legacy rate table.
- [Newer OpenIPC RTL8188FU tree](https://github.com/OpenIPC/realtek-wlan/tree/d488596918d4e339bc9b5dd3bbeabea1751b97e5): monitor receive path and PHYDM support masks.
- [Miyoo Mini Plus kernel build instructions](https://steward-fu.github.io/website/handheld/miyoo_mini_plus_build_kernel.htm).
- [Onion SSH defaults](https://onionui.github.io/docs/4.2/network/ssh).
