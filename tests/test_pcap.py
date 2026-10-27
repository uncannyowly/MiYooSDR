#!/usr/bin/env python3
"""Parser regressions using independent binary PCAP fixtures; no third-party modules.

Run: python3 tests/test_pcap.py
The optional real-device regression uses build/research/stock-monitor.pcap if present.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAX_RECORDS = 100000


class Record(ctypes.Structure):
    _fields_ = [
        ("time", ctypes.c_double), ("length", ctypes.c_uint32),
        ("frequency", ctypes.c_uint16), ("rate_half", ctypes.c_uint16),
        ("sequence", ctypes.c_uint16), ("signal", ctypes.c_int16),
        ("type", ctypes.c_uint8), ("subtype", ctypes.c_uint8),
        ("protected_frame", ctypes.c_uint8), ("retry", ctypes.c_uint8),
        ("bad_fcs", ctypes.c_uint8), ("mcs", ctypes.c_uint8),
        ("receiver", ctypes.c_uint8 * 6), ("transmitter", ctypes.c_uint8 * 6),
        ("has_receiver", ctypes.c_uint8), ("has_transmitter", ctypes.c_uint8),
    ]


class Capture(ctypes.Structure):
    _fields_ = [("packets", ctypes.POINTER(Record)), ("count", ctypes.c_size_t),
                ("duration", ctypes.c_double), ("skipped", ctypes.c_size_t),
                ("limited", ctypes.c_int)]


def frame(control=0x0080, length=24):
    packet = bytearray(length)
    struct.pack_into("<H", packet, 0, control)
    packet[4:10] = bytes.fromhex("010203040506")
    if length >= 16:
        packet[10:16] = bytes.fromhex("102030405060")
    if length >= 24:
        struct.pack_into("<H", packet, 22, 0xabc3)
    return bytes(packet)


def radio(present=0, fields=b"", words=None):
    words = [present] if words is None else words
    return struct.pack("<BBH", 0, 0, 4 + 4 * len(words) + len(fields)) + b"".join(
        struct.pack("<I", word) for word in words) + fields


def container(records, big=False, nano=False, snaplen=65535, linktype=127):
    order = ">" if big else "<"
    magic = 0xa1b23c4d if nano else 0xa1b2c3d4
    output = struct.pack(order + "IHHIIII", magic, 2, 4, 0, 0, snaplen, linktype)
    for seconds, fraction, packet, original in records:
        output += struct.pack(order + "IIII", seconds, fraction, len(packet),
                              len(packet) if original is None else original) + packet
    return output


def one(packet=None, **kwargs):
    if packet is None:
        packet = radio() + frame()
    return container([(1000, 0, packet, None)], **kwargs)


class ParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="miyoosdr-pcap-tests-")
        library = Path(cls.directory.name) / "pcap.so"
        subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-fPIC", "-shared", str(ROOT / "src/pcap.c"), "-o", str(library)],
                       check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.pcap_load.argtypes = [ctypes.c_char_p, ctypes.POINTER(Capture),
                                      ctypes.c_char_p, ctypes.c_size_t]
        cls.lib.pcap_load.restype = ctypes.c_int
        cls.lib.pcap_free.argtypes = [ctypes.POINTER(Capture)]
        cls.lib.packet_type_name.argtypes = [ctypes.POINTER(Record)]
        cls.lib.packet_type_name.restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def load(self, data=None, path=None, error=None):
        if path is None:
            path = Path(self.directory.name) / "fixture.pcap"
            path.write_bytes(data)
        capture, message = Capture(), ctypes.create_string_buffer(256)
        result = self.lib.pcap_load(str(path).encode(), ctypes.byref(capture), message, len(message))
        self.addCleanup(self.lib.pcap_free, ctypes.byref(capture))
        if error is not None:
            self.assertEqual(result, -1)
            self.assertIn(error, message.value.decode())
            self.assertEqual(capture.count, 0)
            self.assertFalse(capture.packets)
        else:
            self.assertEqual(result, 0, message.value.decode())
        return capture

    def test_endian_micro_and_nano_time(self):
        packet = radio() + frame()
        for big in (False, True):
            for nano in (False, True):
                with self.subTest(big=big, nano=nano):
                    scale = 1000000000 if nano else 1000000
                    capture = self.load(container([
                        (1000, scale // 4, packet, None),
                        (1001, scale // 2, packet, None),
                    ], big=big, nano=nano))
                    self.assertEqual(capture.count, 2)
                    self.assertEqual(capture.packets[0].time, 0)
                    self.assertAlmostEqual(capture.packets[1].time, 1.25)
                    self.assertAlmostEqual(capture.duration, 1.25)

    def test_metadata_and_alignment(self):
        # TSFT requires eight-byte alignment; channel requires two-byte alignment.
        fields = struct.pack("<QBBHHbB", 123456789, 0x50, 93, 2462, 0x00c0, -90, 1)
        fields += struct.pack("<H", 0)  # RX flags aligned at offset 24.
        packet = radio(0x482f, fields) + frame(0x4880) + b"\x00" * 4
        capture = self.load(one(packet))
        record = capture.packets[0]
        self.assertEqual((record.type, record.subtype, record.sequence), (0, 8, 0xabc))
        self.assertEqual((record.frequency, record.rate_half, record.signal), (2462, 93, -90))
        self.assertEqual((record.protected_frame, record.retry, record.bad_fcs), (1, 1, 1))
        self.assertEqual(record.length, 24)
        self.assertEqual(bytes(record.receiver), bytes.fromhex("010203040506"))
        self.assertEqual(bytes(record.transmitter), bytes.fromhex("102030405060"))
        self.assertEqual((record.has_receiver, record.has_transmitter), (1, 1))
        self.assertEqual(self.lib.packet_type_name(ctypes.byref(record)), b"Beacon")

    def test_unknown_sentinels_and_zero_signal(self):
        capture = self.load(one())
        record = capture.packets[0]
        self.assertEqual((record.signal, record.rate_half, record.frequency, record.mcs),
                         (32767, 0, 0, 255))
        capture = self.load(one(radio(1 << 5, b"\x00") + frame()))
        self.assertEqual(capture.packets[0].signal, 0)

    def test_mcs_presence_requires_known_index(self):
        for known, expected in [(0, 255), (2, 7)]:
            with self.subTest(known=known):
                capture = self.load(one(radio(1 << 19, bytes([known, 0, 7])) + frame()))
                self.assertEqual(capture.packets[0].mcs, expected)

    def test_extended_bitmap_alignment_and_namespace_reset(self):
        # Data begins at 12 but TSFT aligns to 16, relative to radiotap start.
        fields = b"\x00" * 4 + struct.pack("<Qb", 1234, -42)
        capture = self.load(one(radio(fields=fields, words=[0x80000021, 0]) + frame()))
        self.assertEqual(capture.packets[0].signal, -42)
        # Reset marker lets the next word describe standard field 5 again.
        capture = self.load(one(radio(fields=b"\xa6", words=[0xa0000000, 1 << 5]) + frame()))
        self.assertEqual(capture.packets[0].signal, -90)

    def test_vendor_namespace_skip_and_return(self):
        # The vendor header starts at 16 and consumes header + three opaque bytes.
        # A namespace reset in word two restores regular RSSI in word three.
        fields = bytes.fromhex("11223301") + struct.pack("<H", 3) + b"abc" + b"\xd8"
        words = [0xc0000000, 0xa0000001, 1 << 5]
        capture = self.load(one(radio(fields=fields, words=words) + frame()))
        self.assertEqual(capture.packets[0].signal, -40)

    def test_tlv(self):
        fields = struct.pack("<HHb", 5, 1, -55) + b"\x00" * 3
        capture = self.load(one(radio(1 << 28, fields) + frame()))
        self.assertEqual(capture.packets[0].signal, -55)

    def test_small_backward_steps_hold_time(self):
        # Delivery reordering of a few milliseconds, as in the real capture.
        packet = radio() + frame()
        capture = self.load(container([(10, us, packet, None) for us in (0, 2000, 1000, 3000)]))
        times = [capture.packets[i].time for i in range(capture.count)]
        for got, want in zip(times, [0, 0.002, 0.002, 0.003]):
            self.assertAlmostEqual(got, want, places=9)

    def test_clock_step_back_keeps_later_spacing(self):
        # The capture clock stepped back 10 s after the third frame.
        packet = radio() + frame()
        capture = self.load(container([(s, 0, packet, None) for s in (10, 11, 12, 2, 3, 4, 5)]))
        self.assertEqual([capture.packets[i].time for i in range(capture.count)], [0, 1, 2, 2, 3, 4, 5])
        self.assertEqual(capture.duration, 5)

    def test_combined_signal_wins_over_per_chain_fields(self):
        # mac80211 layout: word 0 has the combined signal, words 1 and 2 repeat
        # the field per antenna chain after a radiotap namespace reset.
        words = [(1 << 5) | (1 << 29) | (1 << 31), (1 << 5) | (1 << 11) | (1 << 29) | (1 << 31),
                 (1 << 5) | (1 << 11)]
        fields = struct.pack("<bbBbB", -50, -60, 0, -70, 1)
        capture = self.load(one(radio(fields=fields, words=words) + frame()))
        self.assertEqual(capture.packets[0].signal, -50)

    def test_action_no_ack_name(self):
        capture = self.load(one(radio() + frame(0x00e0)))
        self.assertEqual(self.lib.packet_type_name(ctypes.byref(capture.packets[0])), b"Action No Ack")

    def test_control_short_addresses(self):
        for subtype in (7, 8, 9, 10, 11, 12, 13, 14, 15):
            with self.subTest(subtype=subtype):
                length = {8: 20, 9: 28, 12: 10, 13: 10}.get(subtype, 16)
                capture = self.load(one(radio() + frame((subtype << 4) | 4, length)))
                record = capture.packets[0]
                self.assertEqual(record.sequence, 65535)
                self.assertEqual(record.has_receiver, 1)
                self.assertEqual(record.has_transmitter, subtype not in (7, 12, 13))

    def test_qos_wds_header_size(self):
        control = 0x8388  # QoS data, both DS bits, order/HT control.
        packet = radio() + frame(control, 36)
        capture = self.load(one(packet))
        self.assertEqual((capture.packets[0].type, capture.packets[0].subtype), (2, 8))
        self.load(one(packet[:-1]), error="No supported")

    def test_prefix_payload_skipping_and_original_length(self):
        packet = radio() + frame() + b"x" * 12000
        capture = self.load(one(packet))
        self.assertEqual(capture.packets[0].length, 12024)
        # Snapshot truncation is valid, unlike a physically truncated PCAP file.
        capture = self.load(container([(1, 0, (radio(2, b"\x10") + frame()), 200)]))
        self.assertEqual(capture.packets[0].length, 187)
        # Incomplete FCS bytes cannot make a too-short original MAC header valid.
        short = radio(2, b"\x10") + frame()
        self.load(container([(1, 0, short, len(short) + 1)]), error="No supported")
        self.load(one(packet)[:-1], error="Truncated PCAP record 1 data")

    def test_malformed_frame_skips_and_timeline_origin(self):
        good = radio() + frame()
        bad = [b"", b"\x00" * 7, radio(1 << 31), radio(1 << 5),
               radio(1 << 30, b"\x00" * 4 + b"\xff\xff"),
               radio(fields=b"", words=[0x80000000, 1]),
               radio() + frame(0x0081), radio() + frame()[:23],
               radio(2, b"\x10") + frame()[:3],
               radio(1 << 28, struct.pack("<HH", 5, 100)),
               radio(fields=b"", words=[0xe0000000, 0])]
        records = [(i, 0, packet, None) for i, packet in enumerate(bad)]
        records.extend([(100, 0, good, None), (101, 0, good, None)])
        capture = self.load(container(records))
        self.assertEqual((capture.count, capture.skipped), (2, len(bad)))
        self.assertEqual(capture.duration, 1)

    def test_header_failures(self):
        for data, message in [
            (b"", "Truncated PCAP header"),
            (b"\x0a\x0d\x0d\x0a" + b"\x00" * 20, "PCAPNG"),
            (b"?" * 24, "Unrecognized"),
            (one(linktype=1), "link type 127"),
            (one(snaplen=0), "snapshot length"),
            (container([]), "No supported"),
        ]:
            with self.subTest(message=message):
                self.load(data, error=message)
        data = bytearray(one())
        struct.pack_into("<H", data, 4, 3)
        self.load(data, error="Unsupported PCAP version")

    def test_record_failures(self):
        for nano in (False, True):
            with self.subTest(nano=nano):
                data = bytearray(one(nano=nano))
                struct.pack_into("<I", data, 28, 1000000000 if nano else 1000000)
                self.load(data, error="timestamp or lengths")
        for offset, value in [(32, 65536), (36, 1)]:
            data = bytearray(one())
            struct.pack_into("<I", data, offset, value)
            self.load(data, error="timestamp or lengths")
        self.load(one()[:30], error="record 1 header")
        self.load(one()[:-1], error="record 1 data")
        self.load(one() + b"\x00", error="record 2 header")

    def test_metadata_and_scan_limits(self):
        packet = radio() + frame()
        record = struct.pack("<IIII", 10, 0, len(packet), len(packet)) + packet
        header = container([])
        capture = self.load(header + record * MAX_RECORDS)
        self.assertEqual((capture.count, capture.limited), (MAX_RECORDS, 0))
        capture = self.load(header + record * (MAX_RECORDS + 1))
        self.assertEqual((capture.count, capture.limited), (MAX_RECORDS, 1))
        self.assertLessEqual(ctypes.sizeof(Record) * MAX_RECORDS, 8 * 1024 * 1024)
        # Malformed records consume the scan budget as well as valid frames.
        invalid = struct.pack("<IIII", 10, 0, 0, 0)
        capture = self.load(header + record + invalid * MAX_RECORDS)
        self.assertEqual((capture.count, capture.skipped, capture.limited), (1, MAX_RECORDS - 1, 1))

    def test_real_device_capture(self):
        path = ROOT / "build/research/stock-monitor.pcap"
        if not path.exists():
            self.skipTest("optional real-device capture unavailable")
        capture = self.load(path=path)
        self.assertEqual((capture.count, capture.skipped, capture.limited), (3647, 0, 0))
        self.assertAlmostEqual(capture.duration, 8.003792, places=6)
        counts = {}
        signals = []
        for i in range(capture.count):
            record = capture.packets[i]
            counts[(record.type, record.subtype)] = counts.get((record.type, record.subtype), 0) + 1
            self.assertEqual(record.frequency, 2462)
            self.assertEqual(record.bad_fcs, 0)
            signals.append(record.signal)
        self.assertEqual(counts, {(2, 8): 867, (1, 11): 641, (1, 12): 613,
                                 (1, 9): 566, (1, 8): 85, (0, 8): 202,
                                 (1, 13): 323, (2, 0): 242, (2, 4): 107, (0, 13): 1})
        self.assertEqual((min(signals), max(signals)), (-90, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
