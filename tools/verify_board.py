#!/usr/bin/env python3
"""Compare the translated V30 board against the reference interpreter.

Both libraries are built from the same sources with the same flags; the only
difference is which opcode dispatch the CPU executes. They must agree on every
rendered frame and every audio block, and on the checksum of the last frame.

Usage: verify_board.py BUILD_DIR CHIPS_DIR [FRAMES]
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PROBE = """
#include "zeroteam_board.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
"""


def build_probe(build: Path, project: Path, library: str, out: Path) -> None:
    subprocess.run([
        "g++", "-O2", "-std=gnu++17", "-I", str(project / "frontend"),
        str(project / "tools/probe.cpp"), "-L", str(build), f"-l:{library}",
        f"-Wl,-rpath,{build}", "-o", str(out),
    ], check=True)


def run(out: Path, chips: Path, frames: int, capture: Path) -> dict:
    proc = subprocess.run([str(out), str(chips), str(frames), str(capture)],
                          check=True, capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    match = re.search(r"video=([0-9a-f]{16}) audio=([0-9a-f]{16}) "
                      r"sites=(\d+) fallback=(\d+)", text)
    if not match:
        raise SystemExit(f"unexpected probe output:\n{text}")
    digest = hashlib.sha256(capture.read_bytes()).hexdigest()
    return {"video": match.group(1), "audio": match.group(2),
            "sites": int(match.group(3)), "fallback": int(match.group(4)),
            "capture": digest}


def main() -> int:
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    chips = Path(sys.argv[2] if len(sys.argv) > 2 else "private/chips").resolve()
    frames = int(sys.argv[3]) if len(sys.argv) > 3 else 1200
    project = Path(__file__).resolve().parent.parent

    ref_bin = build / "probe_ref"
    aot_bin = build / "probe_aot"
    build_probe(build, project, "libzeroteam_board_ref.so", ref_bin)
    build_probe(build, project, "libzeroteam_board.so", aot_bin)

    with tempfile.TemporaryDirectory() as tmp:
        ref = run(ref_bin, chips, frames, Path(tmp) / "ref.bmp")
        aot = run(aot_bin, chips, frames, Path(tmp) / "aot.bmp")

    report = {"frames": frames, "reference": ref, "translated": aot}
    ok = (ref["video"] == aot["video"] and ref["audio"] == aot["audio"]
          and ref["capture"] == aot["capture"] and aot["fallback"] == 0)
    print(f"frames={frames} video={ref['video']}/{aot['video']} "
          f"audio={ref['audio']}/{aot['audio']} "
          f"capture={ref['capture'][:16]}/{aot['capture'][:16]} "
          f"sites={aot['sites']} fallback={aot['fallback']} "
          f"=> {'identical' if ok else 'MISMATCH'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
