# Game-module boundary (scripting.md §2, §3.1; ARCHITECTURE §2.1 `src/module`).
#
#   mce_module_host   the loader (static; part of the umbrella)
#   mce_engine        the umbrella shared library: every runtime engine library linked whole into
#                     one image, so a host and its game module share one engine instance (one
#                     convar registry, one log, one allocator state)
#   mce_module_api    what a game module links: mce_engine + MCE_ENGINE_ID + the compile policy
#
#   mce_add_game_module(<name> SOURCES ... [DEPS ...])   builds a game module (MODULE library)
#
# Hosts that load modules link mce::mce_engine and NO static engine library (that would be a
# second engine instance). On Windows the umbrella needs the M4 export decorations (MCE_API +
# the explicit-instantiation ledger, finding F33), so it is not built there yet.

mce_add_library(mce_module_host
    SOURCES
        src/module_host.cpp
    PUBLIC_DEPS
        mce::mce_ecs
    PRIVATE_DEPS
        mce::mce_platform)
target_include_directories(mce_module_host PUBLIC "${MCE_GENERATED_INCLUDE_DIR}")

set(MCE_ENGINE_LIBS
    mce_core mce_refl mce_asset mce_ecs mce_physics2d mce_platform mce_input
    mce_rhi ${MCE_RHI_BACKEND} mce_rhi_default mce_render mce_audio mce_scene mce_gameplay_kit mce_module_host)

if(WIN32)
    set(MCE_GAME_MODULES_SUPPORTED OFF CACHE INTERNAL "")
    message(STATUS "mce_engine shared library: not built on Windows until the M4 export ledger")
    function(mce_add_game_module name)
    endfunction()
    return()
endif()
set(MCE_GAME_MODULES_SUPPORTED ON CACHE INTERNAL "")
# Relocatable run paths (scripts/ship.py): every binary looks next to itself first, and the
# build-tree path to libmce_engine is recorded relative to $ORIGIN, so a shipped folder never
# depends on (or silently loads from) the build tree.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON PARENT_SCOPE)
set(CMAKE_BUILD_RPATH "\$ORIGIN" PARENT_SCOPE)

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
target_link_libraries(mce_engine
    PRIVATE "-Wl,--whole-archive" ${_mce_whole_archive} "-Wl,--no-whole-archive" ${MCE_ENGINE_LIBS}
    INTERFACE "$<COMPILE_ONLY:${MCE_ENGINE_LIBS}>")
if(NOT MCE_ASAN)  # sanitizer runtimes resolve at load time
    target_link_options(mce_engine PRIVATE "LINKER:--no-undefined")
endif()
mce_apply_compile_policy(mce_engine)

add_library(mce_module_api INTERFACE)
add_library(mce::mce_module_api ALIAS mce_module_api)
target_link_libraries(mce_module_api INTERFACE mce::mce_engine)

function(mce_add_game_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_library(${name} MODULE ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE mce::mce_module_api ${ARG_DEPS})
    # Only mce_game_module() is exported (MCE_MODULE_EXPORT); everything else stays inside.
    set_target_properties(${name} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(NOT MCE_ASAN)
        target_link_options(${name} PRIVATE "LINKER:--no-undefined")
    endif()
    mce_apply_compile_policy(${name})
endfunction()
