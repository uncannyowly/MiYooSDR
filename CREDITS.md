# Credits

- **ESPARGOS [esp-sdr](https://github.com/ESPARGOS/esp-sdr)** (GPL-3.0-or-later)
  is the inspiration. It turns an ESP32's Wi-Fi radio into a receive-only SDR.
  MiYooSDR copies none of its code and targets a different chip (Realtek RTL8188FU
  instead of an Espressif part), but the idea of reusing a Wi-Fi chip's own
  receiver as a spectrum sensor is theirs. esp-sdr notes that the capture
  mechanism itself is not covered by copyright, only the code is.

- The **Realtek RTL8188FU vendor driver** (GPL-2.0) was the reference for which
  baseband registers drive the PSD engine. The register addresses and the
  measurement procedure are facts about the hardware; the code here is original.

- The text font is **Terminus** (`Lat2-Terminus16`), by Dimitar Toshkov Zhekov,
  licensed under the SIL Open Font License 1.1. `src/font.h` is a small bitmap
  generated from it by `tools/mkfont.py`.

- Built with the **miyoomini-toolchain** by Shaun Inman.

MiYooSDR's own code is licensed GPL-3.0-or-later (see `LICENSE`), chosen to match
its esp-sdr lineage.
