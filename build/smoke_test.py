#!/usr/bin/env python3
"""Headless smoke test (the smoke_test action).

Runs the main scene headless for N frames offline, then a dedicated server
and a client over loopback; fails on any "ERROR:" line or a non-zero exit,
or if the client never spawns its player. Not in the default build:
`ninja -C out/host smoke_test`. The action never writes its output, so it
always runs.

Usage: smoke_test.py <engine_binary> [frames]
"""

import os
import socket
import subprocess
import sys
import time

# The client's spawn line (Main::SpawnPlayer), proof it got a snapshot.
SPAWNED = "Main: spawned local player"


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def godot(engine_binary, srcdir, frames, user_args=()):
    # --max-fps: --quit-after counts frames, and headless runs uncapped.
    args = [engine_binary, "--headless", "--path", srcdir, "--max-fps", "60",
            "--quit-after", str(frames)]
    if user_args:
        args += ["--"] + list(user_args)
    return subprocess.Popen(args, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            errors="replace")


def check(name, proc, output, expect=None):
    sys.stdout.write("--- %s\n%s" % (name, output))
    errors = [l for l in output.splitlines() if "ERROR:" in l]
    missing = expect is not None and expect not in output
    if errors or proc.returncode != 0 or missing:
        print("smoke_test: %s FAILED — exit code %d, %d ERROR line(s)%s"
              % (name, proc.returncode, len(errors),
                 ", no \"%s\"" % expect if missing else ""), file=sys.stderr)
        for l in errors:
            print("  " + l, file=sys.stderr)
        return False
    return True


def main():
    engine_binary = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    # This script lives in <project>/build/ — derive the project root.
    srcdir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    offline = godot(engine_binary, srcdir, frames)
    ok = check("offline", offline, offline.communicate()[0])

    # The server outlives the client, so the client leaves first (and the
    # server sees it go).
    port = str(free_udp_port())
    server = godot(engine_binary, srcdir, frames + 360, ["--server", port])
    time.sleep(0.5)
    client = godot(engine_binary, srcdir, frames + 180,
                   ["--join", "127.0.0.1", port])
    client_output = client.communicate()[0]
    server_output = server.communicate()[0]
    ok = check("server", server, server_output, "disconnected") and ok
    ok = check("client", client, client_output, SPAWNED) and ok

    if not ok:
        return 1
    print("smoke_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
