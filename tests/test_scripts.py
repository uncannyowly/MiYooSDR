#!/usr/bin/env python3
"""Shell launchers, the capture script and the Makefile, run for real against stub
system commands so nothing touches this machine's networking.

Run after `make host`: python3 tests/test_scripts.py
"""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

from test_pcap import container, frame, radio


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/host/miyoosdr"


class ScriptTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="miyoosdr-scripts-")
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name)
        self.bin = self.path / "bin"
        self.bin.mkdir()
        self.calls = self.path / "calls"
        self.calls.write_text("")

    def stub(self, name, body="", directory=None):
        """A command that logs its arguments to self.calls, then runs body."""
        script = (directory or self.bin) / name
        script.write_text(f'#!/bin/sh\necho "{name} $*" >> "{self.calls}"\n{body}\n')
        script.chmod(0o755)
        return script

    def env(self, **extra):
        return dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", MIYOOSDR_FB="/nonexistent/fb",
                    MIYOOSDR_INPUT="/nonexistent/input", **extra)

    def logged(self):
        return self.calls.read_text().splitlines()


class LauncherTests(ScriptTest):
    def setUp(self):
        super().setUp()
        self.app = self.path / "app"
        self.app.mkdir()
        shutil.copy(ROOT / "app/MiYooSDR/launch.sh", self.app / "launch.sh")
        self.rtl = self.path / "rtl"  # stands in for /proc/net/rtl8188fu/wlan0
        self.rtl.mkdir()
        (self.rtl / "monitor").write_text("")
        (self.rtl / "rf_info").write_text("cur_ch=11\n")
        self.stub("pgrep", 'case "$1" in wpa_supplicant|udhcpc) exit 0;; esac\nexit 1')
        self.stub("ifconfig", 'echo "wlan0 UP BROADCAST RUNNING"')
        for name in ("killall", "udhcpc"):
            self.stub(name)

    def launch(self, *args):
        return subprocess.run(["sh", str(self.app / "launch.sh"), *map(str, args)], capture_output=True,
                              text=True, timeout=60, env=self.env(MIYOOSDR_RTL_PROC=str(self.rtl)))

    def fake_app(self, live_exit=0):
        """miyoosdr stand-in: needs the radio, and exits with live_exit when run live."""
        self.stub("miyoosdr", 'case "$1" in --needs-radio|--pm-active) exit 0;; esac\n'
                              f'exit {live_exit}', directory=self.app)

    def test_offline_options_run_without_touching_wifi(self):
        shutil.copy(BINARY, self.app / "miyoosdr")
        capture = self.path / "sample.pcap"
        capture.write_bytes(container([(1000, 0, radio() + frame(), None)]))
        self.stub("pkill")
        result = self.launch("--replay", capture, "--shot", self.path / "out.png")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.path / "out.png").exists())
        result = self.launch("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("MiYooSDR "))
        self.assertFalse((self.app / "miyoosdr.log").exists())
        self.assertNotIn("pkill -9 wpa_supplicant", self.logged())

    def test_option_errors_stop_before_wifi_goes_down(self):
        shutil.copy(BINARY, self.app / "miyoosdr")
        self.stub("pkill")
        for args in (["--replay-at", "2", "x.pcap"], ["--replay=x.pcap"], ["--dump", "zero"]):
            with self.subTest(args=args):
                self.assertEqual(self.launch(*args).returncode, 2)
        self.assertEqual(self.logged(), [])
        self.assertFalse((self.app / "miyoosdr.log").exists())

    def test_signal_during_setup_restores_once_and_stops(self):
        self.fake_app()
        # The first teardown step is interrupted, as by Ctrl+C or a dropped SSH session.
        self.stub("pkill", 'case "$*" in "-9 wpa_supplicant") kill -TERM $PPID; sleep 1;; esac')
        result = self.launch()
        self.assertEqual(result.returncode, 143, result.stderr)
        calls = self.logged()
        self.assertNotIn("pkill -9 udhcpc", calls)
        self.assertNotIn("killall -9 audioserver", calls)
        self.assertFalse(any(c.startswith("miyoosdr ") and "--" not in c for c in calls))
        self.assertEqual((self.app / "miyoosdr.log").read_text().count("restore: done"), 1)

    def test_live_exit_status_is_passed_on(self):
        self.fake_app(live_exit=4)
        self.stub("pkill")
        result = self.launch("--dump", "5")
        self.assertEqual(result.returncode, 4, result.stderr)
        self.assertIn("restore: done", (self.app / "miyoosdr.log").read_text())

    def test_offline_crash_still_resumes_mainui(self):
        self.stub("miyoosdr", 'case "$1" in --needs-radio) exit 1;; esac\nkill -KILL $$', directory=self.app)
        self.stub("pkill")
        result = self.launch("--review")
        self.assertEqual(result.returncode, 128 + signal.SIGKILL)
        self.assertIn("pkill -CONT MainUI", self.logged())


class ReplayEntryTests(ScriptTest):
    def test_replay_entry_opens_the_browser_without_live_mode(self):
        apps = self.path / "App"
        (apps / "MiYooSDR").mkdir(parents=True)
        shutil.copytree(ROOT / "app/MiYooSDR Replay", apps / "MiYooSDR Replay")
        self.stub("launch.sh", 'pwd >> "%s"' % self.calls, directory=apps / "MiYooSDR")
        result = subprocess.run(["sh", str(apps / "MiYooSDR Replay/launch.sh")], capture_output=True, text=True,
                                timeout=30, env=self.env())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.logged(), ["launch.sh --review", str(apps / "MiYooSDR")])


