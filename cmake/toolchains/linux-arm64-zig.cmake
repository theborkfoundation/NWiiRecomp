# Linux ARM64 (Debian arm64 / aarch64 userland) cross build using zig.
#
# Prepare the target libraries and compiler wrappers:
#
#   tools/debian-sysroot.py                 # build/sysroot-arm64
#   tools/zig-toolchain.sh arm64            # build/zig-aarch64-*
#
# Configure a generated export or this project with:
#
#   cmake -B build-arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<this file> \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DNWII_SYSTEM_SDL2=ON -DNWII_ENABLE_GLES=ON
#
# Override NWII_ZIG_DIR or NWII_SYSROOT when the wrappers/sysroot live
# elsewhere. NWII_SYSTEM_SDL2=ON is the important part: it links the sysroot's
# SDL2 through pkg-config instead of cross-building SDL2 (which would need a
# host wayland-scanner).

if(NOT DEFINED NWII_ZIG_DIR)
    get_filename_component(NWII_ZIG_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../build" ABSOLUTE)
endif()
if(NOT DEFINED NWII_SYSROOT)
    get_filename_component(NWII_SYSROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../build/sysroot-arm64" ABSOLUTE)
endif()

if(NOT EXISTS "${NWII_ZIG_DIR}/zig-aarch64-cc")
    message(FATAL_ERROR
        "Missing zig wrappers in ${NWII_ZIG_DIR}.\n"
        "Run: tools/zig-toolchain.sh arm64")
endif()
if(NOT EXISTS "${NWII_SYSROOT}/usr/include/SDL2/SDL.h")
    message(FATAL_ERROR
        "Missing Linux ARM64 sysroot at ${NWII_SYSROOT}.\n"
        "Run: tools/debian-sysroot.py")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER "${NWII_ZIG_DIR}/zig-aarch64-cc")
set(CMAKE_CXX_COMPILER "${NWII_ZIG_DIR}/zig-aarch64-cxx")
set(CMAKE_AR "${NWII_ZIG_DIR}/zig-aarch64-ar")
set(CMAKE_RANLIB "${NWII_ZIG_DIR}/zig-aarch64-ranlib")

set(CMAKE_SYSROOT "${NWII_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${NWII_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# zig does not apply --sysroot to -I, so the target's include directories have
# to be named explicitly. /usr/include comes first: libc++ wrapper headers are
# kept ahead of the target's glibc headers.
string(APPEND CMAKE_C_FLAGS_INIT
    " -isystem ${NWII_SYSROOT}/usr/include"
    " -isystem ${NWII_SYSROOT}/usr/include/aarch64-linux-gnu")
string(APPEND CMAKE_CXX_FLAGS_INIT
    " -isystem ${NWII_SYSROOT}/usr/include"
    " -isystem ${NWII_SYSROOT}/usr/include/aarch64-linux-gnu")

# zig's cc emits DWARF even for -O3 -DNDEBUG without an explicit -g. The
# generated code is large enough that this turns a ~160 MB release executable
# into an ~800 MB one. Keep symbols opt-in for target debugging.
set(NWII_ARM64_DEBUG_INFO OFF CACHE BOOL
    "Keep DWARF debug info in Linux ARM64 cross builds (large binaries)")
if(NOT NWII_ARM64_DEBUG_INFO)
    string(APPEND CMAKE_C_FLAGS_INIT " -g0")
    string(APPEND CMAKE_CXX_FLAGS_INIT " -g0")
endif()

# pkg-config sees only the sysroot's .pc files. PKG_CONFIG_SYSROOT_DIR is
# deliberately unset: zig applies --sysroot to -L paths itself, so prefixing
# them here would search inside the sysroot twice.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR}
    "${NWII_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${NWII_SYSROOT}/usr/lib/pkgconfig:${NWII_SYSROOT}/usr/share/pkgconfig")
