# MiYooSDR

A live **2.4 GHz spectrum analyzer and waterfall** for the **Miyoo Mini Plus**,
built from the handheld's own Wi-Fi chip. No SDR dongle, no extra hardware.

The Mini Plus has a Realtek **RTL8188FU** Wi-Fi chip. Buried in its baseband is a
power-spectral-density (PSD) engine meant for factory testing: it measures how
much RF energy sits at a chosen frequency. MiYooSDR drives that engine through the
vendor driver's `/proc` interface, sweeps it across the band, and draws a live
spectrum and a scrolling waterfall on the screen. You can watch Wi-Fi, Bluetooth
and other 2.4 GHz traffic around you, zoom in, and record what you see.

> **Experimental.** This began as a spike to find out whether the idea worked, and
> the code still reads like one. It is a spectrum viewer, not a polished app.
> It is **receive-only**: it only measures energy, never transmits, and it cannot
> demodulate or decode any signal. It restores Wi-Fi when it exits.

Besides the live spectrum and waterfall it also has a **channel occupancy** view
(per Wi-Fi-channel activity bars, to find a clear channel), **signal-to-sound**
(a tone that rises as you move the cursor onto a signal, handy for direction
finding), and a **review** mode that replays saved recordings on the handheld.
It names what uses the frequency under the cursor (Bluetooth LE, Wi-Fi, Zigbee),
counts the **bursts** there (how often a remote or sensor transmits, and for how
long), and remembers your view between launches.

![MiYooSDR showing a 2.4 GHz spectrum and waterfall](docs/screenshot.png)

## Requirements

- A **Miyoo Mini Plus** (the plain Mini, v1 to v4, has no Wi-Fi chip and will not work).
- **OnionOS** with its stock `8188fu` Wi-Fi driver (the one MiYooSDR talks to).

## Install

1. Download `miyoosdr-<version>.zip` from [Releases](../../releases).
2. Extract it to the root of your SD card. It drops in as `App/MiYooSDR`.
3. On the Mini Plus, open it from Onion's **Apps** list.

While it runs, Wi-Fi is taken offline so nothing retunes the radio mid-sweep.
When you quit, the launcher brings Wi-Fi back the way Onion starts it (this takes
a few seconds).

## Controls

| Button | Action |
| --- | --- |
| R2 / L2 | Zoom in / out (full band, then 20, 10, 5 MHz around the cursor) |
| D-pad Left / Right | Move the cursor (pans the view at the edges when zoomed) |
| SELECT (tap) | Jump the cursor to the strongest peak |
| SELECT + Y | Cycle the spectrum view: line, filled, filled with Wi-Fi band shading |
| SELECT + A | Cycle the colour palette (Ember, Spectrum, Ice, Matrix) |
| SELECT + B | Switch the lower panel between waterfall and channel occupancy |
| SELECT + X | Signal-to-sound: a tone whose pitch follows the level under the cursor |
| SELECT + START | Open the recording browser to replay a saved capture |
| Y | Enhance: show dB above the slow background instead of absolute level |
| A | Show or hide the max-hold trace |
| B | Clear the hold, average, waterfall and burst count |
| X | Start / stop recording to `App/MiYooSDR/rec/*.csv` |
| L1 / R1 | Gain down / up |
| D-pad Up / Down | Shift the level range |
| START | Save a screenshot (`App/MiYooSDR/shots/sdr-<date>-<time>.png`) |
| MENU | Quit |

Levels are **uncalibrated** relative dB. The full-band view covers roughly
2402 to 2482 MHz in 256 bins; zoomed views use larger on-chip FFTs on a single
channel, down to about 39 kHz per bin.

## Reading the screen

- **Right of the spectrum:** the cursor frequency, its level now, its max hold and
  the background, then what uses that frequency: the Bluetooth LE channel (`ADV 37`,
  `ADV 38` and `ADV 39` in yellow are the advertising channels that beacons, phones
  and most BLE remotes use), the Wi-Fi channel and the Zigbee / 802.15.4 channel.
  Yellow ticks on the spectrum's bottom edge mark the three advertising channels.
