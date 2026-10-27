#!/usr/bin/env python3
"""Command-line behavior of the real executable, without radio hardware.

Run after `make host`: python3 tests/test_cli.py
"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

from test_pcap import container, frame, radio


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/miyoosdr"
C_BG, C_PANEL, C_CURSOR = (0x0b, 0x0f, 0x14), (0x11, 0x18, 0x21), (0xff, 0x5c, 0x8a)
C_MGMT = (0x3d, 0xdc, 0x84)


def read_png(path):
    """Decode an 8-bit RGB PNG into (width, height, bytes) with every filter type."""
    data = Path(path).read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    offset, idat = 8, b""
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            assert (depth, colour) == (8, 2)
        elif kind == b"IDAT":
            idat += body
        offset += 12 + length
    raw, stride, pixels = zlib.decompress(idat), width * 3, bytearray()
    previous = bytearray(stride)
    for y in range(height):
        kind, row = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - 3] if i >= 3 else 0
            b, c = previous[i], previous[i - 3] if i >= 3 else 0
            if kind == 1:
                row[i] = (row[i] + a) & 255
            elif kind == 2:
                row[i] = (row[i] + b) & 255
            elif kind == 3:
                row[i] = (row[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        pixels += row
        previous = row
    return width, height, bytes(pixels)


def read_ppm(path):
    data = Path(path).read_bytes()
    header, rest = data.split(b"\n255\n", 1)
    width, height = map(int, header.split()[1:3])
    return width, height, rest


def recording(views, step=0.2):
    """Spectrum CSV in the recorder's format; views = [(level, f0, bw, ch, [rows])]."""
    lines = ["# MiYooSDR spike recording. Bin i of a view is f0 + bw*i MHz.",
             "# rows: seconds, gain(IGI), view level, then dB*10 per bin (uncalibrated)"]
    t = 0.0
    for level, f0, bw, ch, rows in views:
        lines.append(f"# view level={level} bins={len(rows[0])} f0={f0:.6f} bw={bw:.6f} ch={ch}")
        for row in rows:
            lines.append(f"{t:.3f},40,{level}," + ",".join(str(int(v * 10)) for v in row))
            t += step
    return "\n".join(lines) + "\n"


class CliTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="miyoosdr-cli-")
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)
        rt = radio((1 << 2) | (1 << 3) | (1 << 5), bytes([12, 0]) + struct.pack("<HHb", 2462, 0x00c0, -67))
        self.capture = self.path / "sample.pcap"
        self.capture.write_bytes(container([(1000, 0, rt + frame(), None),
                                            (1002, 0, rt + frame(control=0x0008), None)]))

    def run_app(self, *args, timeout=30):
        return subprocess.run([str(BINARY), *map(str, args)], cwd=self.path,
                              capture_output=True, text=True, timeout=timeout)

    def test_unknown_options_are_rejected(self):
        for extra in (["--replay=sample.pcap"], ["--bogus"], ["stray"]):
            with self.subTest(extra=extra):
                result = self.run_app("--fake", "--shot", "x.png", *extra)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("unknown option", result.stderr)
                self.assertFalse((self.path / "x.png").exists())

    def test_out_of_range_values_are_rejected(self):
        for extra in (["--pal", "4"], ["--pal", "-1"], ["--style", "3"], ["--zoom", "nan", "1"],
                      ["--zoom", "2462", "4"], ["--zoom", "1e12", "1"], ["--dump", "0"],
                      ["--dump", "x"], ["--shot-after", "0"], ["--burst", "2462", "0"],
                      ["--replay-at", "-1"]):
            with self.subTest(extra=extra):
                result = self.run_app("--fake", "--shot", "x.png", *extra)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse((self.path / "x.png").exists())

    def test_missing_values_are_rejected(self):
        for args in (["--shot"], ["--zoom", "2462"], ["--replay"], ["--fake", "--pal"]):
            with self.subTest(args=args):
                self.assertEqual(self.run_app(*args).returncode, 2)

    def test_needs_radio_reports_whether_options_use_the_radio(self):
        for args, want in ((["--needs-radio"], 0), (["--needs-radio", "--dump", "5"], 0),
                           (["--needs-radio", "--burst", "2462", "5"], 0),
                           (["--needs-radio", "--replay", "sample.pcap"], 1),
                           (["--needs-radio", "--review"], 1), (["--needs-radio", "--fake"], 1),
                           (["--needs-radio", "--version"], 1),
                           (["--needs-radio", "--replay-at", "2"], 2),
                           (["--needs-radio", "--bogus"], 2)):
            with self.subTest(args=args):
                result = self.run_app(*args)
                self.assertEqual(result.returncode, want, result.stderr)
                self.assertNotIn("power save", result.stderr)

    def test_version_matches_the_version_file(self):
        result = self.run_app("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "MiYooSDR " + (ROOT / "VERSION").read_text().strip())

    def test_failed_image_write_is_an_error(self):
        result = self.run_app("--replay", self.capture, "--shot", "/dev/full")
        self.assertNotEqual(result.returncode, 0)

    def test_png_screenshots_are_compressed_and_lossless(self):
        for name in ("frame.png", "frame.ppm"):
            result = self.run_app("--replay", self.capture, "--replay-at", "1", "--shot", name)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(read_png(self.path / "frame.png"), read_ppm(self.path / "frame.ppm"))
        self.assertLess((self.path / "frame.png").stat().st_size, 200_000)

    def test_timeline_bars_use_only_their_category_colours(self):
        # Beacons only: every bar must be management green from top to bottom.
        beacons = self.path / "beacons.pcap"
        beacons.write_bytes(container([(1000 + i // 10, (i % 10) * 100000, radio() + frame(), None)
                                       for i in range(50)]))
        self.assertEqual(self.run_app("--replay", beacons, "--shot", "b.ppm").returncode, 0)
        width, _, pixels = read_ppm(self.path / "b.ppm")
        colours = set()
        for x in range(30, 600):
            for y in range(68, 146):  # inside the timeline panel
                pixel = tuple(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3])
                if pixel not in (C_BG, C_PANEL, C_CURSOR):
                    colours.add(pixel)
        self.assertEqual(colours, {C_MGMT})

    def test_csv_recording_replays_headless(self):
        rows = [[40 + (i % 7) for i in range(256)] for _ in range(6)]
        rows[5][100] = 90
        path = self.path / "rec.csv"
        path.write_text(recording([(0, 2402.0, 0.3125, 0, rows)]))
        first = self.run_app("--replay", path, "--replay-at", "0", "--shot", "a.png")
        last = self.run_app("--replay", path, "--replay-at", "1", "--shot", "b.png")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(last.returncode, 0, last.stderr)
        self.assertNotEqual(read_png(self.path / "a.png"), read_png(self.path / "b.png"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
