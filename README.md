# NWiiRecomp

Static recompiler for Nintendo GameCube and Wii PowerPC executables: `.dol` and
`.elf` images become native C++ against a custom high-level-emulation runtime,
with no instruction-level emulation.

A fork of
[BlackLineInteractive/NWiiRecomp](https://github.com/BlackLineInteractive/NWiiRecomp)
targeting **Linux ARM64 (aarch64)** and **Rayman Origins** (SOJE41), the one
game configured and validated here; upstream carries the rest.

| Component | Role |
| :--- | :--- |
| `nWiiAnalyzer` | DOL/ELF parsing, function boundaries, jump tables |
| `nWiiRecomp` | Offline recompiler, PowerPC 750CL to C++ |
| `nWiiRuntime` | HLE hardware, GX parser, rasterizer, shaders |

Always build `Release`: generated code is only usable with optimisation on.
`./tools/ubuntu-sysroot.py && ./tools/zig-toolchain.sh arm64` prepares the
aarch64 sysroot and zig wrappers.

## Usage

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/nWiiRecomp/nwiirecomp recomp_config_rayman.toml    # writes export/
cd export && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

`export/` is self-contained and builds without this repository.

## Controls

Default is the horizontal Wiimote layout; `NWII_INPUT=classic` reports a
Classic Controller, `NWII_REMOTE=vertical` flips orientation. The mouse drives
the IR pointer and Xbox gamepads work in both modes.

`Space`/`Enter`/`K`/left click accept or jump; `J`/`Z`/`Shift`/right click
attack or back; `Esc`/`P` pause; `-`/`Tab` select; arrows or `WASD` move.

See [AGENTS.md](AGENTS.md) for the build, the test beds, profiling and
conventions, and [ISSUES.md](ISSUES.md) for open work.

Licensed under [LICENSE](LICENSE). No Nintendo code, SDK or game data is
included; supply your own extracted game.
