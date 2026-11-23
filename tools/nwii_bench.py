#!/usr/bin/env python3
"""Benchmark translated-code throughput against a rebuilt export.

Metric: wall time from process start to a guest milestone, which is
compute-bound and therefore moves when the translator does. Steady-state fps and
MIPS are NOT usable here - the guest is VI-capped at 60 fps and the emulator
thread busy-waits, so a faster translator only makes the idle spin longer (MIPS
goes up) while fps stays pegged.

    tools/nwii_bench.py                     # default 5 runs, boot milestone
    tools/nwii_bench.py -n 9               # more samples
    tools/nwii_bench.py --milestone frames:600
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "games", "rayman_origins", "export", "build")
BINARY = os.path.join(BUILD, "RaymanOrigins")
DATA = os.path.join(ROOT, "games", "rayman_origins", "data")


def run_once(milestone, limit):
    env = dict(os.environ)
    env.setdefault("NWII_LOG_LEVEL", "warn")
    started = time.time()
    proc = subprocess.Popen(
        [BINARY, DATA], cwd=BUILD,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env,
    )
    reached = None
    interest = re.compile(r"frame\.count=(\d+)|Initial loading complete")
    try:
        for line in proc.stdout:
            now = time.time() - started
            if now > limit:
                break
            if reached is not None:
                continue
            if milestone == "boot":
                if "Initial loading complete" in line:
                    reached = now
            else:
                match = interest.search(line)
                if match and match.group(1):
                    if int(match.group(1)) >= milestone:
                        reached = now
    finally:
        proc.kill()
        proc.wait()
    return reached


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-n", "--runs", type=int, default=5)
    parser.add_argument("--milestone", default="boot",
                        help="'boot' or frames:<N>")
    parser.add_argument("--limit", type=float, default=20.0,
                        help="per-run wall-clock cap, seconds")
    args = parser.parse_args()

    milestone = args.milestone
    if milestone != "boot":
        if not milestone.startswith("frames:"):
            raise SystemExit("--milestone takes 'boot' or frames:<N>")
        milestone = int(milestone.split(":", 1)[1])

    if not os.path.exists(BINARY):
        raise SystemExit("build the export first: %s" % BINARY)

    samples = []
    for index in range(args.runs):
        got = run_once(milestone, args.limit)
        if got is None:
            print("  run %d: milestone not reached within %.0fs"
                  % (index + 1, args.limit), file=sys.stderr)
            continue
        samples.append(got)
        print("  run %d: %.3fs" % (index + 1, got))

    if not samples:
        raise SystemExit("no samples")

    mean = statistics.mean(samples)
    spread = (max(samples) - min(samples)) / mean * 100.0
    print("\n%s: mean %.3fs, min %.3fs, max %.3fs, spread %.1f%% (n=%d)"
          % (milestone, mean, min(samples), max(samples), spread, len(samples)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
