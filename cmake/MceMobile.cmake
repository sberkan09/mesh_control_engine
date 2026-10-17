# Phone builds (M-MOBILE, ADR-0038): Android (the NDK toolchain, presets android-arm64-*) and iOS
# (presets ios-sim-arm64 / ios-arm64). Included by the root CMakeLists.txt before any target is
# defined. Sets MCE_MOBILE and wraps add_executable for the engine's own executables.
#
# Android turns every engine executable into the shared library SDL's Java activity loads:
#
#   - add_executable(<name> ...) for a target under the engine checkout becomes
#     add_library(<name> SHARED ...), so scripts/ship.py --target android drops lib<name>.so into
#     the APK and the activity (MceActivity, cmake/mobile/android) loads it by that name;
#   - its sources see cmake/mobile/mce_sdl_main.h first, which renames main() to the exported
#     extern "C" SDL_main() the activity calls. The game code keeps a plain main(); no SDL header
#     leaves mce_platform (the SDL wall).
#
# iOS builds every engine executable as an app bundle (<name>.app, MACOSX_BUNDLE) with
# cmake/mobile/ios/Info.plist.in, the same main() -> SDL_main() rename, and cmake/mobile/ios/ios_main.c,
# SDL's UIApplicationMain entry point that calls it. scripts/ship.py --target ios copies the pack
# into the bundle and signs it.
#
# A cross build cannot run the target's mce_cook, so the packs come from a host build:
# MCE_HOST_COOK names that mce_cook (scripts/ship.py builds it on the host preset first and passes
# it), and mce_sample_pack runs it in place of the target's. A project's cook carries the game's
# component types, so a project cooks with the host build's <game>_cook instead: MCE_HOST_BUILD_DIR
# names that host build tree (scripts/ship.py --project builds <game>_cook there first).

if(ANDROID OR IOS)
    set(MCE_MOBILE ON)
else()
    set(MCE_MOBILE OFF)
endif()

set(MCE_HOST_COOK "" CACHE FILEPATH "Host mce_cook used to cook packs in a cross build (Android/iOS)")
set(MCE_HOST_BUILD_DIR "" CACHE PATH "Host build tree whose projects/<name>/<game>_cook cook the projects in a cross build")
# Texture storage of the packs this build cooks (mce_cook --textures): phones take ASTC 4x4.
if(MCE_MOBILE)
    set(_mce_textures_default astc)
else()
    set(_mce_textures_default rgba8)
endif()
set(MCE_COOK_TEXTURES "${_mce_textures_default}" CACHE STRING "Texture storage of cooked packs: rgba8 or astc")
set(MCE_COOK_TEXTURE_ARGS)  # the build-time cooks' extra arguments (samples/CMakeLists.txt, MceProject.cmake)
if(MCE_COOK_TEXTURES STREQUAL "astc")
    set(MCE_COOK_TEXTURE_ARGS --textures astc)
endif()
# Cap on PNG/JPG textures in the packs this build cooks (mce_cook --max-texture-size; M-CONTENT 1a): a power of two,
# empty = no limit. scripts/ship.py --max-texture-size N sets it for one ship.
set(MCE_COOK_MAX_TEXTURE_SIZE "" CACHE STRING "Largest side of PNG/JPG textures in cooked packs: a power of two from 1 to 16384, empty for no limit")
if(MCE_COOK_MAX_TEXTURE_SIZE)
    list(APPEND MCE_COOK_TEXTURE_ARGS --max-texture-size ${MCE_COOK_MAX_TEXTURE_SIZE})
endif()

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

if(IOS)
    set(MCE_SDL_MAIN_HEADER "${CMAKE_CURRENT_LIST_DIR}/mobile/mce_sdl_main.h")
    set(MCE_IOS_DIR "${CMAKE_CURRENT_LIST_DIR}/mobile/ios")
    set(MCE_IOS_BUNDLE_PREFIX "org.meshcontrol" CACHE STRING "iOS: bundle identifier prefix (<prefix>.<target>)")

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
        _add_executable(${name} MACOSX_BUNDLE ${ARGN} "${MCE_IOS_DIR}/ios_main.c")
        target_compile_options(${name} PRIVATE "SHELL:-include \"${MCE_SDL_MAIN_HEADER}\"")
        target_link_libraries(${name} PRIVATE SDL3::Headers)
        string(REGEX REPLACE "[^A-Za-z0-9.-]" "-" bundle_id "${MCE_IOS_BUNDLE_PREFIX}.${name}")
        set_target_properties(${name} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${MCE_IOS_DIR}/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    endfunction()
endif()
