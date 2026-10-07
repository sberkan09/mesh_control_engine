# mce_core (L0): allocators, containers, math, Result/Error, log/assert, convars, jobs, app loop.
# Dependency floor (binding): links only spdlog + Tracy, ever.

mce_add_library(mce_core
    SOURCES
        src/app.cpp
        src/assert.cpp
        src/convar.cpp
        src/hash.cpp
        src/jobs.cpp
        src/log.cpp
        src/math.cpp
        src/memory.cpp
        src/result.cpp
    PUBLIC_DEPS
        spdlog::spdlog
        Tracy::TracyClient)

target_compile_definitions(mce_core PUBLIC
    SPDLOG_NO_EXCEPTIONS
    $<IF:$<CONFIG:Debug>,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>)
