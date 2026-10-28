# mce_bench (M-BENCH): headless micro-benchmarks of the engine's hot paths (ECS iteration, spawn and destroy,
# archetype moves, job dispatch). Prints a table or --json; a number is the machine's, not a budget.
mce_add_executable(mce_bench
    SOURCES
        main.cpp
    DEPS
        mce::mce_ecs
        mce::mce_core)