- **Right of the waterfall:** **BURSTS** counts transmissions at the cursor: the
  level rising 8 dB over the background and falling back. Below it are the time
  since the last burst, the typical spacing between bursts and the last one's
  length. Park the cursor on a signal (SELECT jumps to the strongest) and zoom in
  for finer timing: a full-band sweep takes about 0.2 s, a zoomed one 0.05 to
  0.15 s. B clears the count, and moving the cursor starts a new one.
- **Remembered:** palette, view style, lower panel, enhance, max hold, zoom and the
  cursor are saved to `App/MiYooSDR/settings.txt` when you quit and restored on the
  next start. Delete the file to start fresh.

## Recording and raw samples

Pressing **X** records every sweep to `App/MiYooSDR/rec/*.csv` (seconds, gain,
then dB per bin). Zoom to one channel first and it records just that slice.

For a controlled high-rate capture there is a headless mode:

```sh
App/MiYooSDR/miyoosdr --burst <MHz> <seconds>   # park on one channel, log power vs time
```

It stays on a single channel and records all 128 bins at about 25 sweeps per
second. Each bin is revisited roughly every 40 ms; short transmissions can be
missed, and these measurements do not resolve individual packet durations.

**There is no demonstrated raw I/Q capture path.** MiYooSDR records power against
frequency and time, not a demodulatable sample stream. Its experimental `--racap`
probe did not establish a working ADC/logic-analyzer capture sequence; that does
not prove every possible debug capture path is absent.

**Wi-Fi packet capture does work with the tested stock driver.** A separate
research utility activates monitor mode through Wireless Extensions and saves
802.11 frames with radiotap headers to PCAP, even though `iw list` does not
advertise monitor support. This does not decode Bluetooth or Zigbee. See
[`docs/driver-research.md`](docs/driver-research.md) for the on-device results,
build instructions, automatic Wi-Fi recovery, and metadata limitations.

## How it works

The chip's PSD engine lives at baseband registers `0x808` (control) and `0x8B4`
(result). For each point MiYooSDR writes a tone index to `0x808`, pulses the start
bit, waits for the result to settle, and reads the power from `0x8B4`. The FFT size
in `0x808[15:14]` sets the resolution (128 to 1024 points over 40 MHz). Channels
are changed through the driver's `monitor` proc entry. Registers are reached
through the driver's `read_reg` / `write_reg` proc files, so **no kernel changes
are needed**. The driver's power saving is switched off first (otherwise the
receiver sleeps between beacons and every reading reads the same value).

Full details and the quirks found along the way are in the comments in
[`src/main.c`](src/main.c).

## Build

The Miyoo binary is cross-compiled in the same toolchain image the other Miyoo
ports use (Shaun Inman's `miyoomini-toolchain`, via Docker):

```sh
./build.sh          # -> both ARM binaries and dist/miyoosdr-0.4.0.zip
```

Pushing a version tag such as `v0.4.0` runs the GitHub release workflow. It builds
both binaries and publishes the installation ZIP, SHA-256 checksum, and matching
notes from `docs/releases/<version>.md` as an experimental prerelease.

For a desktop build with a synthetic radio, to work on the UI without hardware:

```sh
make host
build/host/miyoosdr --fake            # fake spectrum, runs on /dev/fb0 if present
build/host/miyoosdr --fake --shot out.png   # render one frame (.png or .ppm) and exit
```

Other modes: `--dump N` (N sweeps as text), `--probe` (PSD timing), `--zoom MHZ LEVEL`,
`--shot-after N` (sweeps before `--shot` renders, default 12), `--settings` (read and
write `settings.txt` even when started with options). The fake radio sweeps at about
the Mini's rate, so burst timing looks like the real thing.

## Credits and license

Inspired by ESPARGOS [esp-sdr](https://github.com/ESPARGOS/esp-sdr), which turns an
ESP32's radio into a receive-only SDR. MiYooSDR shares none of its code; it applies
the same idea to a different chip. See [CREDITS.md](CREDITS.md).

Licensed under **GPL-3.0-or-later**. See [LICENSE](LICENSE).
