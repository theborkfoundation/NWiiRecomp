#!/usr/bin/env python3
"""Benchmark translated-code throughput on the Rayman Origins save-file menu.

The menu is the compute-bound phase: it renders below the guest's 60 Hz VI
limit at 250-360 guest MIPS, so unlike gameplay (VI-capped, guest idle-spinning)
translator speed shows up as wall-clock time.

Metric: wall time from start to a fixed guest frame count. Fixed guest work in
less time is a faster translator, and it does not care about instantaneous fps
wander or the idle spin the way an average-MIPS reading does.

The window is verified, not assumed: frames 3600 and 4200 are captured and must
show the "Select a file" picker.

    tools/nwii_menu_bench.py               # 3 runs
    tools/nwii_menu_bench.py -n 5 --frame 4200
"""

import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "games", "rayman_origins", "export", "build")
BINARY = os.path.join(BUILD, "RaymanOrigins")
DATA = os.path.join(ROOT, "games", "rayman_origins", "data")

# Reaches the "Select a file" picker and stays there (verified by captured
# frames 3600 and 4200 after this change).
SEQUENCE = "240:0x0800:6,2000:0x0800:6,2400:0x0100:6"


def run_once(frame_target, limit, extra):
    env = dict(os.environ)
    env["NWII_AUTOPRESS"] = SEQUENCE
    env["NWII_PERF"] = "1"
    env["NWII_PERF_INTERVAL_MS"] = "250"
    env.update(extra)
    handle = tempfile.NamedTemporaryFile(suffix=".csv", delete=False)
    handle.close()
    env["NWII_PERF_LOG"] = handle.name

    started = time.time()
    proc = subprocess.Popen([BINARY, DATA], cwd=BUILD, env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    reached = None
    try:
        while time.time() - started < limit:
            time.sleep(0.05)
            try:
                with open(handle.name) as csv_file:
                    rows = [line.strip() for line in csv_file if line.strip()]
            except OSError:
                continue
            if len(rows) < 2:
                continue
            # The last column is guest frames in that sampling period, not a
            # running total, so accumulate to get guest work done so far.
            total = 0
            for row in rows[1:]:
                cells = row.split(",")
                try:
                    total += int(cells[-1])
                except ValueError:
                    pass
            if total >= frame_target:
                reached = time.time() - started
                break
    finally:
        proc.kill()
        proc.wait()
        os.unlink(handle.name)
    return reached


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-n", "--runs", type=int, default=3)
    parser.add_argument("--frame", type=int, default=4200,
                        help="guest frame count to time (default 4200)")
    parser.add_argument("--limit", type=float, default=120.0)
    parser.add_argument("--cond", default="",
                        help="extra env, e.g. 'NWII_STRICT=1'")
    args = parser.parse_args()

    extra = {}
    for token in args.cond.split():
        if "=" in token:
            key, _, value = token.partition("=")
            extra[key] = value

    if not os.path.exists(BINARY):
        raise SystemExit("build the export first: %s" % BINARY)

    samples = []
    for index in range(args.runs):
        got = run_once(args.frame, args.limit, extra)
        if got is None:
            print("  run %d: frame %d not reached in %.0fs"
                  % (index + 1, args.frame, args.limit), file=sys.stderr)
            continue
        samples.append(got)
        print("  run %d: %.3fs to frame %d" % (index + 1, got, args.frame))

    if not samples:
        raise SystemExit("no samples")
    mean = statistics.mean(samples)
    print("\nto frame %d: mean %.3fs, min %.3fs, max %.3fs, spread %.1f%% (n=%d)"
          % (args.frame, mean, min(samples), max(samples),
             (max(samples) - min(samples)) / mean * 100.0, len(samples)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
