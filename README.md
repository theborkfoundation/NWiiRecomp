<p align="center">
  <img src="image/logo_wide.jpg" alt="NWiiRecomp" width="700"/>
</p>

<p align="center">
  Static recompilation and runtime toolkit for Nintendo GameCube and Wii executables.
</p>

<p align="center">
(The first recompiler that really works)
</p>

<p align="center">
  <a href="SHOWCASE.md"><strong>View Media Showcase</strong></a>
  <br><br>
  <a href="https://discord.gg/A6w96BuMX">
    <img src="https://img.shields.io/badge/Discord-NWiiRecomp-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="NWiiRecomp Discord"/>
  </a>
  <br><br>
  <a href="https://youtube.com/@blacklineinteractive">
    <img src="https://img.shields.io/badge/YouTube-Blackline_Interactive-FF0000?style=flat-square&logo=youtube&logoColor=white" alt="Blackline Interactive YouTube"/>
  </a>
</p>

---

## What is this?

NWiiRecomp translates Nintendo Wii/GameCube (`.dol`, `.elf`) executables into native C++ code. The output is a standalone executable that runs natively without instruction-level emulation. Hardware interactions are handled by a High-Level Emulation (HLE) runtime layer.

> **Wii U Note:** Support for Wii U (`.rpx`, `.rpl`) and Cafe OS has been moved to our dedicated recompile repository! For Wii U, please visit: [nWiiURecomp](https://github.com/BlackLineInteractive/nWiiURecomp)

> **A Note on Our Approach:** Unlike some recompilation projects that rely heavily on Dolphin's VideoCommon or shader generation (where the recompiler is merely a PPC-to-C translator feeding the GX stream back into an existing emulator), NWiiRecomp features a fully **custom-built** rasterizer, shader generator, and HLE runtime from scratch. We are not simply reusing an existing emulator's pipeline; this is a fully standalone implementation.

> **Note:** The recompiler and runtime are designed to be **universal**. We have tested the architecture on radically different games (e.g., *Need For Speed: Hot Pursuit 2*, *Mario Party 7*, and *Silent Hill: Shattered Memories*). The fact that these fundamentally different engines yield identical, stable hardware behavior and 0 crashes confirms the universality of the core emulator design.
> **Latest Update:** *Mario Party 7* (GameCube) successfully boots past the Nintendo and Hudson logos, displaying the Dolby Pro Logic II screen and reaching the real-time 3D intro story sequence. It achieves this with accurate hardware projection matrix folding, viewport transforms, and fully functional Z-buffer depth testing natively in OpenGL! *Need for Speed: Hot Pursuit 2* (GameCube) has also shown new boot progress. No instruction-level emulation, no Dolphin code.

---

## Project Structure

```
wii-lie/
├── nWiiAnalyzer/   — DOL/ELF parser and function boundary analyzer
├── nWiiRecomp/     — Offline static recompiler (PPC → C++)
├── nWiiRuntime/    — Cross-platform runtime + HLE library
└── nWiiStudio/     — GUI debugging and inspection tool (Raylib + ImGui)
```

---

## What Works

### Analyzer (`nWiiAnalyzer`)

- Full DOL section parsing.
- Recursive disassembly from entry point.
- Branch analysis for function boundary discovery.
- Vtable and jump table pointer recovery.
- Heuristics for OS dispatch stubs (via `lis`/`addi` patterns).

### Recompiler (`nWiiRecomp`)

- Translates PowerPC 750CL instructions to C++.
- Implements core integer, logic, floating point, branch, and SPR instructions,
  including the time base (`mftb`) and decrementer (`mtdec`/`mfdec`).
- Implements paired-singles (SIMD) with GQR-based quantization scales mapped to C++ intrinsics.
- Tail-call detection and `goto`-based local branch inlining.
- Mid-function entry point dispatch through a compact table of instruction
  label offsets (a switch is kept only for sparse analyzer artifacts), so
  callbacks, interrupts, indirect branches and restored thread contexts can
  resume at any instruction.
- Per-game HLE hooks from `[hle_hooks]` in the config TOML; the recompiler
  itself contains no game-specific addresses.
- Functions that are all zeroes in the DOL (filled with code at runtime)
  are routed to the runtime interpreter instead of being recompiled.
- Optional per-function call tracing, enabled at run time with
  `NWII_TRACE_CALLS=1`.

### Runtime (`nWiiRuntime`)

- **Configuration**: Per-game TOML profiles via `tomlplusplus` for target platform, HLE hooks, and input.
- **Interpreter fallback**: A PPC750 integer interpreter executes code that
  does not exist in the DOL image (arena-clear helpers copied to low memory,
  trampolines, streamed overlays), then hands control back to recompiled code.
- **Interrupts**: PI interrupt controller with the Dolphin `INT_CAUSE_*` bit
  layout dispatched into the guest `__OSInterruptTable`; decrementer
  exceptions drive `OSAlarm`.
- **Hardware MMIO**: Register-level emulation of VI, DI (drive commands with
  DMA from a virtual disc), SI (joybus protocol with controller ID, origin,
  and polling), EXI (RTC/SRAM chip with valid checksums), DSP mailboxes,
  AI, MI, and the Wii IPC (Starlet) interface.
- **IOS HLE**: /dev/di, /dev/fs (virtual NAND with SYSCONF and settings),
  /dev/es, /dev/stm, /dev/usb served through a fd-based kernel, reachable
  both from IOS_* library calls and from the raw HW IPC ring.
- **Virtual disc**: Direct reads from extracted game dumps (`sys/` +
  `files/`), WBFS, and ISO images.
- **Input**: Selectable modes in `[input]`: gamepad as Classic/GC pad,
  gamepad with stick- or gyro-assisted pointer, full tilt control with
  sensitivity setting, smartphone as Wiimote over UDP, keyboard and mouse.
- **GX Graphics**: Structure tracking and WGPIPE ring-buffer parsing.
- **Memory**: DOL loading and virtual memory mapping; GC and Wii low-memory
  layouts (arena, FST, BI2, console type) set up per platform.
- **Headless mode**: `NWII_HEADLESS=1` runs without a window for automated
  boot testing and log capture.

### Rendering backends

Desktop OpenGL 3.3 core is the default. Both profiles share the GL renderer, GX
command processing, texture decoding and SDL presentation path. Select ES in the
runtime config:

```toml
[graphics]
backend = "OpenGLES"   # case-insensitive; OpenGL remains the default
```

- The runtime requests an ES 3.0 context through SDL (`PROFILE_ES`, EGL) and
  verifies the returned context, the required entry points and the
  shader-relevant limits. Selecting ES on a platform without a usable driver is
  a hard error carrying the SDL details; there is no silent fallback to desktop
  OpenGL.
- TEV and XFB shaders are dialect-aware: GLSL 3.30 for desktop, `#version 300
  es` with explicit precision qualifiers for ES. GL textures use sized `RGBA8`
  formats, and the optional EFB texture dump reads through a framebuffer rather
  than `glGetTexImage`, which ES does not provide.
- Build with `-DNWII_ENABLE_GLES=OFF` to omit the profile; selecting it then
  fails with an explicit build-time refusal.
- **macOS/ANGLE**: macOS has no native ES driver. Point the build at an ANGLE
  distribution containing `libEGL.dylib` and `libGLESv2.dylib`:

  ```bash
  cmake -B build -DNWII_ANGLE_DIR=$HOME/Projects/ANGLE-dist
  ```

  The runtime sets SDL's `SDL_VIDEO_GL_DRIVER`/`SDL_VIDEO_EGL_DRIVER` to the
  absolute dylib paths unless those variables are already set; ANGLE is loaded
  dynamically by SDL, not statically linked. Run-time overrides still work.
  ANGLE redistribution is subject to ANGLE's BSD-3-Clause license.
- **Tested matrix**:
  - macOS arm64 (Apple M4), desktop OpenGL 3.3 core context (driver reported
    OpenGL 4.1 / GLSL 4.10).
  - macOS arm64 (Apple M4) + ANGLE 2.1 over Metal (local `ANGLE-dist` with
    `libEGL.dylib`/`libGLESv2.dylib`, arm64, ad-hoc signed; ANGLE reports
    "git hash: unknown hash") — context `OpenGL ES 3.0.0`, GLSL ES 3.00,
    16 texture units, 16384 max texture size, 31 varying vectors.
  - On that ANGLE ES context the TEV and XFB shaders compile and link, EFB
    rendering and the EFB texture-dump readback work, XFB presentation matches
    desktop pixel-for-pixel, and captured early boot frames are pixel-identical
    to desktop GL. Late, time-varying frames differ only by small amounts
    consistent with a few frames of timing offset.
  - Linux/EGL (Mesa) and other ES implementations are unverified. Use
    `SDL_VIDEO_GL_DRIVER`/`SDL_VIDEO_EGL_DRIVER` there; `NWII_ANGLE_DIR` is
    currently macOS-only.

### Studio (`nWiiStudio`)

- Raylib + ImGui-based GUI (legacy; main runtime now uses SDL2 + OpenGL)
- DOL file browser and loader
- Function list panel with address and instruction count
- Disassembly viewer (raw PPC hex + decoded mnemonic)
- Basic memory map view
- **Settings & Config Integration**: Direct integration with `recomp_config.toml` to manage paths cleanly.
- **Thematic Themes**: Includes "Nintendo" theme (GameCube Indigo / Wii aesthetic) for a polished user experience.

---

> **A Note on Development:** To ensure maximum code quality and preserve the integrity of the architecture, AI is used **exclusively** for generating commit message titles and translating code comments to English—nothing more. All core reverse-engineering, recompilation logic, and architectural design are entirely human-driven.

---

## Building

**Requirements:** CMake 3.20+, GCC or Clang with C++20 (generated code uses
GNU label addresses for mid-function resume), internet access (SDL2 is fetched
automatically).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Generated code is not usable without optimization, so use a Release build (or
`--config Release` with a multi-config generator) for any performance
measurement.

### Linux ARM64 cross builds

The Linux ARM64 target uses Debian `arm64` (a 64-bit aarch64 userland). Prepare
the target libraries and zig compiler wrappers once:

```bash
./tools/debian-sysroot.py            # build/sysroot-arm64 from the lock
./tools/zig-toolchain.sh arm64       # build/zig-aarch64-{cc,cxx,ar,ranlib}
```

The sysroot tool resolves the SDL2/X11/Wayland dependency closure from the
Debian trixie archive, downloads the `.deb`s into `build/debs-arm64`, and
unpacks them into `build/sysroot-arm64`:

```bash
./tools/debian-sysroot.py                            # populate from the lock
./tools/debian-sysroot.py --verify                   # check the tree against the lock
./tools/debian-sysroot.py --update-lock --dry-run    # refresh the lock, no downloads
./tools/debian-sysroot.py --print-pkgconfig          # pkg-config environment for the tree
```

`tools/sysroots/arm64.lock` pins the exact package versions; regular runs resolve
against it, and `--update-lock` is the deliberate way to move. Only the lock is
tracked; the sysroot and `.deb` cache are derived artifacts under the ignored
`build/` directory. Unpacking zstd-compressed `.deb` payloads needs `bsdtar`
(`libarchive-tools` on Linux; macOS ships it).

Configure a generated export (or this project) for Linux ARM64:

```bash
cmake -B build-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/wii-lie/cmake/toolchains/linux-arm64-zig.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DNWII_SYSTEM_SDL2=ON \
  -DNWII_ENABLE_GLES=ON
cmake --build build-arm64 -j$(nproc)
```

`NWII_SYSTEM_SDL2=ON` links the sysroot's SDL2 through pkg-config instead of
cross-building SDL2, so no host `wayland-scanner` is needed. pkg-config's
library paths stay relative to the sysroot and zig's `--sysroot` resolves them
once; `PKG_CONFIG_SYSROOT_DIR` must stay unset. Without the option the project
still fetches SDL2 2.28.5, preserving the macOS/desktop path.

Run on the target:

- X11/Wayland desktop: SDL selects the driver. `[graphics] backend =
  "OpenGLES"` uses EGL, or keep the default `OpenGL` on Mesa/GLX.
- KMSDRM console/handheld: `SDL_VIDEODRIVER=kmsdrm`, `[graphics] backend =
  "OpenGLES"`, and access to the DRM device (`video`/`render` group, console
  VT).
- Shared SDL2 requires `libsdl2-2.0-0` plus Mesa/EGL/GLES and the selected
  window/audio backend libraries on the target.
- The sysroot lock is Debian trixie (glibc 2.41). Match the target's release
  with `--suite` + `--update-lock`, or build natively, rather than running a
  newer-glibc binary on an older userland.
- Cross builds default to `-g0`: zig emits DWARF even at `-O3`, which turns the
  ~160 MB release executable into an ~800 MB one. Pass
  `-DNWII_ARM64_DEBUG_INFO=ON` when you need target symbols.
- On a headless ARM64 target, Xvfb + Mesa llvmpipe provides the GL context;
  SDL's default audio driver (PulseAudio/ALSA) is used. No
  `SDL_AUDIODRIVER=disk` workaround is needed after the AX idle-PB fix.

On a native Debian/Ubuntu arm64 host, install `build-essential cmake
ninja-build pkg-config libsdl2-dev libegl1-mesa-dev libgles2-mesa-dev` and use
`-DNWII_SYSTEM_SDL2=ON` without a toolchain file.

---

## Usage

```bash
# 1. Analyze and recompile the game (paths and options come from the TOML)
./build/nWiiRecomp/nwiirecomp config.toml

# 2. Build the generated project (Release unless you choose otherwise)
cd export
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j12
# Multi-config generators (Xcode, Visual Studio):
# cmake --build build --config Release

# 3. Run with the extracted game directory
cd ..
./export/build/<ProjectName> path/to/extracted-game config.toml
```

The recompiler outputs a self-contained `export/` directory containing the
generated sources and a copy of `nWiiRuntime`. It can be built independently
without the rest of this repository. Delete `export/` (including its `build/`)
before regenerating so no stale sources survive.

Useful environment variables:

```bash
NWII_HEADLESS=1      # run without a window, log-only (boot debugging)
NWII_TRACE_CALLS=1   # print every recompiled function entry
NWII_STATS=1         # periodic [STAT] probe; off by default because it reads
                     # 1.2 MB back to the CPU every 5 s
```

The periodic `[STAT]` probe is opt-in for that reason. Screenshots
(`NWII_SCREENSHOT`), texture dumps (`NWII_TEXDUMP`), XFB presentation
(`NWII_XFB`) and logo step dumps (`NWII_LOGO_STEPS`) are also diagnostic
readbacks and should stay off in performance runs.

### Performance overlay and CSV metrics

`NWII_PERF` enables a bitmap-font overlay and a CSV logger that sample the
runtime once per interval. It is separate from `NWII_STATS` (the periodic
`[STAT]` probe).

```bash
# Compact overlay on the game window
NWII_PERF=1 ./RaymanOrigins path/to/game config.toml

# Full overlay: adds the current FRAMES count and TEX upload/hit/miss split
NWII_PERF=full ./RaymanOrigins path/to/game config.toml

# CSV only, no overlay: least perturbing for performance measurements
NWII_PERF_LOG=/tmp/nwii-perf.csv ./RaymanOrigins path/to/game config.toml

# Sample every 250 ms instead of the default 1000 ms
NWII_PERF=1 NWII_PERF_INTERVAL_MS=250 NWII_PERF_LOG=/tmp/nwii-perf.csv \
  ./RaymanOrigins path/to/game config.toml
```

- `NWII_PERF` — unset, empty, `0`, or `off`: no overlay. Any other value
  (`1`, `full`, `2`, …) enables the overlay; `full` or `2` also shows the
  extra full-mode line.
- `NWII_PERF_LOG` — path to append CSV rows. The header is written only when
  the file is new or empty. A file is not truncated on startup.
- `NWII_PERF_INTERVAL_MS` — sampling window in milliseconds. Default `1000`;
  values outside `100..60000` are ignored.
- `NWII_PERF_LOG` without `NWII_PERF` samples and logs without drawing the
  overlay. Use that mode for clean performance numbers: the overlay itself is
  drawn after the game's present and before the swap, so it consumes CPU/GPU
  time and can perturb the numbers it reports.
- When neither variable is set, the overlay, sampler and CSV writer do no
  work. The runtime's underlying relaxed atomic counters are still incremented
  by normal execution.

The compact overlay is:

```text
FPS ... FRAME ... P99 ...
VI ... DRAWS ... MIPS ...
PARSE ... RENDER ... WGP ...
FIFO ... TEX <uploads>/<lookups> SH ...
CB ... CTX ... RS ...
AUD ... V ... PK ...
ERR BAD ... UNK ... VFIX ... HDR ... DL ...
```

The full overlay adds:

```text
FRAMES ... TEX UP ... HIT ... MISS ...
```

#### Frame pacing and guest CPU

| Overlay / CSV | Unit | Meaning |
| :--- | :--- | :--- |
| `FPS` / `fps` | frames/s | Host presentation rate: the number of `on_present()` calls in the sample window divided by its duration. This is the window's present rate, not `FRAMES` (the guest GX frame rate). With vsync it usually tracks the display refresh; with vsync off it tracks the host loop. |
| `FRAME` / `frame_ms` | ms | Rolling average interval between host presents over the last 240 presents. It is a rolling value, not a per-sample reset, so it can differ from `1000 / FPS`. |
| `P99` / `p99_ms` | ms | The 99th percentile of those 240 present intervals (nearest-rank). A high p99 relative to `FRAME` indicates occasional hitches/stutter even when average FPS looks healthy. |
| `FRAMES` / `frames` | count in window | Guest GX frames drained by the command processor (`g_stat_frames`), one per detected GX frame boundary. Full overlay mode and CSV only. This is not necessarily one-to-one with host presents: the runtime can present the same guest frame more than once, or drain multiple guest frames between presents. |
| `VI` / `vi` | count in window | VI interrupt triggers delivered to the guest (`g_vi_trigger_count`). The runtime deliberately coalesces retrace requests to roughly one per field (~59.94 Hz) unless `NWII_NO_VI_GATE` is set. This counts VI events, not GX draws or actual scanouts. A value above `FPS` means retrace events outnumbered presentations; it does not by itself prove that frames were skipped. |
| `MIPS` / `mips` | million guest instructions/s | Guest PowerPC instructions retired per second: the delta of `CPUContext::inst_count` divided by the sample duration and 1e6. It measures emulated guest CPU work, not host CPU MIPS and not a host CPU percentage. |
| `PARSE` / `parse_ms` | ms in window | CPU milliseconds spent parsing and decoding the GX command FIFO (`FifoParser::Parse` plus `DecodeDraw`). It is not per frame. At the default 1000 ms window this is effectively ms/s; divide by 10 for the approximate percentage of one CPU core (3 ms/s ≈ 0.3 % of a core). If the interval is changed, the value is still milliseconds in that window, not a normalized rate. |
| `RENDER` / `render_ms` | ms in window | CPU milliseconds spent in the GL renderer's `Render(frame)` call, covering command submission, state/shader/texture work on the CPU. It does not include GPU execution or buffer swap time. Same window semantics as `PARSE`. GPU-bound scenes can show low `RENDER` while `FPS` or `P99` is poor. |

#### GX command and renderer throughput

| Overlay / CSV | Unit | Meaning |
| :--- | :--- | :--- |
| `DRAWS` / `draws` | count in window | Decoded GX draw primitives in the drained guest frames (`g_stat_draws`). Each surviving draw command increments it once; it is a count of guest draw calls, not GL draw calls, triangles, or vertices. |
| `WGP` / `wgp_mb` | MiB/s | Bytes per second redirected from the GX write-gather pipe into guest memory (`wgp.redirected_bytes`). This happens when the PI FIFO destination differs from the GP command FIFO, typically while recording display lists or uploading vertex data. It is a normal mechanism, not GPU bus bandwidth, and not all WGP traffic. The runtime also prints `[WGPREDIR]` transitions when enabled. |
| `FIFO` / `fifo_mb` | MiB/s | GX command-stream bytes pushed into the FIFO and fed to the parser per second (`fifo_bytes`), after write-gather redirection is filtered out. This is command-stream throughput, not FIFO queue depth, rendering bandwidth, or GPU memory bandwidth. |
| `TEX` / `tex_up`, `tex_hit`, `tex_miss` | counts in window | Texture-cache activity. `tex_up` is the number of successful texture uploads to GL (decode plus `glTexImage2D`); `tex_hit` is a lookup served by an existing GL texture; `tex_miss` is a lookup that was not cached and triggered decode/upload. The compact overlay shows `tex_up/(tex_hit + tex_miss)` (uploads versus lookups). A high hit count and low upload count is healthy; streaming movies or animated textures can legitimately miss every frame. A miss is not an error. |
| `SH` / `shaders` | entries (gauge) | Current number of entries in the generated GX shader cache (`nwii_stat_shaders()`), not shader compiles per second. It grows as new TEV/state permutations are encountered. A stable value means the shader cache is warm; it is not reset by the sample interval. |

#### Guest scheduling and callbacks

| Overlay / CSV | Unit | Meaning |
| :--- | :--- | :--- |
| `CB` / `cb` | count in window | HLE callbacks queued into the runtime's pending callback queue (`perf::callbacks` in `queue_callback`), including IRQ callbacks. This counts callbacks queued, not callbacks executed. Boot and IOS initialization can queue hundreds; a steady stream can be normal for games with async audio/disk work. |
| `CTX` / `ctx` | count in window | Successful guest OSContext restores: how often `hle_load_context_from_guest()` loaded a context from guest memory. This is not a count of host thread context switches. It is high during boot loading and idle guest scheduling; a sustained, extreme value (for example, millions per second) points to a context-restore loop rather than useful work. |
| `RS` / `rs` | count in window | Runtime-injected guest reschedules: the number of times the idle-thread path jumped to the guest `__OSReschedule` trampoline because runnable threads were detected. It is a scheduling diagnostic, not host thread migrations. |

#### Audio

| Overlay / CSV | Unit | Meaning |
| :--- | :--- | :--- |
| `AUD` / `audio` | sample frames in window | Per-channel sample frames pulled by the SDL audio callback from the ADMA ring. At 32 kHz and real-time playback this is roughly 32,000 frames per second; each frame carries left and right samples. A value near zero means no audio was produced/pulled (silence or underrun). This is not a video frame count. |
| `V` / `voices` | voices (gauge) | Number of running AX voices at the most recent sample. This is a gauge, not a per-window count; 0 means no active voices, for example on a silent menu. |
| `PK` / `peak` | PCM16 amplitude | Maximum absolute 16-bit audio sample seen by the pulls since the previous sample, reset to 0 after each sample. Range is 0..32768. Zero means silence; values near 32767 indicate possible clipping. This is a peak across both channels, not RMS and not a percentage. |

#### GX integrity counters

These are deltas of named diagnostics for the sample window. Each row is also
included in the overlay's final `ERR` line, which turns red when any non-zero
value is present.

| Overlay / CSV | Diagnostic | Meaning |
| :--- | :--- | :--- |
| `BAD` / `bad` | `fifo.bad_continuation` | A GX command was accepted whose payload did not end on another command boundary, meaning the command size or stream framing was wrong. This is an error-class event: check it with any visible corruption. |
| `UNK` / `unk` | `fifo.unknown_opcode` | A byte could not be decoded as a GX opcode; the parser skipped it to recover framing. This is a warning-class event. A single boot-time occurrence may be harmless; repeated occurrences mean commands are being missed. |
| `VFIX` / `vfix` | `fifo.vsize_repaired` | A draw's vertex size was re-derived from the stream because the VAT disagreed with the observed data. This is a warning-class recovery; verify that the draw's vertices look correct. |
| `HDR` / `hdr` | `fifo.headerless_vertices` | A vertex-data block had no draw header in front of it and was dropped. This is an error-class event because it means lost geometry. A one-off boot-time occurrence can be engine padding; repeated or per-frame occurrences correlate with missing/garbled draws. |
| `DL` / `dl` | `dl.rejected` | A display list was refused: size greater than 4 MiB, outside MEM1/MEM2, or nesting depth ≥ 4. This is an error-class event and means silently lost geometry. Empty lists are counted separately as the info-only `dl.empty` and are not included here. |

The overlay's `ERR` row is not a separate CSV column; it is the visual sum of
`bad + unk + vfix + hdr + dl` for the window. Other diagnostics such as
`frame.carry_without_end`, `dl.empty`, and `fifo.resync_bytes` are printed by
the diagnostic logger but are not part of this overlay/CSV.

#### CSV format and how to read the numbers

CSV columns, in order:

```text
fps,frame_ms,p99_ms,vi,draws,mips,parse_ms,render_ms,wgp_mb,fifo_mb,
tex_up,tex_hit,tex_miss,shaders,cb,ctx,rs,audio,voices,peak,
bad,unk,vfix,hdr,dl,frames
```

- `fps` is normalized per second; `mips` is normalized per second;
  `wgp_mb` and `fifo_mb` are MiB/s.
- `fps`, `frame_ms`, and `p99_ms` describe host presents. `frames` describes
  guest GX frames. Do not assume they are equal.
- `parse_ms` and `render_ms` are **milliseconds accumulated inside the sample
  window**, not per-frame averages and not automatically normalized. At the
  default 1000 ms window they read as ms/s; at a 250 ms window, 2.0 means 2 ms
  in that 250 ms window.
- `vi`, `draws`, `tex_up`, `tex_hit`, `tex_miss`, `cb`, `ctx`, `rs`, `audio`,
  and `frames` are **counts in the sample window**. At the default 1000 ms
  window they are per-second counts; if you change `NWII_PERF_INTERVAL_MS`,
  they remain counts for that window, not rates.
- `shaders` and `voices` are instantaneous gauges. `peak` is the maximum since
  the previous sample, reset afterward.
- `frame_ms` and `p99_ms` use the rolling last-240-present window, while
  `fps` uses the configured sampling window.
- `NWII_PERF_LOG` appends one row per sample and has no timestamp column. The
  first rows normally include boot/loading activity; use a steady-state slice
  when comparing performance. Remove the file before a run if you want a
  fresh header.

---

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) — Project structure and early inspiration from friend Ran-J; the foundation this recompiler is built on.
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) — Inspiration for the static recompilation approach

