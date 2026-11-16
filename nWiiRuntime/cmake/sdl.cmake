# SDL2 selection shared by the top-level project and generated exports.
#
# Default: fetch SDL2 2.28.5 and link its static target, which keeps macOS and
# desktop builds on the tested library.
#
# NWII_SYSTEM_SDL2=ON: use the target's SDL2 through pkg-config. This is the
# Linux ARM64 cross-build path: the Debian arm64 sysroot provides SDL2 with
# X11/Wayland/KMSDRM/EGL support, so no host wayland-scanner or SDL2
# cross-build is needed.
#
# zig cross-compiling notes: PKG_CONFIG_LIBDIR must point into the sysroot, but
# PKG_CONFIG_SYSROOT_DIR must be left unset. zig applies --sysroot to -L paths
# itself, so pre-prefixing library directories searches inside the sysroot
# twice. It does not apply --sysroot to -I, so include directories are made
# absolute here.

option(NWII_SYSTEM_SDL2
    "Use the target's SDL2 via pkg-config instead of fetching SDL2" OFF)

if(NWII_SYSTEM_SDL2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)

    set(NWII_SDL_LIBRARIES ${SDL2_LIBRARIES})
    set(NWII_SDL_LIBRARY_DIRS ${SDL2_LIBRARY_DIRS})
    set(NWII_SDL_INCLUDE_DIRS "")

    if(CMAKE_SYSROOT AND NOT IS_ABSOLUTE "${CMAKE_SYSROOT}")
        message(FATAL_ERROR "CMAKE_SYSROOT must be an absolute path")
    endif()

    foreach(_nwii_sdl_include IN LISTS SDL2_INCLUDE_DIRS)
        # /usr/include itself is provided by the toolchain as -isystem; naming
        # it again puts the target's glibc headers ahead of zig's libc++
        # wrapper headers.
        if(NOT _nwii_sdl_include STREQUAL "/usr/include")
            if(CMAKE_SYSROOT)
                list(APPEND NWII_SDL_INCLUDE_DIRS
                    "${CMAKE_SYSROOT}${_nwii_sdl_include}")
            else()
                list(APPEND NWII_SDL_INCLUDE_DIRS
                    "${_nwii_sdl_include}")
            endif()
        endif()
    endforeach()

    if(NOT NWII_SDL_LIBRARIES)
        message(FATAL_ERROR
            "pkg-config found SDL2 headers but no libraries; install "
            "libsdl2-dev in the sysroot or disable NWII_SYSTEM_SDL2")
    endif()
    message(STATUS
        "SDL2: system ${SDL2_VERSION} (link ${NWII_SDL_LIBRARIES})")
else()
    include(FetchContent)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.28.5
    )
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL2)
    set(NWII_SDL_LIBRARIES SDL2-static)
    set(NWII_SDL_LIBRARY_DIRS "")
    set(NWII_SDL_INCLUDE_DIRS "")

    if(APPLE)
        # SDL's Cocoa GL backend unconditionally asks for
        # NSOpenGLPFAAllowOfflineRenderers *and* pins NSOpenGLPFAScreenMask to
        # one display. On dual-GPU Macs no pixel format satisfies both, so every
        # SDL_GL_CreateContext fails. Drop the attribute; offline renderers are
        # of no use to us. Idempotent, runs before SDL compiles.
        set(_nwii_sdl_gl_src
            "${sdl2_SOURCE_DIR}/src/video/cocoa/SDL_cocoaopengl.m")
        if(EXISTS "${_nwii_sdl_gl_src}")
            file(READ "${_nwii_sdl_gl_src}" _nwii_sdl_gl_txt)
            string(REPLACE
                "attr[i++] = NSOpenGLPFAAllowOfflineRenderers;"
                "/* NWii: breaks pixel format on dual-GPU Macs */"
                _nwii_sdl_gl_patched "${_nwii_sdl_gl_txt}")
            if(NOT _nwii_sdl_gl_txt STREQUAL _nwii_sdl_gl_patched)
                file(WRITE "${_nwii_sdl_gl_src}" "${_nwii_sdl_gl_patched}")
                message(STATUS
                    "Patched SDL Cocoa GL: removed AllowOfflineRenderers")
            endif()
        endif()
    endif()
endif()
