#!/usr/bin/env python3
"""Package existing ARM builds; run ./build.sh [VERSION] to compile first."""
import argparse
import hashlib
from pathlib import Path
import re
import stat
import subprocess
import time
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def package(version):
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", version):
        raise ValueError("version must be X.Y.Z with an optional prerelease suffix")
    # Explicit allowlist: never ship local captures, settings, keys, or logs.
    files = {
        "miyoosdr": "build/miyoo/miyoosdr",
        "config.json": "app/MiYooSDR/config.json",
        "icon.png": "app/MiYooSDR/icon.png",
        "launch.sh": "app/MiYooSDR/launch.sh",
        "research/miyoo-pcap": "build/miyoo/miyoo-pcap",
        "research/monitor-capture.sh": "tools/monitor-capture.sh",
        "research/inspect-capture.py": "tools/inspect-capture.py",
        "README.md": "README.md",
        "LICENSE": "LICENSE",
        "CREDITS.md": "CREDITS.md",
        "docs/driver-research.md": "docs/driver-research.md",
        "docs/screenshot.png": "docs/screenshot.png",
        f"docs/releases/{version}.md": f"docs/releases/{version}.md",
    }
    for name in ("miyoosdr", "research/miyoo-pcap"):
        header = (ROOT / files[name]).read_bytes()[:20]
        if (header[:6] != b"\x7fELF\x01\x01" or
                int.from_bytes(header[18:20], "little") != 40):
            raise ValueError(f"{name}: expected a little-endian 32-bit ARM ELF binary")
    source = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain", "--untracked-files=normal"))
    build_info = (
        f"MiYooSDR {version}\nSource commit: {source}\n"
        f"Source worktree: {'modified' if dirty else 'clean'}\n"
        "Target: Miyoo Mini Plus / OnionOS / ARMv7 hard-float\n"
        "Packet capture: stock RTL8188FU via Wireless Extensions; see docs/driver-research.md\n"
    )
    timestamp = time.gmtime(max(315532800, int(git("log", "-1", "--format=%ct"))))[:6]
    output = ROOT / "dist" / f"miyoosdr-{version}.zip"
    output.parent.mkdir(parents=True, exist_ok=True)
    executable = {"miyoosdr", "launch.sh", "research/miyoo-pcap",
                  "research/monitor-capture.sh", "research/inspect-capture.py"}
    payloads = {name: (ROOT / path).read_bytes() for name, path in files.items()}
    payloads["BUILD.txt"] = build_info.encode()
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, payload in sorted(payloads.items()):
            entry = zipfile.ZipInfo(f"App/MiYooSDR/{name}", date_time=timestamp)
            entry.create_system = 3
            mode = 0o755 if name in executable else 0o644
            entry.external_attr = (stat.S_IFREG | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(entry, payload)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    checksum = output.with_suffix(".zip.sha256")
    checksum.write_text(f"{digest}  {output.name}\n")
    print(f"Built {output} ({output.stat().st_size:,} bytes)")
    print(f"SHA-256 {digest}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?", default="0.5.0")
    package(parser.parse_args().version)
