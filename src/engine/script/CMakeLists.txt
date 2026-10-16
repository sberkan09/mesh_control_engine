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
