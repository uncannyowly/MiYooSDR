#!/usr/bin/env python3
"""Exercise offline playback through the real executable without radio hardware."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from test_pcap import container, frame, radio


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/miyoosdr"


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="miyoosdr-replay-")
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name)
        self.capture = self.path / "sample.pcap"
        # Two independent timestamped management/data frames with PHY metadata.
        rt = radio((1 << 2) | (1 << 3) | (1 << 5),
                   bytes([12, 0]) + struct.pack("<HHb", 2462, 0x00c0, -67))
        self.capture.write_bytes(container([
            (1000, 0, rt + frame(), None),
            (1002, 0, rt + frame(control=0x0008), None),
        ]))

    def run_replay(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], cwd=self.path,
                              capture_output=True, text=True, timeout=5)

    def render(self, at, name):
        image = self.path / name
        result = self.run_replay("--replay", self.capture, "--replay-at", at, "--shot", image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 frames, 2.000000 seconds, 0 skipped", result.stderr)
        self.assertNotIn("power save", result.stderr)
        self.assertEqual(image.read_bytes()[:8], b"\x89PNG\r\n\x1a\n")
        return image.read_bytes()

    def test_replay_renders_offline_and_clamps_end(self):
        start = self.render(0, "start.png")
        end = self.render(2, "end.png")
        beyond = self.render(999, "beyond.png")
        self.assertNotEqual(start, end)
        self.assertEqual(end, beyond)

    def test_bad_timestamp_options_rejected_before_radio_access(self):
        for value in ("-1", "nan", "inf", "garbage", "1second"):
            with self.subTest(value=value):
                result = self.run_replay("--replay", self.capture, "--replay-at", value)
                self.assertEqual(result.returncode, 2)
                self.assertIn("nonnegative finite seconds", result.stderr)
                self.assertNotIn("power save", result.stderr)
        result = self.run_replay("--replay-at", 0)
        self.assertEqual(result.returncode, 2)
        self.assertIn("requires --replay", result.stderr)

    def test_malformed_capture_returns_failure_without_image(self):
        self.capture.write_bytes(b"invalid")
        image = self.path / "invalid.png"
        result = self.run_replay("--replay", self.capture, "--shot", image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Truncated PCAP header", result.stderr)
        self.assertFalse(image.exists())

    def test_offline_dispatch_does_not_apply_power_mode_option(self):
        result = self.run_replay("--pm-active", "--replay", self.capture,
                                 "--shot", self.path / "offline.png")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("power save", result.stderr)

    def test_launcher_executes_offline_path_before_network_setup(self):
        launcher = self.path / "launch.sh"
        launcher.write_text((ROOT / "app/MiYooSDR/launch.sh").read_text())
        stub = self.path / "miyoosdr"
        stub.write_text('#!/bin/sh\nprintf "offline playback: %s\\n" "$*"\n')
        stub.chmod(0o755)
        for option in ("--replay", "--review"):
            result = subprocess.run(["sh", str(launcher), option, "sample.pcap"],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"offline playback: {option} sample.pcap", result.stdout)
            self.assertFalse((self.path / "miyoosdr.log").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
