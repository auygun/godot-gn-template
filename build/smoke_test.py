#!/usr/bin/env python3
"""Headless smoke test (the smoke_test action).

Runs the main scene headless for N frames; fails on any "ERROR:" line or a
non-zero exit. Not in the default build: `ninja -C out/host smoke_test`.
The action never writes its output, so it always runs.

Usage: smoke_test.py <engine_binary> [frames]
"""

import os
import subprocess
import sys


def main():
    engine_binary = sys.argv[1]
    frames = sys.argv[2] if len(sys.argv) > 2 else "60"
    # This script lives in <project>/build/ — derive the project root.
    srcdir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    proc = subprocess.run(
        [engine_binary, "--headless", "--path", srcdir, "--quit-after", frames],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors="replace")
    sys.stdout.write(proc.stdout)

    errors = [l for l in proc.stdout.splitlines() if "ERROR:" in l]
    if errors or proc.returncode != 0:
        print("smoke_test: FAILED — exit code %d, %d ERROR line(s)"
              % (proc.returncode, len(errors)), file=sys.stderr)
        for l in errors:
            print("  " + l, file=sys.stderr)
        return 1

    print("smoke_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
