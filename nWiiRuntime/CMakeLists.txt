cmake_minimum_required(VERSION 3.20)

include(FetchContent)

# The top-level project (and the generated export) includes this module before
# add_subdirectory. Keep a fallback so nWiiRuntime can also be added directly.
if(NOT NWII_SDL_LIBRARIES)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdl.cmake")
endif()

# GLAD GL loader: use the vendored copy when present, otherwise fetch the
# generator and produce the same GL 3.3 core loader (glad1 keeps the
# glad/glad.h + gladLoadGLLoader API this code uses). Either way the result is
# a `glad` target nwiiruntime links against.
#
# The same loader serves the OpenGL ES 3.0 profile: glad1 resolves every
# declared entry point by name from the active SDL context (it strips the
# "OpenGL ES" version prefix), and the runtime uses only the GL/ES common
# subset. GLES-only code paths avoid desktop-only calls (glGetTexImage is
# replaced by a framebuffer readback), so one loader is reproducible for both
# profiles and no second, colliding loader is linked.
set(NWII_VENDORED_GLAD "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/glad")
if(EXISTS "${NWII_VENDORED_GLAD}/src/glad.c")
    add_library(glad STATIC "${NWII_VENDORED_GLAD}/src/glad.c")
    target_include_directories(glad PUBLIC "${NWII_VENDORED_GLAD}/include")
else()
    message(STATUS "vendored GLAD not found - fetching Dav1dde/glad v0.1.36")
    set(GLAD_PROFILE "core" CACHE STRING "" FORCE)
    set(GLAD_API "gl=3.3" CACHE STRING "" FORCE)
    set(GLAD_GENERATOR "c" CACHE STRING "" FORCE)
    FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad.git
        GIT_TAG        v0.1.36
    )
    # glad v0.1.36 still declares cmake_minimum_required(<3.5), which CMake 4.x
    # rejects outright. Allow the old policy for this fetched subproject only.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_MakeAvailable(glad)
endif()

add_library(nwiiruntime STATIC
    src/core/loader.cpp
    src/core/config.cpp
    src/core/diagnostics.cpp
    src/core/perf_overlay.cpp
    src/core/os_hle_stubs.cpp
    src/core/event_scheduler.cpp
    src/input/input_manager.cpp
    src/input/sources/mouse_keyboard_source.cpp
    src/input/sources/gamepad_source.cpp
    src/input/sources/phone_source.cpp
    src/hle/memory.cpp
    src/hle/gx.cpp
    src/hle/pad.cpp
    src/hle/dvd.cpp
    src/hle/gx/fifo_parser.cpp
    src/hle/gx/gx_matrix_seam.cpp
    src/hle/gx/gx_state_seam.cpp
    src/hle/gx/gx_state_seam.cpp
    src/hle/gx/renderer_gl.cpp
    src/hle/gx/renderer_factory.cpp
    src/hle/gx/tev_shader_gen.cpp
    src/hle/texture_cache_gl.cpp
    src/hle/ax.cpp
    src/hle/ax_hle.cpp
    src/hle/bink_video.cpp
    src/hle/movie_audio.cpp
    src/hle/ios.cpp
    src/hle/ios_kernel.cpp
    src/hle/virtual_disc.cpp
    src/hw/mmio_dispatcher.cpp
    src/platform/platform.cpp
    src/platform/wii_platform.cpp
    src/platform/gc_platform.cpp
    src/hw/hw_pi.cpp
    src/hw/hw_pe.cpp
    src/hw/hw_vi.cpp
    src/hw/hw_dsp.cpp
    src/hw/hw_ai.cpp
    src/hw/hw_di.cpp
    src/hw/hw_ipc.cpp
    src/hw/hw_all.cpp
    src/hw/hw_cp.cpp
    src/hle/devices/di_device.cpp
    src/hle/devices/fs_device.cpp
    src/hle/devices/stm_device.cpp
    src/hle/devices/usb_device.cpp
    src/hle/devices/es_device.cpp
    src/hle/interpreter.cpp
)

