#!/bin/sh
#
# Writes the compiler wrappers used by cmake/toolchains/linux-arm64-zig.cmake.
# Zig ships its own clang, LLD, libc and libc++, so a Linux ARM64 cross build
# works from macOS or x86_64 Linux without a system cross toolchain.
#
#   tools/zig-toolchain.sh arm64          # wrappers in build/
#   tools/zig-toolchain.sh arm64 /opt/x   # ... in /opt/x
#
# The Zig binary is taken from $ZIG, or `zig` on PATH.
#
# ar/ranlib deliberately do not receive -target: zig's LLVM archiver is
# target-independent, and zig 0.16 rejects -target for those subcommands.

set -eu

arch=${1:-}
out=${2:-build}

case "$arch" in
    arm64)
        target=aarch64-linux-gnu
        prefix=zig-aarch64
        ;;
    *)
        echo "usage: $0 arm64 [output-dir]" >&2
        exit 2
        ;;
esac

zig=${ZIG:-$(command -v zig || true)}
if [ -z "$zig" ]; then
    echo "$0: zig not found; install it or set ZIG=/path/to/zig" >&2
    exit 1
fi

mkdir -p "$out"

write_compiler() {
    name=$1
    shift
    printf '#!/bin/sh\nexec %s %s -target %s "$@"\n' \
        "$zig" "$*" "$target" > "$out/$name"
    chmod +x "$out/$name"
}

write_tool() {
    name=$1
    shift
    printf '#!/bin/sh\nexec %s %s "$@"\n' "$zig" "$*" > "$out/$name"
    chmod +x "$out/$name"
}

write_compiler "$prefix-cc" cc
write_compiler "$prefix-cxx" c++
write_tool "$prefix-ar" ar
write_tool "$prefix-ranlib" ranlib

echo "wrote $out/$prefix-{cc,cxx,ar,ranlib} ($target via $zig)"
