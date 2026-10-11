# Phone builds (M-MOBILE, ADR-0038): Android (the NDK toolchain, presets android-arm64-*) and iOS.
# Included by the root CMakeLists.txt before any target is defined. Sets MCE_MOBILE and, on
# Android, turns every engine executable into the shared library SDL's Java activity loads:
#
#   - add_executable(<name> ...) for a target under the engine checkout becomes
#     add_library(<name> SHARED ...), so scripts/ship.py --target android drops lib<name>.so into
#     the APK and the activity (MceActivity, cmake/mobile/android) loads it by that name;
#   - its sources see cmake/mobile/mce_sdl_main.h first, which renames main() to the exported
#     extern "C" SDL_main() the activity calls. The game code keeps a plain main(); no SDL header
#     leaves mce_platform (the SDL wall).
#
# A cross build cannot run the target's mce_cook, so the packs come from a host build:
# MCE_HOST_COOK names that mce_cook (scripts/ship.py builds it on the host preset first and passes
# it), and mce_sample_pack runs it in place of the target's.

if(ANDROID OR IOS)
    set(MCE_MOBILE ON)
else()
    set(MCE_MOBILE OFF)
endif()

set(MCE_HOST_COOK "" CACHE FILEPATH "Host mce_cook used to cook packs in a cross build (Android/iOS)")

if(ANDROID)
    set(MCE_SDL_MAIN_HEADER "${CMAKE_CURRENT_LIST_DIR}/mobile/mce_sdl_main.h")

    function(add_executable name)
        if(ARGC GREATER 1 AND (ARGV1 STREQUAL "IMPORTED" OR ARGV1 STREQUAL "ALIAS"))
            _add_executable(${ARGV})
            return()
        endif()
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE ours)
        if(NOT ours OR CMAKE_CURRENT_SOURCE_DIR MATCHES "/third_party/")
            _add_executable(${ARGV})
            return()
        endif()
        add_library(${name} SHARED ${ARGN})
        target_compile_options(${name} PRIVATE "SHELL:-include \"${MCE_SDL_MAIN_HEADER}\"")
        # SDL's activity needs the window/input glue in the same image (SDL3 is static).
        target_link_libraries(${name} PRIVATE android log)
    endfunction()
endif()
