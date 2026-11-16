cmake_minimum_required(VERSION 3.20)
project(nWiiRecomp LANGUAGES C CXX)

# Only the macOS Metal renderer needs Objective-C. Keeping these languages off
# other platforms lets a Linux ARM64 cross toolchain configure this project
# without an Objective-C compiler for the target.
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# Dolphin SDK signature database (GPLv2+ data from dolphin-emu, referenced by
# the config TOMLs as third_party/dolphin-sigdb/totaldb.dsy). Fetched on demand
# so the blob does not have to live in this repository.
set(NWII_SIGDB "${CMAKE_SOURCE_DIR}/third_party/dolphin-sigdb/totaldb.dsy")
if(NOT EXISTS "${NWII_SIGDB}")
    message(STATUS "totaldb.dsy not found - downloading from dolphin-emu")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Data/Sys/totaldb.dsy"
        "${NWII_SIGDB}"
        EXPECTED_HASH SHA256=09fb4a167946dbdfee5890c0f027e40b3eacbb6cc5ee9184adeb9d1a81c2c0aa
        STATUS NWII_SIGDB_STATUS
        SHOW_PROGRESS)
    list(GET NWII_SIGDB_STATUS 0 NWII_SIGDB_CODE)
    if(NOT NWII_SIGDB_CODE EQUAL 0)
        file(REMOVE "${NWII_SIGDB}")
        message(FATAL_ERROR
            "totaldb.dsy download failed (${NWII_SIGDB_STATUS}). Fetch it "
            "manually from the Dolphin repository (Data/Sys/totaldb.dsy) into "
            "third_party/dolphin-sigdb/. If upstream changed the file, update "
            "EXPECTED_HASH here.")
    endif()
endif()

FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# SDL2: fetched 2.28.5 by default, or the target's system SDL2 with
# -DNWII_SYSTEM_SDL2=ON (the Linux ARM64 cross-build path).
include("${CMAKE_SOURCE_DIR}/nWiiRuntime/cmake/sdl.cmake")

add_subdirectory(nWiiAnalyzer)
add_subdirectory(nWiiRecomp)
add_subdirectory(nWiiRuntime)

option(NWII_BUILD_STUDIO "Build nWiiStudio GUI" OFF)
if(NWII_BUILD_STUDIO)
    add_subdirectory(nWiiStudio)
endif()

option(NWII_BUILD_TESTS "Build recompiler regression tests" OFF)
if(NWII_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
