# Test bed: Orange Pi Zero 3 (aarch64)

The reference low-end ARM64 target for the Linux port. Everything here was
verified live over SSH on 2026-11-19; treat it as the source of truth when
comparing measurements between the Mac and the ARM board.

Host alias: `opiz3` (`ssh opiz3`). Game tree: `~/nwii-test`.

## Hardware and OS

| Item | Value |
| :--- | :--- |
| Board | Orange Pi Zero 3 (Allwinner H618, `xunlong,orangepi-zero3`, `allwinner,sun50i-h616`) |
| CPU | 4x Cortex-A53 (r0p4), 1 cluster, 480 MHz .. 1512 MHz |
| Governor | `ondemand` (also available: `conservative`, `userspace`, `powersave`, `performance`, `schedutil`) |
| RAM | 1986 MiB total, ~530 MiB free, 993 MiB zram swap |
| Storage | 14.8 GB microSD (`/dev/mmcblk0p1`), ~7.8 GB free |
| OS | Ubuntu 24.04.5 LTS (arm64), glibc 2.39 |
| Kernel | `6.1.31-sun50iw9` (vendor kernel, **not** an Ubuntu linux-tools kernel) |
| GPU | Mali-G31 Bifrost, `panfrost` DRM driver loaded (`renderD128`), DRM `sun4i-drm` for scanout |
| Compiler on target | gcc 13.3.0 (native builds possible, but the cross build is the normal path) |
| SDL2 | system 2.30.0 |

Thermals before a run are ~49-50 C on all four zones (`gpu`, `ve`, `cpu`,
`ddr`), so the A53s are not thermally throttled at the start of a measurement
and hold 1512 MHz under load. Long runs on a bare board will heat up; re-check
`scaling_cur_freq` and the thermal zones if a number looks anomalous.

## Display path (this matters for profiling)

The board is driven headless by a **Weston kiosk compositor on the VNC
backend**, and Weston selects the **Pixman** (CPU) renderer:

```
weston --backend=vnc --width=640 --height=480 --port=5901 --shell=kiosk \
       --socket=wayland-kiosk --config=/home/bachnx/.config/weston.ini ...
[weston log] Using Pixman renderer
```

Consequences, all confirmed on the board:

- Socket: Wayland display `wayland-kiosk` under `XDG_RUNTIME_DIR=/run/user/1001`.
- The Mali-G31 **is** functional: `eglinfo` shows
  `OpenGL ES renderer: Mali-G31 (Panfrost)` on the `GBM` and `surfaceless`
  EGL platforms, GLES 3.1 / GL 3.1.
- Over the `wayland-kiosk` socket the EGL **Wayland platform only offers
  llvmpipe**, and `eglinfo` reports `Wayland platform: eglInitialize failed`
  for the X11/LIBDECOR case. Because Weston composites in Pixman it exports no
  GPU-capable Wayland EGL surface, so any client presenting through this socket
  falls back to software rasterization.
- Therefore the game logs `[GL] llvmpipe (LLVM 20.1.2, 128 bits) | OpenGL ES
  3.2` and all GX rasterization runs on the A53 cores via llvmpipe's LLVM JIT.
- `MESA_LOADER_DRIVER_OVERRIDE=panfrost|zink` makes **no** difference; the
  override does not change what Weston can export. It is not a usable knob.
- The HDMI output is **disconnected**, which is why Weston is on VNC at all.
  The VNC backend composites in Pixman by design, so there is no way to get a
  GPU surface out of this Weston instance. Do not chase `weston.ini` renderer
  settings for it.
- `SDL_VIDEODRIVER=kmsdrm` is not available: this SDL2 build only has
  `wayland`, `offscreen`, `x11`(absent) and `dummy` drivers (`strings
  libSDL2-2.0.so.0`), and the connector is unplugged anyway.

Profiling implication: samples attributed to `llvmpipe-0..3`, `libgallium`,
`[JIT] tid <n>` and `libEGL_mesa` belong to **software rasterization**, not to
guest CPU emulation. Do not read those as emulator cost.

### Reaching the real GPU: `SDL_VIDEODRIVER=offscreen`

The Composit**or** is the blocker, not the graphics** stack**: SDL's `offscreen`
video driver creates an EGL context directly on the device, bypassing Wayland
entirely. This works, and it is the way to measure the Mali-G31:

