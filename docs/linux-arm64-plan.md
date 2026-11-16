# Linux ARM64 target

Status: implementation, 2026-11-16. The Linux ARM64 cross build is wired up and
the full Rayman Origins export cross-compiles and links as an aarch64 ELF. Target
execution is not yet validated on hardware; see [Validation](#validation).

## Goal and scope

Run the generated game and nWiiRuntime on a Linux ARM64 host, presenting through
SDL2 on X11, Wayland or KMSDRM, with desktop OpenGL (GLX) or OpenGL ES 3.0
(EGL). The recompiler remains a host tool; the generated export is the target
artifact. Android/iOS packaging is out of scope.

## Build modes

1. **Cross build (primary).** macOS or x86_64 Linux using zig's bundled clang
   and libc++, plus the Debian arm64 sysroot. SDL2 comes from the sysroot via
   pkg-config, so SDL2 itself is not cross-built and no host `wayland-scanner`
   is needed.
2. **Native build on the target.** Debian/Ubuntu arm64 with system SDL2,
   EGL/GLES and Mesa. No toolchain file or sysroot; same CMake options.
3. **Arm64 container.** Docker on Apple Silicon can compile and run headless
   smoke tests natively (`--platform linux/arm64`); it has no GPU, so graphics
   still needs target hardware.

## Implementation

- `tools/debian-sysroot.py` populates `build/sysroot-arm64` from
  `tools/sysroots/arm64.lock` (Debian trixie arm64, 177 packages). The lock pins
  exact versions; the tree and `.deb` cache are derived and ignored.
- `tools/zig-toolchain.sh arm64` writes `build/zig-aarch64-{cc,cxx,ar,ranlib}`.
  Only the compilers receive `-target aarch64-linux-gnu`; zig's LLVM archiver is
  target-independent and rejects `-target`.
- `cmake/toolchains/linux-arm64-zig.cmake` sets the sysroot, find-root modes,
  target `-isystem` headers and `PKG_CONFIG_LIBDIR`. `PKG_CONFIG_SYSROOT_DIR`
  stays unset: zig applies `--sysroot` to `-L` itself, so pre-prefixed library
  directories would be searched inside the sysroot twice.
- `nWiiRuntime/cmake/sdl.cmake` is shared by the top-level project and generated
  exports. It fetches SDL2 2.28.5 by default (macOS/desktop parity) and with
  `-DNWII_SYSTEM_SDL2=ON` uses pkg-config's SDL2, prefixing only include
  directories with the sysroot and linking `${SDL2_LIBRARIES}`.
- The root CMake declares only C/CXX on non-Apple platforms; OBJC/OBJCXX are
  enabled on Apple for Metal. Linux links `Threads::Threads`, `${CMAKE_DL_LIBS}`
  and `m`.
- The recompiler emits the same SDL2 module include into fresh exports, so a
  standalone export supports both fetched and system SDL2.

## Runtime configuration

- **X11/Wayland desktop:** SDL selects the video driver. The default
  `[graphics] backend = "OpenGL"` uses GLX; `"OpenGLES"` uses EGL.
- **KMSDRM console/handheld:** `SDL_VIDEODRIVER=kmsdrm`, `[graphics] backend =
  "OpenGLES"`, and DRM access (`video`/`render` groups, console VT). The runtime
  already requests an ES 3.0 EGL context and presents the EFB through a texture
  quad.
- **Target libraries:** shared SDL2 needs `libsdl2-2.0-0`; Mesa EGL/GLES plus
  `libdrm`/`libgbm` for KMSDRM; X11 or Wayland client libraries for those
  drivers; ALSA or PulseAudio. SDL2 is shared by default; the sysroot also has
  `libSDL2.a` for a future static option.
- Normal play performs no GPU-to-CPU readback: the periodic `[STAT]` probe is
  behind `NWII_STATS=1`, and screenshots, texture dumps, XFB and logo-step dumps
  remain opt-in diagnostics.

## Validation

Done:

- The full Rayman Origins export (89 generated units, `main_output.cpp`, runtime
  `main.cpp`) cross-compiled to aarch64 with zig and the sysroot.
- The executable linked against the sysroot's SDL2 2.32.4 via pkg-config:
  `ELF 64-bit LSB executable, ARM aarch64`.
- The default (fetched SDL2) macOS build still passes `ctest` 10/10.

Pending:

- Run the cross-built binary on arm64 hardware or an arm64 container: boot,
  X11/Wayland/KMSDRM context creation, ES 3.0 shader compile/link, rendering,
  audio, input, screenshots.
- Match the sysroot suite to the target's glibc before deploying (the trixie
  lock is glibc 2.41; older userlands need `--suite` + `--update-lock` or a
  native build).
- Release binaries are ~160 MB after the default `-g0` (zig otherwise emits
  DWARF even at `-O3`; the unstripped debug build was ~796 MiB: 672 MiB DWARF
  plus ~146 MiB `.text`). Pass `-DNWII_ARM64_DEBUG_INFO=ON` when target symbols
  are needed, and strip for deployment.
- Decide whether KMSDRM targets should ship shared or static SDL2 and document
  the target package set.

## Risks

- glibc/suite mismatch between the cross sysroot and the target image.
- SDL2 version drift: the sysroot has 2.32.4; the fetched path stays on 2.28.5.
  The runtime uses stable SDL2 APIs, but both should be exercised.
- Driver-specific EGL/KMSDRM behavior on low-end ARM64 GPUs (Mesa Panfrost,
  freedreno, Mali blobs); the ES path was validated on ANGLE/Metal, not these
  drivers.
- Cross-compiling generated code is CPU-heavy; builds are currently serial per
  unit at `-j` and produce large intermediate objects. DWARF is off by default
  to keep release binaries and link steps reasonable.
