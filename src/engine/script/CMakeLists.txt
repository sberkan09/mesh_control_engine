# mce_script (spike, ADR-0039): Luau scripts as ECS systems over reflected components.
#
# Opt-in like mce_net: not in MCE_ENGINE_LIBS, so libmce_engine never contains it. Only its test
# (tests/script) and samples/script_headless link it while the spike is evaluated. Luau's headers
# stay in src/ (the Luau wall in scripts/ci/check_walls.py).
mce_add_library(mce_script
    SOURCES
        src/script.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_refl
        mce::mce_ecs
    PRIVATE_DEPS
        Luau.VM
        Luau.Compiler)