```sh
cd ~/nwii-test
SDL_VIDEODRIVER=offscreen ./run.sh                 # Mali-G31, no window
SDL_VIDEODRIVER=offscreen NWII_MAX_SECONDS=30 \
  $P record -F 299 -g -o /tmp/gpu.data -- ./run.sh
```

It logs:

```
[GL] display 0: 1024x768 fmt=SDL_PIXELFORMAT_RGB888
[GL] Mali-G31 (Panfrost) | OpenGL ES 3.1 Mesa 25.2.8-0ubuntu0.24.04.4
[GL] vendor=Mesa glsl=OpenGL ES GLSL ES 3.10 requested=OpenGLES context=3.0
```

Verified that it really renders (not a black/no-op surface): `NWII_STATS=1`
reports `efb_nonblack=307196/307200` at 10 s and a compiled shader, and
`SDL_VIDEO_OFFSCREEN_SAVE_FRAMES=1` writes real frames. The default window is
1024x768 here instead of 640x480; pass nothing special, the game is 4:3 either
way.

Caveat: offscreen presents to a pbuffer, so the picture is not visible on a
monitor or over VNC. It is a **measurement** path, not a play path. For
interactive play stay on `wayland-kiosk` (llvmpipe) or attach an HDMI display
and run a `drm-backend` Weston with `gl-renderer` (untested here).

## Deployed layout (`~/nwii-test`)

```
nwii-test/
├── RaymanOrigins          168 MiB aarch64 ELF, NOT stripped (has .debug_* + .symtab)
├── game/                  extracted SOJE41 tree (disc, files, sys, h3.bin, cert/ticket/tmd.bin)
├── assets/games/SOJE41.toml   per-game quirks profile (linear_i8, chroma_bias_i8)
├── config_gpu.toml        [graphics] backend = "OpenGLES"
├── run.sh                 launcher (sets env, execs the game)
├── start.sh               pkill + setsid launcher, logs to run-now.log
├── perf-bench.csv         previous NWII_PERF_LOG capture
├── run-now.log / .pid     output of the last start.sh
└── run-bench.log          a prior instrumented run
```

`run.sh` is the canonical launcher:

```sh
#!/bin/sh
cd "$(dirname "$0")" || exit 1
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-kiosk}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"
exec ./RaymanOrigins game config_gpu.toml "$@"
```

Run the game with `./run.sh` from `~/nwii-test`. It takes the full Wayland
socket, audio (PulseAudio, 32 kHz) and controller input. The main loop exits by
itself only with `NWII_MAX_SECONDS`; otherwise close the window or kill it.

The deployed binary currently differs from
`games/rayman_origins/export/build-arm64/RaymanOrigins` (`md5sum`
`22712ab1…` on the board vs `72b1d557…` locally). Both are 168-175 MB release
builds with DWARF + symbols; the board copy is the one that matters for
profiling, and its build IDs/symbols must be present for `perf` to symbolize.

## Cross build and deploy

Standard flow from the repo root (see `docs/linux-arm64-plan.md`):

```sh
tools/ubuntu-sysroot.py            # build/sysroot-arm64
tools/zig-toolchain.sh arm64       # build/zig-aarch64-{cc,cxx,ar,ranlib}
cd games/rayman_origins/export
cmake -B build-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=../../../../cmake/toolchains/linux-arm64-zig.cmake \
  -DCMAKE_BUILD_TYPE=Release -DNWII_SYSTEM_SDL2=ON -DNWII_ENABLE_GLES=ON
cmake --build build-arm64 -j
scp build-arm64/RaymanOrigins opiz3:~/nwii-test/RaymanOrigins
```

Release defaults: `-O3 -DNDEBUG -g0`. Pass `-DNWII_ARM64_DEBUG_INFO=ON` when
target symbols are needed, then strip for deployment if size matters. A release
binary is ~160-175 MB.

### Native Bink decoder in a cross build

The sysroot now includes FFmpeg: `tools/ubuntu-sysroot.py` seeds
`libavcodec-dev` and `libavutil-dev`, and the lock records the resolved
closure. Because the script targets the same archive the board runs (Ubuntu
noble via ports.ubuntu.com), the sonames match - `libavcodec.so.60` and
`libavutil.so.58`, the same ones `/usr/lib/aarch64-linux-gnu` provides on the
Pi. No hand-copied libraries are needed.

```sh
tools/ubuntu-sysroot.py            # build/sysroot-arm64, includes FFmpeg headers
tools/zig-toolchain.sh arm64
```

