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
NWiiRecomp/
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

### Linux ARM64 cross-build sysroot

The Linux ARM64 target uses Debian `arm64` (a 64-bit aarch64 userland) and needs
the target's headers and libraries before cross-compiling. `tools/debian-sysroot.py`
resolves the SDL2/X11/Wayland dependency closure from the Debian trixie archive,
downloads the `.deb`s into `build/debs-arm64`, and unpacks them into
`build/sysroot-arm64`:

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
(`libarchive-tools` on Linux; macOS ships it). This is the sysroot foundation
for the next step: wiring a Linux ARM64 cross toolchain/preset against
`build/sysroot-arm64`.

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
