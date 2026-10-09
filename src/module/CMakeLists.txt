# Game-module boundary (scripting.md §2, §3.1; ARCHITECTURE §2.1 `src/module`).
#
#   mce_module_host   the loader (static; part of the umbrella)
#   mce_engine        the umbrella shared library: every runtime engine library linked whole into
#                     one image, so a host and its game module share one engine instance (one
#                     convar registry, one log, one allocator state)
#   mce_module_api    what a game module links: mce_engine + MCE_ENGINE_ID + the compile policy
#
#   mce_add_game_module(<name> SOURCES ... [DEPS ...])   builds a game module (cmake/MceTarget.cmake)
#
# Hosts that load modules link mce::mce_engine and NO static engine library (that would be a
# second engine instance): the player runtime, and the editor, the MCP server and the cook with
# their libraries (UMBRELLA targets). On Windows the umbrella needs the M4 export decorations
# (MCE_API + the explicit-instantiation ledger, finding F33), so it is not built there yet.

mce_add_library(mce_module_host
    SOURCES
        src/module_host.cpp
    PUBLIC_DEPS
        mce::mce_ecs
    PRIVATE_DEPS
        mce::mce_platform)
target_include_directories(mce_module_host PUBLIC "${MCE_GENERATED_INCLUDE_DIR}")

# MCE_ENGINE_LIBS, MCE_GAME_MODULES_SUPPORTED and the $ORIGIN run paths are set by the root
# CMakeLists.txt (before the first executable), and UMBRELLA targets (cmake/MceTarget.cmake) link
# the umbrella defined here in place of the static libraries.
if(NOT MCE_GAME_MODULES_SUPPORTED)
    message(STATUS "mce_engine shared library: not built on Windows until the M4 export ledger")
    function(mce_add_game_module name)
    endfunction()
    return()
endif()

add_library(mce_engine SHARED src/engine_dll.cpp)
add_library(mce::mce_engine ALIAS mce_engine)
# Every object of every engine library goes in (whole archive), so static-init registrations
# (convars) and symbols only a module uses are present. The plain list after it supplies the
# third-party dependencies; CMake's WHOLE_ARCHIVE feature cannot be used because the engine
# libraries also reach the link line transitively without it.
set(_mce_whole_archive "")
foreach(lib ${MCE_ENGINE_LIBS})
    list(APPEND _mce_whole_archive "$<TARGET_FILE:${lib}>")
endforeach()
if(APPLE)
    # ld64 has no --whole-archive: -force_load per archive. Undefined symbols are already a link
    # error for dylibs by default (the --no-undefined below).
    set(_mce_force_load "")
    foreach(lib ${_mce_whole_archive})
        list(APPEND _mce_force_load "-Wl,-force_load,${lib}")
    endforeach()
    target_link_libraries(mce_engine
        PRIVATE ${_mce_force_load} ${MCE_ENGINE_LIBS}
        INTERFACE "$<COMPILE_ONLY:${MCE_ENGINE_LIBS}>")
else()
    target_link_libraries(mce_engine
        PRIVATE "-Wl,--whole-archive" ${_mce_whole_archive} "-Wl,--no-whole-archive" ${MCE_ENGINE_LIBS}
        INTERFACE "$<COMPILE_ONLY:${MCE_ENGINE_LIBS}>")
    if(NOT MCE_ASAN)  # sanitizer runtimes resolve at load time
        target_link_options(mce_engine PRIVATE "LINKER:--no-undefined")
    endif()
endif()
mce_apply_compile_policy(mce_engine)

# mce_module_api: what a game module links. mce_add_game_module itself lives in
# cmake/MceTarget.cmake, which the installed SDK ships too (cmake/MceInstall.cmake).
add_library(mce_module_api INTERFACE)
add_library(mce::mce_module_api ALIAS mce_module_api)
target_link_libraries(mce_module_api INTERFACE mce::mce_engine)