Configure the export as usual; the runtime's CMake finds the headers and
libraries in the sysroot and defines `NWII_HAVE_BINK_DECODER`. To check
playback, run
`SDL_VIDEODRIVER=offscreen NWII_AUTOADVANCE=1 NWII_MAX_SECONDS=45 ./run.sh`.
The first decoded frame logs `[Bink] native decoder: 640x456`. For an A/B
comparison, add `NWII_NATIVE_BINK=0` to use the guest decoder.

If a build is ever pointed at a different suite, keep it on the board's release.
Debian trixie ships FFmpeg 7.1 (`libavcodec.so.61`), which the board cannot
load; the script takes `--mirror` and `--suite` when targeting something else.

## Profiling toolbox on the target

`perf` is **not** preinstalled and the `perf` wrapper refuses to run because the
vendor kernel is `6.1.31-sun50iw9`. Installed once via:

```sh
sudo apt-get install -y --no-install-recommends \
    linux-tools-common linux-tools-6.8.0-149-generic
```

Use the absolute binary path; the `/usr/bin/perf` wrapper will keep failing:

```sh
P=/usr/lib/linux-tools/6.8.0-149-generic/perf   # perf version 6.8.12
```

- `perf_event_paranoid` is `2`, but **unprivileged** `record`/`stat` work (the
  events are `cycles:Pu`, user-space only). Prefer running unprivileged.
- **Do not wrap the game in `sudo`.** `sudo` resets `XDG_RUNTIME_DIR` to
  `/run/user/0`, which hides `wayland-kiosk`, and the run dies with
  `SDL_Init failed: wayland not available`. This was the first profiling
  mistake here.
- `gdb` and `strace` are present. There is no `ltrace`, `valgrind` or
  `hotspot`. `-g0` release binaries still symbolicate through `.symtab`.

Canonical profile (unprivileged, game keeps its own Wayland env):

```sh
cd ~/nwii-test
$P record -F 299 -g -o /tmp/perf.data -- env NWII_MAX_SECONDS=20 ./run.sh
$P report -i /tmp/perf.data --stdio -g none --sort comm      # who burns CPU
$P report -i /tmp/perf.data --stdio -g none --sort symbol    # which symbols
```

Per-thread CPU without perf (useful to confirm saturation):

```sh
cd ~/nwii-test
NWII_MAX_SECONDS=26 ./run.sh >/tmp/x.log 2>&1 & P=$!
sleep 12; snap() { for t in /proc/$P/task/*; do c=$(cat $t/comm|tr -d ' '); \
  read -r _ _ _ _ _ _ _ _ _ _ _ _ _ u s _ < $t/stat; echo "$(basename $t)|$c|$u|$s"; done; }
snap >/tmp/g1; sleep 8; snap >/tmp/g2
# delta of u+s ticks / 8s => % of one core per thread
```

Runtime instrumentation relevant to perf work:

- `NWII_PERF_LOG=<csv>` — CSV sampler with no overlay (least perturbing):
  `fps,mips,parse_ms,render_ms,…`, column meanings in `README.md`.
- `NWII_PERF=1|full` — same numbers as a bitmap overlay (perturbs: drawn before
  the swap).
- `NWII_PERF_INTERVAL_MS` — 100..60000, default 1000.
- `NWII_STATS=1` — periodic `[STAT]` EFB readback; off by default, 1.2 MB/call.
- `NWII_NO_VSYNC=1`, `NWII_HEADLESS=1`, `NWII_SAMPLE=1`, `NWII_TRACE_CALLS=1`,
  `NWII_MAX_SECONDS`.
- Guessing a rasterizer knob is available: `LP_NUM_THREADS` (llvmpipe thread
  count) does scale this workload, confirming the rasterizer is on the critical
  path.

## Baseline numbers (llvmpipe, 640x480, ES 3.2, vsync on)

Steady state, windows 15+ of a `NWII_MAX_SECONDS=22` run, averaged:

| Config | fps | guest MIPS | parse_ms | render_ms |
| :--- | :--- | :--- | :--- | :--- |
| `LP_NUM_THREADS=1` | 15.6 | 7.2 | 13.9 | 20.2 |
| `LP_NUM_THREADS=2` | 27.5 | 8.1 | 25.6 | 36.4 |
| `LP_NUM_THREADS=4` (default) | 33.7 | 8.5 | 31.1 | 44.9 |
| `LP_NUM_THREADS=8` | 33.7 | 8.4 | 31.2 | 46.6 |

Notes:

