# NWiiRecomp

Static recompiler for Nintendo GameCube and Wii PowerPC executables: `.dol` and
`.elf` images become native C++ against a custom high-level-emulation runtime,
with no instruction-level emulation.

A fork of
[BlackLineInteractive/NWiiRecomp](https://github.com/BlackLineInteractive/NWiiRecomp)
targeting **Linux ARM64 (aarch64)** and **Rayman Origins** (SOJE41) — the one
game currently configured and validated. Upstream carries the wider game matrix
and the Wii U work.

| Component | Role |
| :--- | :--- |
| `nWiiAnalyzer` | DOL/ELF parsing, function boundaries, jump tables |
| `nWiiRecomp` | Offline recompiler, PowerPC 750CL to C++ |
| `nWiiRuntime` | HLE hardware, GX parser, rasterizer, shaders |
| `nWiiStudio` | ImGui inspection tool (not built by default) |

Generated code is only usable with optimisation on; always build `Release`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./tools/ubuntu-sysroot.py && ./tools/zig-toolchain.sh arm64   # aarch64 only
```

## Usage

```sh
./build/nWiiRecomp/nwiirecomp recomp_config_rayman.toml    # writes export/
cd export && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

`export/` is self-contained and builds without this repository.

See [TESTBED.md](TESTBED.md) for the test beds and profiling,
[AGENTS.md](AGENTS.md) for conventions, and
[docs/linux-arm64-plan.md](docs/linux-arm64-plan.md) for the toolchain.

Licensed under [LICENSE](LICENSE). No Nintendo code, SDK or game data is
included; supply your own extracted game.
