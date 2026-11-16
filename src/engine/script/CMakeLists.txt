# mce_script (ADR-0041, M-SCRIPT): Luau scripts as ECS systems over reflected components.
#
#   mce_script           the VM host and the scene-script runner (mce/script/script.h,
#                        scene_scripts.h): loads bytecode only. The stock hosts link it (mce_player,
#                        the editor, mce_mcp) where MCE_WITH_SCRIPT is on.
#   mce_script_compiler  Luau source -> bytecode (mce/script/compiler.h): the cook, the editor and
#                        tests link it; the player never does (the compiler wall).
#
# Opt-in like mce_net: not in MCE_ENGINE_LIBS, so libmce_engine never contains Luau. UMBRELLA:
# where game modules are supported the engine dependencies resolve against libmce_engine, so a
# host's scripts and its game module share one engine. Luau's headers stay in src/ and compiler/
# (the Luau wall in scripts/ci/check_walls.py).
mce_add_library(mce_script UMBRELLA
    SOURCES
        src/script.cpp
        src/api_world.cpp
        src/api_missions.cpp
        src/api_inventory.cpp
        src/api_status.cpp
        src/api_clock.cpp
        src/api_weather.cpp
        src/scene_scripts.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_refl
        mce::mce_ecs
        mce::mce_asset
        mce::mce_gameplay_kit
    PRIVATE_DEPS
        Luau.VM)
target_compile_definitions(mce_script INTERFACE MCE_HAS_SCRIPT=1)
# MSVC 14.51 sees luaL_error as [[noreturn]], so the `return 0;` after it is C4702 (unreachable code) under /WX;
# GCC and Clang do not warn. The returns stay for the compilers that need them.
if(MSVC)
    target_compile_options(mce_script PRIVATE /wd4702)
endif()

mce_add_library(mce_script_compiler UMBRELLA
    SOURCES
        compiler/compiler.cpp
        compiler/definitions.cpp
        compiler/names_check.cpp
        compiler/state_check.cpp
        compiler/typecheck.cpp
    PUBLIC_DEPS
        mce::mce_script
    PRIVATE_DEPS
        Luau.Compiler
        Luau.Analysis
        Luau.Ast)
# Luau's analysis throws on internal limits; typecheck.cpp (alone in the engine) catches them.
set_source_files_properties(compiler/typecheck.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/EHsc,-fexceptions>")
# MSVC: these TUs include Luau's C++ headers, whose ParseError/ParseErrors derive from std::exception. The engine
# policy's _HAS_EXCEPTIONS=0 swaps the STL's std::exception for a smaller class, so this target and Luau (built with
# exceptions) disagree on the layout of every Luau error object; the inline destructors each side emits are COMDATs
# the linker picks one of, and any syntax error then crashed the cook (SIGSEGV in test_script, test_fuzz_script,
# player_scripts). The compiler never ships in the player (the compiler wall), so it builds like Luau does.
# libstdc++/libc++ keep one std::exception layout under -fno-exceptions, so GCC and Clang need nothing here.
if(MSVC)
    get_target_property(_mce_sc_defs mce_script_compiler COMPILE_DEFINITIONS)
    list(REMOVE_ITEM _mce_sc_defs _HAS_EXCEPTIONS=0)
    set_property(TARGET mce_script_compiler PROPERTY COMPILE_DEFINITIONS ${_mce_sc_defs})
    get_target_property(_mce_sc_opts mce_script_compiler COMPILE_OPTIONS)
    list(REMOVE_ITEM _mce_sc_opts /EHs-c-)
    set_property(TARGET mce_script_compiler PROPERTY COMPILE_OPTIONS ${_mce_sc_opts})
    target_compile_options(mce_script_compiler PRIVATE /EHsc)
endif()