- Boot/menu windows reach ~47-57 fps; heavy in-game scenes settle at ~34 fps.
- `NWII_NO_VSYNC=1` does not help (43.8 vs 42.7 fps in a short run): the
  present is not the limiter, the rasterizer is.
- Guest throughput is ~8.5 guest MIPS steady state, and it is **flat** across
  rasterizer thread counts while fps nearly doubles — the wall is not guest
  emulation at low thread counts.
- Per-thread CPU during gameplay: the emulator thread pins one A53 at ~102 %,
  the 4 llvmpipe workers take ~52 % each, the SDL audio thread ~19 %.
- Whole-run `perf stat`: 84.8 G cycles for 60.4 G instructions (~0.71 IPC) over
  22.4 s; `branch-misses` 163 M, `cache-misses` 99.5 M.

## Baseline numbers (Mali-G31, `SDL_VIDEODRIVER=offscreen`)

Steady state, windows 15+, `NWII_MAX_SECONDS=20..40`:

| Config | fps | guest MIPS | parse_ms | render_ms |
| :--- | :--- | :--- | :--- | :--- |
| default (vsync) | 59.9 | 9.6 | 39 | 94-120 |
| `NWII_NO_VSYNC=1` | 59.9 | 9.7 | 47 | 120 |
| `NWII_HIDDEN_WINDOW=1` | 59.9 | 9.7 | - | - |

Long run (40 s, 38 windows): fps avg 58.8, **min 43.6**, p99 23.1 ms, max
present interval 51.1 ms; guest frames are a clean 60 per 1 s window
(`frames=60`) and `vi≈60`. The 43.6 fps / 51 ms outliers are scene transitions,
not steady-state stutter.

Same-scene A/B, identical `NWII_MAX_SECONDS=30` run on each path, counting only
windows that drained guest frames:

| Path | gameplay windows | avg guest frames/s | max frames/s | guest MIPS |
| :--- | :--- | :--- | :--- | :--- |
| llvmpipe (wayland-kiosk) | 23 | 31.0 | 35 | 10.8 |
| Mali-G31 (`offscreen`) | 24 | **52.3** | **60** | 12.6 |

The win is **guest frames per second (1.7x), not guest MIPS**: it renders 60
guest frames/s instead of 35, so animation and game time run at the intended
rate. Total process CPU drops to a single core (`RaymanOrigins` 100 %, second
thread 17 %, everything else ~1-2 %) with no rasterizer threads at all — `perf`
attributes 99.24 % of samples to the game process, against 40 % on llvmpipe
where four llvmpipe workers took ~52 % of a core each. Software rasterization
was costing roughly half the total CPU and a third of the frame rate.
`NWII_NO_VSYNC` and `NWII_HIDDEN_WINDOW` change nothing: 60 fps is the guest's
own VI-limited target (~59.94 Hz), not a host cap.

One residual cost is unchanged by the display path: the idle spin is still
**37.2 % of guest PC samples on both paths** (753/2026 on llvmpipe, 733/1972 on
the GPU), and the poll rate is the same ~1.49 M `process_pending_callbacks`
calls/s. Fixing the idle-thread throttle would help both paths; fixing the
display path only removed the rasterizer. There are also occasional 43-49 fps
windows at scene transitions even on the GPU. Cold-cache (`drop_caches`) boot
timing is not measured above.

Sample attribution for a 30 s window (10 K samples, 299 Hz): by command
`RaymanOrigins` 99.24 %, `PulseMainloop` 0.39 %, `SDLAudioP2` 0.31 %.

Gameplay-only window (attached to a running process with
`perf record -p <pid>`, `timeout -s INT` so perf finalises the file; 17 K
samples). On the **llvmpipe** path the emulator thread is 31.6 % of all samples,
split as:

| Bucket | % of all | % of emulator thread |
| :--- | :--- | :--- |
| timebase (`clock_gettime` + `steady_clock::now` + `dec_expired`) | 8.95 | 28 |
| translated `func_*` guest code (439 distinct) | 8.79 | 28 |
| callback poll (`process_pending_callbacks`) | 6.09 | 19 |
| scheduler / IPC (`EventScheduler`, `ipc_tick`, `di_tick`) | 1.80 | 6 |
| GX FIFO parse (`cp_process_fifo`, `ProcessGXFifo`) | 1.69 | 5 |
| GL renderer CPU side (`RendererGL`, `TextureCache`) | 0.84 | 3 |