- [Dolphin Emulator](https://github.com/dolphin-emu/dolphin) — Huge thanks for the endless hardware documentation, GX/DSP accuracy, and HLE inspiration! 
  - **Disclaimer:** The NWiiRecomp runtime code is fully custom. Code from Dolphin is not copied 1:1; any logic based on Dolphin is explicitly marked in comments. The `totaldb.dsy` file is an exception—it is taken directly from Dolphin and is subject to Dolphin's license.
- [Decaf-emu](https://github.com/decaf-emu/decaf-emu.git) — Great resource for RPX/RPL loaders and Cafe OS kernel/syscalls.
- [WiiUBrew](https://wiiubrew.org/wiki/Hardware/GX2) — Excellent Wii U GX2 and Cafe OS documentation.
- [CafeGLSL](https://github.com/Exzap/CafeGLSL.git) — Open-source shader compiler alternative, crucial for understanding GX2 shaders.
- [rpl2elf](https://github.com/Relys/rpl2elf.git) — Useful for RPX/RPL to ELF conversion and parsing.
- [GhidraRPXLoader](https://github.com/decaf-emu/GhidraRPXLoader.git) — RPX loader logic.
- [WiiBrew](https://wiibrew.org/wiki/Main_Page) — Wii hardware and software documentation
- [YAGCD](https://hitmen.c02.at/files/yagcd/) — Yet Another GameCube Documentation — Low-level GC/Wii CPU and hardware reference
- PowerPC 750CL User's Manual — Official ISA reference

---

## License

License. See [LICENSE](LICENSE) for details.  
© 2026 Volodymyr Vovchok.

> **Disclaimer:** This project contains no copyrighted Nintendo code, SDKs, or game data. You must provide your own legally obtained game executables.
