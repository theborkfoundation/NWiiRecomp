# Agent notes — wii-lie

Working notes for AI agents on this repository. Keep this file short; the
authoritative detail lives in the linked docs.

## What this repo is

NWiiRecomp — a static recompiler from GameCube/Wii PowerPC executables to
native C++. Four parts: `nWiiAnalyzer` (DOL/ELF parser), `nWiiRecomp`
(PPC → C++), `nWiiRuntime` (HLE runtime, rasterizer, shader generator),
`nWiiStudio` (ImGui debugger). The recompiler is a host tool; a generated
`export/` is the target artifact.

`README.md` is the feature reference, `BUG_REPORT.md` tracks findings by
`I-NNN` id, `PROGRESS.md` is the local Rayman Origins log (gitignored),
`PLAN.md` is the load-time optimisation write-up.

## Test bed

**The VM is the only working testbed as of 2026-11-21** (`ssh vm`): aarch64
Ubuntu 22.04 under QEMU/HVF on the Mac, with Weston running in kiosk mode as a
boot service on tty1. The game is deployed at `~/nwii-test`; `./run.sh` there
launches it as a Wayland client against the compositor, which is the same path
a consumer device takes (Wayland → compositor → EGL/GLES), not a direct SDL
window.

The **Orange Pi Zero 3** (`ssh opiz3`, Cortex-A53, Ubuntu 24.04, Mali-G31) is
the real low-end target, but it has been offline since 2026-11-21 and is not
reachable.

**Read [TESTBED.md](./TESTBED.md) before running or profiling anything.** It
records both machines' hardware, the Weston setup and the two unit-file traps
that break it, the deploy/cross-build steps, the working `perf` invocation, and
the baseline fps/MIPS tables.

Timings from the VM are **M4 timings, not A53 timings**. Use it to answer "did
I break it" and "did this get better at all"; it cannot tell you whether
something is fast enough on the class of hardware the port targets. A change
that looks free on the M4 can dominate on an in-order core — the `game_id`
comparison in the I-037 port was 0.01% on the Mac and 9.43% on the board. On
the board, `SDL_VIDEODRIVER=offscreen ./run.sh` profiles the real Mali-G31
(60 fps) instead of llvmpipe (~34 fps); that path has no visible output and is
measurement-only.

The VM has **no `perf`** and `kernel.perf_event_paranoid=4`, so there are no
symbol-level profiles there. The `NWII_PERF` / `NWII_PERF_LOG` CSV counters are
all the attribution available until the board is back.

### Staging builds on the VM

**`~/nwii-test/RaymanOrigins` and `run.sh` are the only permanent artifacts.**
Stage every other build to `~/nwii-test/scratch/` and delete it when the
comparison is done — including `/tmp` copies, which live on the same overlay
filesystem, not in RAM.

The VM disk is a fixed-size qcow2 and it has already been filled once: a dozen
kept debug binaries reached 2.0 GB of a 15 GB disk, leaving 130 MB free, at
which point even `scp` of a 170 MB test build failed. A release binary is
~170 MB, so a five-way A/B set costs ~1 GB before anything else. Reclaim with
`df -h /` before staging, and never leave a candidate binary behind.

Keep at most one fallback binary (`RaymanOrigins.prev`). Everything else is an
intermediate and belongs in `scratch/`.

## Working conventions

- Generated sources live under `games/*/export/` and are gitignored. Delete
  `export/` (including its `build/`) before regenerating so nothing stale
  survives.
- Measure with `-DCMAKE_BUILD_TYPE=Release`; see `README.md` for the
  `NWII_PERF` / `NWII_PERF_LOG` CSV sampler and what each column means.
- `PROGRESS.md`, `BUG_REPORT.md` and `games/` are gitignored local working
  notes and data; `PLAN.md`, `README.md` and `TESTBED.md` are tracked.
- When a runtime path looks hot, prefer measuring on the board over guessing:
  the A53's in-order cores change the picture completely compared with Apple
  Silicon. While it is offline, say plainly that a change is unmeasured rather
  than presenting M4 numbers as if they transferred.
- **A/B must compare binaries built from the same tree.** Both mistakes below
  produced confident, meaningless numbers: diffing against an older staged
  binary that predated the fixes under test, and `cp`-ing a candidate over a
  running executable, which fails with `Text file busy` and silently leaves the
  previous binary in place so both runs measure the same thing. Run distinct
  paths, and `md5sum` the two before trusting the result.
- Copy an edited runtime file into `games/*/export/` before cross-building, or
  the build silently uses the previous version. Verify with `diff -q`.