On the **Mali-G31** path (`SDL_VIDEODRIVER=offscreen`, 30 s whole-run window)
the emulator thread is 87.4 % of all samples:

| Bucket | % of all | % of emulator thread |
| :--- | :--- | :--- |
| translated `func_*` guest code | 25.07 | 29 |
| timebase (`clock_gettime` + `steady_clock::now` + `dec_expired`) | 22.30 | 26 |
| callback poll (`process_pending_callbacks`) | 16.76 | 19 |
| scheduler / IPC | 4.83 | 6 |
| GX FIFO parse | 3.56 | 4 |
| `TextureCache` | 3.42 | 4 |
| `RendererGL::Render` | 0.11 | 0 |

The *shape* is the same on both paths — ~70 % of the guest CPU thread is
runtime bookkeeping rather than translated guest code, with the timebase the
single largest item — but on the GPU path ~29 % more of the thread is real
guest work because the guest is actually running at 60 fps. Per-thread CPU on
the GPU path: `RaymanOrigins` main thread **100 %**, second thread 17 %,
audio/pulse ~1-2 %. `run_game` self-cost rises to 3.11 % because guest
execution, not HLE polling, now dominates.

So **over 70 % of the guest CPU thread is runtime bookkeeping, not translated
guest code**, and the single largest item is reading the timebase.

Guest PC distribution during gameplay (`NWII_SAMPLE=1`, one sample per 20,000
`process_pending_callbacks` calls, 2026 samples): **37.2 % land on the idle-spin
address `0x80006CCC`**. The other top PCs are `0x800962F4` (6 %), `0x800A25D0`
(6 %), `0x80071A24` (3 %). The idle thread is a bare `goto` loop in the
generated code (`func_80006CCC` in `output_1.cpp`) whose only per-iteration work
is `++ctx.inst_count` and `process_pending_callbacks`; it never blocks or
sleeps, so the A53 burns a core spinning when the guest has nothing to run.

Guest throughput also collapses at the menu/gameplay boundary: ~39 MIPS at
~25 fps while only the menu runs (~1.6 M guest instructions per present), then
~8.5 MIPS at ~34 fps once drawing starts (~250 K guest instructions per
present). So per-frame guest work *falls* ~6.5x as the scene gets heavier while
the emulator thread stays pinned at 100 % of a core: in gameplay the guest is
not compute-bound, the core is being burned on idle spinning and runtime
bookkeeping. ~40 MIPS is the best guest throughput observed on this board and
~8.5 MIPS is what gameplay actually achieves — i.e. ~21 % of peak.

### Measured poll rate (the dominant cost)

`NWII_SAMPLE=1` emits one sample per 20,000 `process_pending_callbacks` calls.
A 20 s gameplay run produced 1,484 samples, i.e. **~29.7 M poll cycles, ~1.5 M
`process_pending_callbacks` calls per second**.

`clock_gettime(CLOCK_MONOTONIC)` on this board costs **53 ns/call (18.9 M/s)**
(measured with a `-O2` microbenchmark; the vDSO path, ~80 cycles at 1512 MHz).
The non-throttled poll body reads the clock at least three times per iteration
(`read_timebase` in the `dec_is_due` test, `steady_clock::now()` for the VI
gate, `read_timebase` again in `EventScheduler::advance`), so:

```
1.5 M calls/s x ~159 ns of clock reads ~ 0.24 s of CPU per second ~ 24 % of a core
```

which matches the measured 28 % timebase share in the profile table above.
The throttle at `ios.cpp:1017` (`(ctx.inst_count - last_poll_inst) < 2048`)
exists precisely to avoid this, but it is guarded by
`ctx.pc != 0x80006ccc` — the idle spin is **excluded**, so the idle thread runs
the entire poll body (clock reads plus `ipc_tick`, `di_tick`,
`cp_process_fifo`, `EventScheduler::advance`) on every backward branch.

## Gotchas

- Headless (`NWII_HEADLESS=1`) has its own loop and ignores
  `NWII_MAX_SECONDS`; run it under `timeout -s KILL`. It also never reaches the
  perf CSV sampler, so it gives no `mips` numbers.
- The `[JIT] tid <n>` samples are llvmpipe's LLVM-generated rasterizer, split
  across the four llvmpipe worker threads. They are not the emulator.
- `start.sh` leaves a stale `run-now.pid`; check with `pgrep -x RaymanOrigins`
  before trusting it.
- A cold cache (`sudo sysctl vm.drop_caches=3`) shifts boot timings
  noticeably; compare like with like.
