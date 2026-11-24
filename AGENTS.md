# Agent notes — wii-lie

How to build NWiiRecomp, get a binary onto a test bed, measure it, and debug what
the GX path silently discards.

**`ISSUES.md` is the ledger of open work — read it before starting and update it
as you go.** Ids are stable `I-nnn`. It is git-ignored, so it will not appear in
`git status`; it exists in the working tree. What the runtime supports today is
in `README.md`.

## What this repo is

A static recompiler from GameCube/Wii PowerPC executables to native C++. Three
parts: `nWiiAnalyzer` (DOL/ELF parser), `nWiiRecomp` (PPC → C++), `nWiiRuntime`
(HLE runtime, rasterizer, shader generator). The recompiler is a host tool; a
generated `export/` is the target artifact.

This fork targets **Linux ARM64 (aarch64)** and **Rayman Origins** (SOJE41), the
one game configured and validated here.

## Build

```sh
tools/ubuntu-sysroot.py      # once: the aarch64 sysroot from Ubuntu noble
tools/zig-toolchain.sh arm64 # once: zig wrappers in build/
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/nWiiRecomp/nwiirecomp recomp_config_rayman.toml   # writes export/
cd export && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

Always build `Release`: generated code is only usable with optimisation on.
`export/` is self-contained and builds without this repository.

Cross builds link the target's SDL2 from `build/sysroot-arm64` via
`cmake/toolchains/linux-arm64-zig.cmake` (`-DNWII_SYSTEM_SDL2=ON`), pinned by
`tools/sysroots/arm64.lock`; `tools/ubuntu-sysroot.py --verify` checks the tree
against the lock. Do not set `PKG_CONFIG_SYSROOT_DIR` — zig applies `--sysroot`
to `-L` itself, so prefixing searches the sysroot twice. The suite must match the
target image: a trixie sysroot ships FFmpeg 7.1 (`libavcodec.so.61`) where noble
has 6.1 (`.so.60`), so the binary will not load on the board.

zig emits DWARF even at `-O3`, which turns a ~160 MB release executable into
~800 MB; `-g0` is applied by default, and `-DNWII_ARM64_DEBUG_INFO=ON` restores
symbols for on-target debugging.

## Working rules

- **Delete `export/` (including its `build/`) before regenerating**, so nothing
  stale survives. Generated sources under `games/*/export/` are gitignored.
- **Copy an edited runtime file into `games/*/export/` before cross-building**,
  or the build silently uses the previous version. Verify with `diff -q`.
- **Keep the VM disk lean.** `~/nwii-test/RaymanOrigins` and `run.sh` are the only
  permanent artifacts. Stage every other build to `~/nwii-test/scratch/` and
  delete it when the comparison is done — including `/tmp` copies, which share
  the overlay filesystem. Keep at most one fallback (`RaymanOrigins.prev`).
  A release binary is ~170 MB and the disk has been filled once; check
  `df -h /` first.
- **Measure the game menu**, and verify the menu with frame captures rather than
  inferring it from elapsed time or draw counts. Align samples to the captured
  interval.
- **A/B must compare binaries built from the same tree.** Diffing against an older
  staged binary, or `cp`-ing over a running one (`Text file busy` leaves the old
  binary in place), both produce confident, meaningless numbers. Run distinct
  paths and `md5sum` the pair. Identical binaries have also diverged run to run,
  so an A/A control is worth the wall clock.
- **Never present M4 numbers as if they transferred to the A53.** An in-order,
  3-4x narrower core turns a free change into the dominant one — the I-037
  `game_id` comparison was 0.01% on the Mac and 9.43% on the board. Say plainly
  that a change is unmeasured while the board is offline.

## Test beds

Two targets that answer different questions. Both deploy to `~/nwii-test` and run
`./run.sh` there (it takes the Wayland socket, audio and input; it exits by
itself only with `NWII_MAX_SECONDS`).

| | `vm` (primary) | `opiz3` (reference low-end) |
| :--- | :--- | :--- |
| Access | `ssh vm` (`127.0.0.1:2222`) | `ssh opiz3` |
| CPU | 4x Apple M4 under HVF | 4x Cortex-A53, in-order, 480-1512 MHz |
| RAM | 961 MiB | 1986 MiB + zram |
| OS | Ubuntu 22.04.5, kernel 5.15.0-203 | Ubuntu 24.04.5, kernel 6.1.31-sun50iw9 |
| GPU | virtio-gpu, no virgl | Mali-G31 Bifrost (panfrost) |
| Renderer | llvmpipe | llvmpipe via Weston; panfrost via `offscreen` |

Both run **Weston kiosk on tty1** as a boot service, so the game is a Wayland
client through the compositor's EGL/GLES stack — the path a consumer device
takes, not a direct SDL window. `journalctl -u weston-kiosk -f` follows it.

- **VM display size is fixed at 640x480** (`weston.ini` `Virtual-1`, matching
  `-device virtio-gpu-pci,xres=640,yres=480`). A larger CRTC is scaled to 50%
  inside that window and looks half-size. Check the mode in
  `/sys/kernel/debug/dri/0/state`, not `/sys/class/graphics/fb0/virtual_size`
  (the boot console, which never follows the scanout).
- **The board's Weston is on the VNC backend, which composites in Pixman**, so
  the `wayland-kiosk` socket only offers llvmpipe and all GX rasterization runs
  on the A53s. The Mali-G31 is functional (`eglinfo` on GBM/surfaceless) but no
  GPU-capable Wayland surface is exported; `MESA_LOADER_DRIVER_OVERRIDE` changes
  nothing. `kmsdrm` is unavailable in this SDL2 build. Samples under
  `llvmpipe-*`, `libgallium`, `[JIT] tid` and `libEGL_mesa` are software
  rasterization, not guest CPU.
- **`SDL_VIDEODRIVER=offscreen ./run.sh` is the only way to reach the real
  Mali-G31.** It creates an EGL context on the device and presents to a pbuffer,
  so there is no visible output — measurement only. It renders for real
  (`efb_nonblack=307196/307200`; `SDL_VIDEO_OFFSCREEN_SAVE_FRAMES=1` writes
  frames) at 1024x768.

### Profiling

`NWII_PERF=1 NWII_PERF_LOG=<csv>` is the portable instrument: it needs no
privileges and works on both beds, one row per `NWII_PERF_INTERVAL_MS`
(default 1000, so one row per second). `frames` is the count in *that period*,
not a running total. `NWII_PERF=2` is
the on-screen overlay. The column order is the header the runtime writes
(`nWiiRuntime/src/core/perf_overlay.cpp`); the ones that matter most are `fps`,
`p99_ms`, `mips`, `parse_ms`, `render_ms`, `tex_up`, `shaders`, and the tails
`hdr` (`fifo.headerless_vertices`), `dl` (`dl.rejected`) and `bad`
(`fifo.bad_continuation`).

`perf` differs per bed:

- **VM:** the hardware PMU is not exposed under HVF (`perf stat -e cycles` says
  `<not supported>`), so sample `cpu-clock`. `perf_event_paranoid=4` blocks
  unprivileged recording, so it needs `sudo` with the Wayland environment passed
  explicitly (sudo resets `XDG_RUNTIME_DIR`):

  ```sh
  ssh vm 'cd ~/nwii-test && sudo env XDG_RUNTIME_DIR=/run/user/1000 \
    WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland \
    NWII_AUTOADVANCE=1 NWII_MAX_SECONDS=25 \
    perf record -e cpu-clock -o /tmp/p.data -- ./RaymanOrigins ./game ./config_gpu.toml'
  ssh vm 'sudo perf report -i /tmp/p.data --stdio -g none --sort symbol'
  ```

  Under HVF, hypervisor-exposed instructions are weighted far above bare metal:
  `mrs cntvct_el0` looked like 42% of `process_pending_callbacks` self time, but
  `NWII_STEADYTB=1` measured *slower*, so that attribution is an artifact.
  Confirm with an A/B.

- **Board:** `perf` is not preinstalled and `/usr/bin/perf` refuses the vendor
  kernel. Use the absolute path
  (`/usr/lib/linux-tools/6.8.0-149-generic/perf`), which takes `-F 299 -g` and
  records unprivileged — do not wrap it in `sudo`.

Baselines (Orange Pi Zero 3, llvmpipe, 640x480, vsync on, steady state):

| Config | fps | guest MIPS | parse_ms | render_ms |
| :--- | :--- | :--- | :--- | :--- |
| `LP_NUM_THREADS=1` | 15.6 | 7.2 | 13.9 | 20.2 |
| `LP_NUM_THREADS=2` | 27.5 | 8.1 | 25.6 | 36.4 |
| `LP_NUM_THREADS=4` (default) | 33.7 | 8.5 | 31.1 | 44.9 |
| `LP_NUM_THREADS=8` | 33.7 | 8.4 | 31.2 | 46.6 |

The same scene on the Mali-G31 via `offscreen`: 59.9 fps, 9.6 guest MIPS. The win
is guest frames, not MIPS — software rasterization was costing about half the
total CPU. Over 70% of the guest thread is runtime bookkeeping, the largest item
being the timebase read; 37.2% of guest PC samples land on the idle spin
`0x80006CCC` (`ISSUES.md` I-029).

### Measuring a change to the hot path

`fps` and average `mips` are **not** valid metrics, for a translator change or a
runtime one, and they mislead in the same direction. The guest is VI-capped at
60 Hz and the emulator thread busy-waits at 100 % of a core, so making work
cheaper just makes the idle spin longer: `fps` stays pegged and `mips` goes
**up**. I-029 is the worked example - collapsing the poll body from 2769 to 20
samples per profile moved `mips` 174.8 -> 251.0 (+44 %) and CPU time by 0.0 %.
Neither number alone is ever the answer.

Measure the thing the change is supposed to move:

- **Guest throughput** - `tools/nwii_menu_bench.py`, wall time to a fixed guest
  frame count on the save-file menu (below 60 fps, 250-360 guest MIPS).
  `tools/nwii_bench.py` does the same for the boot milestone. Both report spread
  and take `-n` so an A/B can interleave baseline and candidate to cancel drift.
- **Host CPU** - `/usr/bin/time -p` (or `time`) around a fixed
  `NWII_MAX_SECONDS` window. This is the only metric that can see a change to
  work that is hidden behind the spin.

Both, or the result is not evidence.

**Verify the screen, do not assume it.** `NWII_SCREENSHOT=<prefix>
NWII_SHOT_EVERY=<n>` writes BMPs (frames are numbered globally, and
`NWII_SHOT_FROM`/`NWII_SHOT_TO` bound the range). Read the captures: the
save-file menu shows "Select a file" with save slots. Both tools use the
sequence `NWII_AUTOPRESS=240:0x0800:6,2000:0x0800:6,2400:0x0100:6`, which is
verified to reach it. Note the menu is reached around frame 3300: a benchmark
target of 2400 measures the stretch *before* it and will show no difference
between two binaries that differ only on that screen.

## Diagnostics

The runtime reports what it *discards*. Every GX bug so far was silent, so a
"slow" or "wrong picture" run is usually dropped work.

```sh
NWII_DIAG=info ./RaymanOrigins <game>     # rate-limited detail lines
NWII_STRICT=1 ./RaymanOrigins <game>      # exit 3 if any error-class counter fired
tools/nwii_run.sh 20 /tmp/run.log ./RaymanOrigins <game>   # bounded, summary on stderr
```

`tools/nwii_run.sh` always kills with SIGKILL: the guest can leave `SIGALRM`
undelivered, so `alarm(2)`/`timeout(1)` are not reliable bounds.

`fifo.bad_continuation` is the invariant to stare at: it fires when an accepted
command's payload does not end on another command, which names the culprit:

```
[DIAG] fifo.bad_continuation x3: cmd 0x90 at 0x1423 sized 8451 -> next byte 0xFF is not a command
```

## Rendering traps

Each of these cost real debugging time:

- **The strap/safety screen is video-gated, not input-gated.** `NWII_AUTOPRESS`
  does not advance it; the prologue movie has to complete or fail.
  `NWII_SKIP_VIDEOS` was removed — it never worked.
- **The menu phase streams vertices with no draw header** (emitter `0x8052DAB4`).
  Unhandled, the parser reads a float mantissa as an opcode and the resync
  destroys the frame. `fifo.headerless_vertices` counts them and the parser
  absorbs them as a unit; `NWII_NO_BLOCK_RECOVERY=1` disables that to measure it.
- **A VAT/VCD disagreement silently drops a draw.** The engine streams 1-byte
  index triples under a VCD that claims 16 bytes/vertex, so the payload looks
  incomplete. The parser retries shorter strides and keeps the one whose
  continuation parses best (`NWII_STRIDEDBG=1`); a wrong-looking draw is more
  often a stride choice than a missing command. Ruled out here: the CP FIFO ring
  (`NWII_CPSTAT=1` shows `wp == rp == base`), off-page WGP writes
  (`NWII_WGP_DROP4=1` doubles the errors — they are FIFO data), and a missing VCD
  write.
- **A black panel with a healthy fps counter means every draw is dropped.** Watch
  `dl.rejected` and `shader.link_failed`; the frame is still presented, so the
  loss is upstream of it.
- **Textures are bound per draw, not per batch.** Binding at batch open made
  every later draw sample the first draw's texture. `NWII_NOTEX=1` leaves every
  unit on texture 0 and distinguishes "white because of the texture" from any
  other cause.
- **Texture wrap comes from the BP tile descriptor** (TexMode0, BP 0x80-0x87),
  not the GL default. The game asks for repeat on ~2000 draws and clamp on 146,
  so a tiled look is usually what it asked for.
- **`tools/doldis.py <main.dol> <start> <end> [--hex]`** disassembles by address,
  section-aware. A plain offset only works in the first DOL text section, and
  capstone stops at paired-single opcodes, so `--hex` is the fallback there.

## Gotchas

- Headless (`NWII_HEADLESS=1`) has its own loop, ignores `NWII_MAX_SECONDS` and
  never reaches the CSV sampler. Run it under `timeout -s KILL`.
- A cold cache (`sudo sysctl vm.drop_caches=3`) shifts boot timings; compare like
  with like.
- No screenshot protocol in the VM's Weston build, so verification is visual on
  the qemu window or via the CSV counters.
- `[graphics] backend = "OpenGL"` uses GLX; `"OpenGLES"` uses EGL. KMSDRM needs
  `SDL_VIDEODRIVER=kmsdrm` plus `video`/`render` group access and a free VT.
