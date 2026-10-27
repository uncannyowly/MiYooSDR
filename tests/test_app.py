#!/usr/bin/env python3
"""App internals driven headlessly through tests/harness.c (the app with main() renamed).

Run: python3 tests/test_app.py
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

from test_cli import recording
from test_pcap import container, frame, radio


ROOT = Path(__file__).resolve().parents[1]
# Never let a test reach this machine's own framebuffer or keyboard.
SAFE_ENV = dict(os.environ, MIYOOSDR_FB="/nonexistent/fb", MIYOOSDR_INPUT="/nonexistent/input")
REVIEW_LIMIT = 4096  # readings the harness's CSV review keeps, so truncation is cheap to reach


def burst(channel, rows):
    f0 = 2407 + 5 * channel - 64 * 0.3125
    lines = [f"# MiYooSDR burst capture: channel {channel}, bin i = {f0:.4f} + 0.3125*i MHz, 128 bins, 128-pt FFT",
             "# columns: seconds, gain(IGI), then dB*10 per bin (uncalibrated)"]
    lines += [f"{0.04 * k:.4f},40," + ",".join(str(int(v * 10)) for v in row) for k, row in enumerate(rows)]
    return "\n".join(lines) + "\n"


class AppTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="miyoosdr-app-tests-")
        cls.binary = Path(cls.directory.name) / "harness"
        subprocess.run(["cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        "-Wno-format-truncation", f"-DREVIEW_MAX_VALUES={REVIEW_LIMIT}u", "-o", str(cls.binary),
                        str(ROOT / "tests/harness.c"), str(ROOT / "src/pcap.c"), "-lm", "-lpthread"], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="miyoosdr-app-")
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)

    def harness(self, *args):
        result = subprocess.run([str(self.binary), *map(str, args)], cwd=self.path, env=SAFE_ENV,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_zoom_during_a_sweep_leaves_no_old_view_data_in_max_hold(self):
        out = self.harness("zoom-mid-sweep")
        self.assertIn("level 1 untouched 128 of 128", out)
        self.assertIn("next sweep untouched 0", out)

    def test_gain_change_during_a_sweep_applies_from_the_next_sweep(self):
        gain, worst = re.search(r"gain now (\w+) worst ([\d.]+)", self.harness("gain-mid-sweep")).groups()
        self.assertEqual(gain, "2A")
        self.assertLess(float(worst), 0.01)

    def test_peak_reports_the_frequency_the_bin_measures(self):
        # Full band bin 100 is tone -28 of channel 5 (2432 MHz): 2402 + 100 * 0.3125.
        self.assertIn("peak 2433.25 MHz, 30 dB over background", self.harness("peak"))

    def test_zooming_in_keeps_the_cursor_on_the_same_frequency(self):
        level, now, was, half = re.search(r"level (\d) cursor ([\d.]+) was ([\d.]+) half-bin ([\d.]+)",
                                          self.harness("zoom-keeps-freq")).groups()
        self.assertEqual(level, "3")
        self.assertLessEqual(abs(float(now) - float(was)), float(half) + 1e-9)

    def test_panning_past_the_band_edge_never_wraps_fft_tones(self):
        steps = re.findall(r"tone0 (-?\d+) last (-?\d+) half (\d+)", self.harness("pan-left"))
        self.assertEqual(len(steps), 6)
        for tone0, last, half in steps:
            self.assertGreaterEqual(int(tone0), -int(half))
            self.assertLessEqual(int(last), int(half) - 1)

    def test_holding_x_does_not_toggle_the_recording_back_off(self):
        self.assertIn("recording 1", self.harness("autorepeat", self.path))
        self.assertEqual(len(list((self.path / "rec").glob("*.csv"))), 1)

    def test_packet_replay_shows_status_messages(self):
        capture = self.path / "one.pcap"
        capture.write_bytes(container([(1000, 0, radio() + frame(), None)]))
        self.assertIn("status shown 1", self.harness("replay-status", capture))

    def test_screenshots_in_the_same_second_do_not_overwrite(self):
        self.assertIn("shots 3", self.harness("shots", self.path))

    def test_browser_lists_everything_newest_first(self):
        (self.path / "rec").mkdir()
        (self.path / "research").mkdir()
        for i in range(130):  # more than the old shared 128-entry list held
            name = self.path / f"rec/sdr-{i:03d}.csv"
            name.write_text("0.000,40,0,400\n")
            os.utime(name, (1_000_000 + i, 1_000_000 + i))
        for name, stamp in (("research/stock-monitor.pcap", 999_000), ("rec/COPY.PCAP", 2_000_000),
                            ("research/notes.txt", 3_000_000)):
            (self.path / name).write_bytes(b"x")
            os.utime(self.path / name, (stamp, stamp))
        lines = self.harness("list", self.path).splitlines()
        self.assertEqual(len(lines), 132)
        self.assertEqual(lines[0], "P rec/COPY.PCAP")
        self.assertEqual(lines[1], "C rec/sdr-129.csv")
        self.assertEqual(lines[-1], "P research/stock-monitor.pcap")

    def test_b_in_a_direct_replay_opens_the_browser(self):
        capture = self.path / "one.pcap"
        capture.write_bytes(container([(1000, 0, radio() + frame(), None)]))
        self.assertIn("browser 1 drained 1", self.harness("replay-back", capture))

    def test_csv_rows_keep_their_own_views_frequency_axis(self):
        full = [[40] * 256 for _ in range(3)]
        zoom = [[50] * 128 for _ in range(2)]
        path = self.path / "views.csv"
        path.write_text(recording([(0, 2402.0, 0.3125, 0, full), (1, 2430.0, 0.15625, 5, zoom)]))
        out = self.harness("csv-load", path)
        self.assertIn("sections 2 rows 5 truncated 0 duration 0.800", out)
        self.assertIn("section level 0 nb 256 f0 2402.0000 bw 0.31250 ch 0 first 0 rows 3", out)
        self.assertIn("section level 1 nb 128 f0 2430.0000 bw 0.15625 ch 5 first 3 rows 2", out)
        self.assertIn("view level 1 nb 128 f0 2430.0000 bw 0.15625 ch 5", self.harness("csv-show", path, 3))
        self.assertIn("view level 0 nb 256 f0 2402.0000 bw 0.31250 ch 0", self.harness("csv-show", path, 2))

    def test_csv_waterfall_shows_the_rows_recorded_before(self):
        rows = [[40 + 5 * k] * 256 for k in range(5)]  # each row louder than the one before
        path = self.path / "rising.csv"
        path.write_text(recording([(0, 2402.0, 0.3125, 0, rows)]))
        out = self.harness("csv-show", path, 4)
        self.assertIn("wf_rows 5", out)
        levels = [int(v) for v in re.search(r"^wf (.*)$", out, re.M).group(1).split()]
        self.assertEqual(levels, sorted(levels, reverse=True))
        self.assertEqual(len(set(levels)), 5)

    def test_csv_burst_capture_loads_its_channel(self):
        path = self.path / "burst.csv"
        path.write_text(burst(6, [[45] * 128 for _ in range(4)]))
        out = self.harness("csv-load", path)
        self.assertIn("sections 1 rows 4 truncated 0 duration 0.120", out)
        self.assertIn("section level 0 nb 128 f0 2417.0000 bw 0.31250 ch 6 first 0 rows 4", out)

    def test_csv_corrupt_header_is_rejected(self):
        path = self.path / "huge.csv"
        path.write_text(recording([(0, 2402.0, 0.3125, 0, [[40] * 4])]).replace("bw=0.312500", "bw=1e300"))
        self.assertIn("error: Unrecognised recording header", self.harness("csv-load", path))

    def test_csv_torn_and_timeless_rows_are_skipped(self):
        text = recording([(0, 2402.0, 0.3125, 0, [[40] * 256 for _ in range(3)])])
        text += "nan,40,0," + ",".join(["400"] * 256) + "\n"  # no usable time
        text += "0.800,40,0,400,400\n"  # wrong number of readings
        text += "1"  # torn last line
        path = self.path / "torn.csv"
        path.write_text(text)
        self.assertIn("sections 1 rows 3 truncated 0 duration 0.400", self.harness("csv-load", path))

    def test_csv_stops_at_the_reading_limit(self):
        rows = [[40] * 128 for _ in range(40)]  # 5120 readings, over the harness's 4096
        path = self.path / "long.csv"
        path.write_text(recording([(1, 2430.0, 0.15625, 5, rows)]))
        self.assertIn(f"rows {REVIEW_LIMIT // 128} truncated 1", self.harness("csv-load", path))

    def test_csv_review_draws_its_own_footer(self):
        path = self.path / "footer.csv"
        path.write_text(recording([(0, 2402.0, 0.3125, 0, [[40] * 256 for _ in range(2)])]))
        self.assertIn("render footer pixels 0 review help 1", self.harness("csv-frame", path, 1))


if __name__ == "__main__":
    unittest.main(verbosity=2)
