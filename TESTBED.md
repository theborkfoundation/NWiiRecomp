# Test beds: aarch64 Linux

Two targets, and they answer different questions.

| | `vm` (primary) | `opiz3` (reference low-end) |
| :--- | :--- | :--- |
| Access | `ssh vm` (`127.0.0.1:2222`) | `ssh opiz3` |
| CPU | 4x Apple M4 cores under HVF (`-accel=hvf -cpu host`) | 4x Cortex-A53 (r0p4), in-order, 480-1512 MHz |
| RAM | 961 MiB | 1986 MiB + 993 MiB zram swap |
| OS | Ubuntu 22.04.5 (glibc 2.35), kernel 5.15.0-203 | Ubuntu 24.04.5 (glibc 2.39), kernel 6.1.31-sun50iw9 |
| GPU | virtio-gpu, no virgl | Mali-G31 Bifrost (panfrost) |
| Renderer | llvmpipe (software) | llvmpipe via Weston; panfrost via `offscreen` |
| Status | **available** | **offline since 2026-11-21** |

Game tree on both: `~/nwii-test`. The VM needs the display pinned to 640x480;
the board's thermal zones idle at ~49-50 C so the A53s do not throttle early.

**Measurements are not transferable between them.** An A53 is in-order and 3-4x
narrower per clock, so a change that is free on the M4 can dominate there. The
`game_id` string comparison in the I-037 port measured 0.01% on the Mac and
**9.43% of all samples** on the board. Anything touching a hot path needs a
board run before it is called done — while the board is offline, say plainly
that a change is unmeasured rather than presenting VM numbers as if they
transferred.

## Display path

Both machines run **Weston in kiosk mode on tty1** as a boot service, so the
game presents as a Wayland client through the compositor's EGL/GLES stack — the
same path a consumer device takes, not a direct SDL window.

```sh
ssh vm systemctl status weston-kiosk    # enabled at boot
ssh vm journalctl -u weston-kiosk -f
ssh vm ~/nwii-test/run.sh               # game as a Wayland client
```

The VM's `weston.ini` pins `Virtual-1` to **640x480**, matching
`-device virtio-gpu-pci,xres=640,yres=480`. The CRTC and the qemu window are
independent: qemu draws whatever mode the guest selects into that fixed 640x480
window, so a larger CRTC is scaled to 50% in both directions inside it and
everything looks half-size. Check the mode in
`/sys/kernel/debug/dri/0/state`, not `/sys/class/graphics/fb0/virtual_size`
(the kernel boot console, which never follows the scanout).

The board is driven headless by Weston on the **VNC backend**, which composites
in **Pixman**, so the `wayland-kiosk` socket only offers llvmpipe and all GX
rasterization runs on the A53s. Consequences:

- The Mali-G31 **is** functional (`eglinfo` shows it on GBM/surfaceless,
  GLES 3.1), but no GPU-capable Wayland surface is exported through this
  socket. `MESA_LOADER_DRIVER_OVERRIDE=panfrost|zink` changes nothing.
- The HDMI output is unplugged, which is why Weston is on VNC at all.
- `SDL_VIDEODRIVER=kmsdrm` is unavailable in this SDL2 build.
- Samples attributed to `llvmpipe-*`, `libgallium`, `[JIT] tid <n>` and
  `libEGL_mesa` are **software rasterization**, not guest CPU emulation.

### Reaching the real GPU on the board

`SDL_VIDEODRIVER=offscreen` creates an EGL context directly on the device,
bypassing the compositor. It is the only way to measure the Mali-G31:

```sh
cd ~/nwii-test && SDL_VIDEODRIVER=offscreen ./run.sh
```

It presents to a pbuffer, so there is no visible output — a measurement path,
not a play path. It renders for real (`efb_nonblack=307196/307200`, and
`SDL_VIDEO_OFFSCREEN_SAVE_FRAMES=1` writes actual frames) at 1024x768.

## Deployed layout (`~/nwii-test`)

```
RaymanOrigins             ~168 MiB aarch64 ELF, not stripped
game/                     extracted SOJE41 tree
run.sh                    launcher: sets the Wayland env, execs the game
config_gpu.toml           [graphics] backend = "OpenGLES"
```

**`RaymanOrigins` and `run.sh` are the only permanent artifacts.** Stage every
other build to `~/nwii-test/scratch/` and delete it when the comparison is done
— including `/tmp` copies, which are on the same overlay filesystem, not in
RAM. Keep at most one fallback binary.