class CaptureScriptTests(ScriptTest):
    def setUp(self):
        super().setUp()
        self.research = self.path / "research"
        self.research.mkdir()
        shutil.copy(ROOT / "tools/monitor-capture.sh", self.research / "monitor-capture.sh")
        self.net = self.path / "net"  # stands in for /sys/class/net/wlan0
        self.net.mkdir()
        (self.net / "type").write_text("1\n")
        self.wpa = self.stub("wpa_supplicant")
        for name in ("killall", "ifconfig", "udhcpc"):
            self.stub(name)

    def run_script(self, *args):
        return subprocess.run(["sh", str(self.research / "monitor-capture.sh"), *args], capture_output=True,
                              text=True, timeout=60,
                              env=self.env(MIYOO_WPA=str(self.wpa), MIYOO_NET=str(self.net),
                                           MIYOO_LOCK=str(self.path / "lock")))

    def test_missing_capture_tool_stops_before_wifi_goes_down(self):
        self.stub("pgrep", "exit 0")
        result = self.run_script()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("miyoo-pcap", result.stderr)
        self.assertFalse(any(c.startswith("killall") for c in self.logged()))

    def test_restore_restarts_wifi_when_already_managed(self):
        # Interface type 1: it never left managed mode, so a failing mode switch must not block Wi-Fi.
        self.stub("miyoo-pcap", "exit 1", directory=self.research)
        self.stub("pgrep", "exit 1")
        result = self.run_script("--restore")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(any(c.startswith("wpa_supplicant -B") for c in self.logged()))
        deadline = time.monotonic() + 3  # udhcpc is started in the background
        while not any(c.startswith("udhcpc") for c in self.logged()) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertTrue(any(c.startswith("udhcpc") for c in self.logged()))


class MakefileTests(ScriptTest):
    def test_clean_keeps_local_research_data(self):
        shutil.copy(ROOT / "Makefile", self.path / "Makefile")
        shutil.copy(ROOT / "VERSION", self.path / "VERSION")
        for name in ("host/miyoosdr", "miyoo/miyoosdr", "research/stock-monitor.pcap"):
            (self.path / "build" / name).parent.mkdir(parents=True, exist_ok=True)
            (self.path / "build" / name).write_text("x")
        subprocess.run(["make", "-s", "clean"], cwd=self.path, check=True, timeout=30)
        self.assertTrue((self.path / "build/research/stock-monitor.pcap").exists())
        self.assertFalse((self.path / "build/host").exists())
        self.assertFalse((self.path / "build/miyoo").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