# Optional native decoder for the Rayman Bink hook. Keep a guest-code
# fallback in exports built without FFmpeg development files.
# Zig cross builds do not populate CMAKE_LIBRARY_ARCHITECTURE.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_av_multiarch "${CMAKE_SYSTEM_PROCESSOR}-linux-gnu")
endif()
find_path(NWII_AVCODEC_INCLUDE libavcodec/avcodec.h PATH_SUFFIXES ${_av_multiarch})
find_path(NWII_AVUTIL_INCLUDE libavutil/frame.h PATH_SUFFIXES ${_av_multiarch})
find_library(NWII_AVCODEC avcodec PATH_SUFFIXES ${_av_multiarch})
find_library(NWII_AVUTIL avutil PATH_SUFFIXES ${_av_multiarch})
if(NWII_AVCODEC_INCLUDE AND NWII_AVUTIL_INCLUDE AND NWII_AVCODEC AND NWII_AVUTIL)
    set_source_files_properties(src/hle/bink_video.cpp PROPERTIES
        COMPILE_DEFINITIONS NWII_HAVE_BINK_DECODER
        INCLUDE_DIRECTORIES "${NWII_AVCODEC_INCLUDE};${NWII_AVUTIL_INCLUDE}")
    target_link_libraries(nwiiruntime PRIVATE "${NWII_AVCODEC}" "${NWII_AVUTIL}")
endif()

if(NWII_SDL_INCLUDE_DIRS)
    target_include_directories(nwiiruntime PUBLIC ${NWII_SDL_INCLUDE_DIRS})
endif()
if(NWII_SDL_LIBRARY_DIRS)
    target_link_directories(nwiiruntime PUBLIC ${NWII_SDL_LIBRARY_DIRS})
endif()
target_link_libraries(nwiiruntime PUBLIC
    ${NWII_SDL_LIBRARIES}
    tomlplusplus::tomlplusplus
    glad)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(nwiiruntime PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# OpenGL ES 3.0 shares the GL renderer and loader. Keep an explicit switch so a
# build can refuse the profile instead of silently producing a GL-only binary;
# the runtime reports the refusal when "OpenGLES" is selected.
option(NWII_ENABLE_GLES "Enable the OpenGL ES 3.0 rendering profile" ON)
if(NWII_ENABLE_GLES)
    target_compile_definitions(nwiiruntime PUBLIC NWII_ENABLE_GLES=1)
else()
    target_compile_definitions(nwiiruntime PUBLIC NWII_ENABLE_GLES=0)
endif()

# macOS has no native OpenGL ES driver; ANGLE supplies ES 3.0 on top of Metal.
# SDL loads libEGL/libGLESv2 dynamically by name, so point it at the chosen
# distribution at build time. The runtime only sets SDL's driver paths when the
# caller has not set them already.
set(NWII_ANGLE_DIR "" CACHE PATH
    "ANGLE directory containing libEGL.dylib and libGLESv2.dylib (macOS)")
if(NWII_ANGLE_DIR)
    if(NOT APPLE)
        message(FATAL_ERROR
            "NWII_ANGLE_DIR is currently supported for the macOS ANGLE dylibs "
            "only; set SDL_VIDEO_GL_DRIVER/SDL_VIDEO_EGL_DRIVER at run time for "
            "other platforms.")
    endif()
    if(NOT EXISTS "${NWII_ANGLE_DIR}/libEGL.dylib" OR
       NOT EXISTS "${NWII_ANGLE_DIR}/libGLESv2.dylib")
        message(FATAL_ERROR
            "NWII_ANGLE_DIR must contain libEGL.dylib and libGLESv2.dylib: "
            "${NWII_ANGLE_DIR}")
    endif()
    target_compile_definitions(nwiiruntime PUBLIC
        NWII_ANGLE_DIR="${NWII_ANGLE_DIR}")
    message(STATUS "OpenGL ES via ANGLE: ${NWII_ANGLE_DIR}")
endif()

if(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(METAL_LIBRARY Metal)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(IOKIT_LIBRARY IOKit)
    find_library(COREVIDEO_LIBRARY CoreVideo)
    target_link_libraries(nwiiruntime PUBLIC 
        ${COCOA_LIBRARY} 
        ${METAL_LIBRARY} 
        ${QUARTZCORE_LIBRARY}
        ${IOKIT_LIBRARY} 
        ${COREVIDEO_LIBRARY}
    )
    target_sources(nwiiruntime PRIVATE src/hle/gx/renderer_metal.mm)
endif()

target_include_directories(nwiiruntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Game profiles are looked up as assets/games/<game id>.toml relative to the
# working directory. Place them next to the built executable so a run from the
# build directory finds them.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets DESTINATION ${CMAKE_BINARY_DIR})

# add_executable(nwiiruntime_exe src/main.cpp)
# target_link_libraries(nwiiruntime_exe PRIVATE nwiiruntime)

# target_link_libraries(nwiiruntime_exe PRIVATE nwiirecomp_lib)