This is not tidiness: the VM disk is a fixed-size qcow2 that has already been
filled once. A dozen kept debug binaries reached 2.0 GB of 15 GB and even
`scp` of a test build failed. A release binary is ~170 MB, so a five-way A/B
costs ~1 GB. Check `df -h /` before staging.

Run the game with `./run.sh` from `~/nwii-test`. It takes the full Wayland
socket plus audio and input; it exits by itself only with `NWII_MAX_SECONDS`,
so otherwise close the window or kill it.

## Profiling

The CSV sampler (`NWII_PERF_LOG`) is the portable instrument: it works on both
machines and needs no privileges. Column meanings are in `README.md`.

```sh
cd ~/nwii-test
NWII_PERF=1 NWII_PERF_LOG=/tmp/p.csv NWII_MAX_SECONDS=60 ./run.sh
```

On the VM, `perf` is installed (`linux-tools-5.15.0-203-generic`) but the
**hardware PMU is not exposed under HVF** — `perf stat -e cycles` reports
`<not supported>`. Sample `cpu-clock` instead. `perf_event_paranoid=4` blocks
unprivileged recording, so it needs `sudo` with the Wayland environment passed
through explicitly, since sudo resets `XDG_RUNTIME_DIR` to `/run/user/0`:

```sh
ssh vm 'cd ~/nwii-test && sudo env XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland \
  NWII_AUTOADVANCE=1 NWII_MAX_SECONDS=25 \
  perf record -e cpu-clock -o /tmp/p.data -- ./RaymanOrigins ./game ./config_gpu.toml'
ssh vm 'sudo perf report -i /tmp/p.data --stdio -g none --sort symbol'
```

Under HVF, hypervisor-exposed instructions are weighted far more heavily than
on bare metal. `mrs cntvct_el0` showed 42% of `process_pending_callbacks` self
time, but `NWII_STEADYTB=1` (a vDSO read instead) measured **slower**, 73.6 vs
96.8 MIPS, so the counter is cheap and that attribution is an artifact. Confirm
any such conclusion with an A/B.

On the board, `perf` is not preinstalled and the `/usr/bin/perf` wrapper
refuses to run on the vendor kernel. Use the absolute binary path
(`/usr/lib/linux-tools-6.8.0-149-generic/perf`), which also takes `-F 299 -g`
and records **unprivileged** — do not wrap it in `sudo` there either.

Either way: **A/B must compare binaries built from the same tree.** Two
mistakes produced confident, meaningless numbers here — diffing against an
older staged binary that predated the fixes under test, and `cp`-ing a
candidate over a running executable, which fails with `Text file busy` and
leaves the previous binary in place so both runs measure the same thing. Run
distinct paths and `md5sum` the pair first.

## Baseline numbers (Orange Pi Zero 3, A53)

Kept as the reference for what the target hardware does. llvmpipe, 640x480,
ES 3.2, vsync on, steady state:

| Config | fps | guest MIPS | parse_ms | render_ms |
| :--- | :--- | :--- | :--- | :--- |
| `LP_NUM_THREADS=1` | 15.6 | 7.2 | 13.9 | 20.2 |
| `LP_NUM_THREADS=2` | 27.5 | 8.1 | 25.6 | 36.4 |
| `LP_NUM_THREADS=4` (default) | 33.7 | 8.5 | 31.1 | 44.9 |
| `LP_NUM_THREADS=8` | 33.7 | 8.4 | 31.2 | 46.6 |

The same scene on the Mali-G31 via `SDL_VIDEODRIVER=offscreen`: **59.9 fps**,
9.6 guest MIPS, 52.3 guest frames/s against 31.0 on llvmpipe. The win there is
guest frames, not MIPS — software rasterization was costing about half the total
CPU. `NWII_NO_VSYNC` and `NWII_HIDDEN_WINDOW` change nothing: 60 fps is the
guest's own VI limit.

Over 70% of the guest CPU thread is runtime bookkeeping rather than translated
guest code, and the largest single item is reading the timebase. 37.2% of guest
PC samples land on the idle spin `0x80006CCC`, a bare `goto` loop that never
blocks. Fixing the idle-thread throttle would help both display paths.

## Gotchas

- Headless (`NWII_HEADLESS=1`) has its own loop, ignores `NWII_MAX_SECONDS` and
  never reaches the CSV sampler. Run it under `timeout -s KILL`.
- A cold cache (`sudo sysctl vm.drop_caches=3`) shifts boot timings noticeably;
  compare like with like.
- No screenshot protocol in the VM's Weston build, so verification is visual on
  the qemu window or via the CSV counters.
