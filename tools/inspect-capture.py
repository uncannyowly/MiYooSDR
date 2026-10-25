#!/usr/bin/env python3
"""Summarize a radiotap PCAP without printing addresses or packet payloads.

Requires Scapy: uv run --with scapy tools/inspect-capture.py CAPTURE.pcap
Reported PHY fields are driver-provided, not independently calibrated.
"""
import argparse
from collections import Counter
import json
import zlib

from scapy.all import Dot11, Dot11Beacon, PcapReader, RadioTap


def summarize(path):
    types, frequencies, signals, rates, present = (Counter() for _ in range(5))
    packets = undecoded = beacons = malformed_radiotap = 0
    fcs_checked = fcs_mismatches = driver_bad_fcs = 0
    first = last = None
    with PcapReader(str(path)) as reader:
        if reader.linktype != 127:
            raise ValueError(f"expected radiotap link type 127, got {reader.linktype}")
        for packet in reader:
            packets += 1
            stamp = float(packet.time)
            if first is None:
                first = stamp
            last = stamp
            if RadioTap not in packet or Dot11 not in packet:
                undecoded += 1
                continue
            radio, frame = packet[RadioTap], packet[Dot11]
            if radio.version != 0 or radio.len < 8 or radio.len > len(bytes(packet)):
                malformed_radiotap += 1
            types[f"{frame.type}:{frame.subtype}"] += 1
            flags = int(radio.Flags or 0)
            driver_bad_fcs += bool(flags & 0x40)
            raw_frame = bytes(packet)[radio.len:]
            if flags & 0x10 and len(raw_frame) >= 4:
                fcs_checked += 1
                supplied = int.from_bytes(raw_frame[-4:], "little")
                fcs_mismatches += (zlib.crc32(raw_frame[:-4]) & 0xffffffff) != supplied
            present[str(radio.present)] += 1
            for field, counter in (("ChannelFrequency", frequencies),
                                   ("dBm_AntSignal", signals), ("Rate", rates)):
                value = getattr(radio, field, None)
                if value is not None:
                    counter[value] += 1
            beacons += int(Dot11Beacon in packet)
    return {
        "packets": packets,
        "span_seconds": round(last - first, 6) if first is not None else 0,
        "undecoded": undecoded,
        "malformed_radiotap": malformed_radiotap,
        "beacons": beacons,
        "fcs_checked": fcs_checked,
        "fcs_mismatches": fcs_mismatches,
        "driver_marked_bad_fcs": driver_bad_fcs,
        "frame_type_subtype_counts": dict(types),
        "frequency_mhz_counts": dict(frequencies),
        "driver_signal_dbm_range": [min(signals), max(signals)] if signals else None,
        "legacy_rate_mbps_counts": dict(rates),
        "radiotap_present_counts": dict(present),
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture")
    args = parser.parse_args()
    print(json.dumps(summarize(args.capture), indent=2))
