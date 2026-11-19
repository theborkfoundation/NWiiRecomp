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

The low-end performance target is an **Orange Pi Zero 3** (`ssh opiz3`,
aarch64 Cortex-A53, Ubuntu 24.04, Mali-G31). The game is deployed at
`~/nwii-test`; `./run.sh` there launches it on the Weston `wayland-kiosk`
socket.

**Read [TESTBED.md](./TESTBED.md) before running or profiling anything on the
board.** It records the hardware, the display path (Weston VNC/Pixman, so
llvmpipe software rasterization — *not* the Mali GPU), the deploy/cross-build
steps, the working `perf` invocation, and the current baseline fps/MIPS table.

To profile the real Mali-G31, use `SDL_VIDEODRIVER=offscreen ./run.sh`: SDL
then creates an EGL context straight on the device and the game runs at 60 fps
instead of ~34 fps on llvmpipe. That path is measurement-only (no visible
output), and the llvmpipe path is what interactive play uses through
`wayland-kiosk`.

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
  Silicon.
