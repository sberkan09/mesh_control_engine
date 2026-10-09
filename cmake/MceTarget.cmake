# Shared compile policy for every MCE target (core.md §1.4).
#
#  - no exceptions, no RTTI in engine targets (/EHs-c- /GR-, -fno-exceptions -fno-rtti)
#  - precise floating point everywhere (/fp:precise, -ffp-contract=off): determinism (ADR-0007)
#  - warnings are errors
#
# Helpers:
#   mce_add_library(<name> [UMBRELLA] SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
#   mce_add_executable(<name> [UMBRELLA] SOURCES ... [DEPS ...])
#   mce_add_test(<name> [UMBRELLA] [NO_CTEST] SOURCES ... [DEPS ...])
#   mce_engine_deps(<out> <deps...>)
#
# UMBRELLA marks a target that loads game modules, or is linked by one that does: the editor, the
# MCP server, the cook and their libraries. Where game modules are supported such a target links
# the mce_engine shared library (src/module) in place of every static engine library in its deps,
# so it and the module it loads share one engine instance (one convar registry, one reflection
# registry, one allocator). Linking a static engine library next to the umbrella would silently
# make a second engine, so the helpers refuse an UMBRELLA library in a non-UMBRELLA target. Where
# modules are not supported (Windows) UMBRELLA changes nothing.

function(mce_apply_compile_policy target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8
            /EHs-c- /GR- /fp:precise
            /wd4324)  # C4324: padding from alignas(64) on SPSC/MPSC cursors is intended
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0 NOMINMAX WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS)
        if(MCE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(MCE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
            -fno-exceptions -fno-rtti -ffp-contract=off)
        if(MCE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(MCE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    if(MCE_FORCE_ASSERTS)
        target_compile_definitions(${target} PRIVATE MCE_FORCE_ASSERTS=1)
    endif()
endfunction()

# <out> = <deps> with every engine library (MCE_ENGINE_LIBS, with or without the mce:: prefix)
# replaced by mce::mce_engine where game modules are supported; unchanged elsewhere. The umbrella's
# interface carries the engine libraries compile-only, so headers resolve as before.
function(mce_engine_deps out)
    if(NOT MCE_GAME_MODULES_SUPPORTED)
        set(${out} ${ARGN} PARENT_SCOPE)
        return()
    endif()
    set(result)
    set(on_umbrella FALSE)
    foreach(dep IN LISTS ARGN)
        string(REGEX REPLACE "^mce::" "" bare "${dep}")
        if(bare IN_LIST MCE_ENGINE_LIBS OR bare STREQUAL "mce_engine")
            set(on_umbrella TRUE)
        else()
            list(APPEND result "${dep}")
        endif()
    endforeach()
    if(on_umbrella)
        list(PREPEND result mce::mce_engine)
    endif()
    set(${out} ${result} PARENT_SCOPE)
endfunction()

# Fails the configure when a non-UMBRELLA target links an UMBRELLA library (a second engine).
function(_mce_check_umbrella_deps target umbrella)
    if(umbrella OR NOT MCE_GAME_MODULES_SUPPORTED)
        return()
    endif()
    foreach(dep IN LISTS ARGN)
        string(REGEX REPLACE "^mce::" "" bare "${dep}")
        if(TARGET ${bare})
            get_target_property(is_umbrella ${bare} MCE_UMBRELLA)
            if(is_umbrella)
                message(FATAL_ERROR "${target} links ${dep}, an UMBRELLA library: add UMBRELLA to ${target} "
                                    "so it links mce_engine instead of the static engine libraries")
            endif()
        endif()
    endforeach()
endfunction()

function(mce_add_library name)
    cmake_parse_arguments(ARG "UMBRELLA" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    add_library(mce::${name} ALIAS ${name})
    target_include_directories(${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    _mce_check_umbrella_deps(${name} ${ARG_UMBRELLA} ${ARG_PUBLIC_DEPS} ${ARG_PRIVATE_DEPS})
    if(ARG_UMBRELLA)
        mce_engine_deps(ARG_PUBLIC_DEPS ${ARG_PUBLIC_DEPS})
        mce_engine_deps(ARG_PRIVATE_DEPS ${ARG_PRIVATE_DEPS})
        set_target_properties(${name} PROPERTIES MCE_UMBRELLA TRUE)
    endif()
    target_link_libraries(${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    mce_apply_compile_policy(${name})
endfunction()

function(mce_add_executable name)
    cmake_parse_arguments(ARG "UMBRELLA" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    _mce_check_umbrella_deps(${name} ${ARG_UMBRELLA} ${ARG_DEPS})
    if(ARG_UMBRELLA)
        mce_engine_deps(ARG_DEPS ${ARG_DEPS})
    endif()
    target_link_libraries(${name} PRIVATE ${ARG_DEPS})
    mce_apply_compile_policy(${name})
endfunction()

# NO_CTEST builds the executable without registering it (GPU tests register themselves under
# MCE_GPU_TESTS). An UMBRELLA test compiles tests/test_main.cpp in directly: mce::doctest_main
# would bring a static mce_core next to the umbrella.
function(mce_add_test name)
    cmake_parse_arguments(ARG "UMBRELLA;NO_CTEST" "" "SOURCES;DEPS" ${ARGN})
    find_package(Threads REQUIRED)
    _mce_check_umbrella_deps(${name} ${ARG_UMBRELLA} ${ARG_DEPS})
    if(ARG_UMBRELLA AND MCE_GAME_MODULES_SUPPORTED)
        add_executable(${name} ${ARG_SOURCES} "${PROJECT_SOURCE_DIR}/tests/test_main.cpp")
        target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
        mce_engine_deps(ARG_DEPS ${ARG_DEPS})
        target_link_libraries(${name} PRIVATE ${ARG_DEPS} mce::mce_engine mce::doctest Threads::Threads)
    else()
        add_executable(${name} ${ARG_SOURCES})
        target_link_libraries(${name} PRIVATE ${ARG_DEPS} mce::doctest_main Threads::Threads)
    endif()
    mce_apply_compile_policy(${name})
    if(NOT ARG_NO_CTEST)
        add_test(NAME ${name} COMMAND ${name})
    endif()
endfunction()
