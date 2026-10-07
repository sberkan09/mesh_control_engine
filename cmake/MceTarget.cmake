# Shared compile policy for every MCE target (core.md §1.4).
#
#  - no exceptions, no RTTI in engine targets (/EHs-c- /GR-, -fno-exceptions -fno-rtti)
#  - precise floating point everywhere (/fp:precise, -ffp-contract=off): determinism (ADR-0007)
#  - warnings are errors
#
# Helpers:
#   mce_add_library(<name> [STATIC] SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
#   mce_add_executable(<name> SOURCES ... [DEPS ...])
#   mce_add_test(<name> SOURCES ... [DEPS ...])

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

function(mce_add_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    add_library(${name} STATIC ${ARG_SOURCES})
    add_library(mce::${name} ALIAS ${name})
    target_include_directories(${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${name} PUBLIC ${ARG_PUBLIC_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    mce_apply_compile_policy(${name})
endfunction()

function(mce_add_executable name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_DEPS})
    mce_apply_compile_policy(${name})
endfunction()

function(mce_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PRIVATE ${ARG_DEPS} mce::doctest_main Threads::Threads)
    mce_apply_compile_policy(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()
