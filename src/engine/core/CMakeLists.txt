# mce_core (L0): allocators, containers, math, Result/Error, log/assert, convars, jobs, app loop.
# Dependency floor (binding): links only spdlog + Tracy, ever.

mce_add_library(mce_core
    SOURCES
        src/app.cpp
        src/assert.cpp
        src/convar.cpp
        src/crash.cpp
        src/hash.cpp
        src/jobs.cpp
        src/log.cpp
        src/math.cpp
        src/memory.cpp
        src/perf_capture.cpp
        src/result.cpp
        src/version.cpp
    PUBLIC_DEPS
        spdlog::spdlog
        Tracy::TracyClient)

# The crash handler (mce/crash.h): dladdr/dl_iterate_phdr live in libdl before glibc 2.34. Windows
# loads dbghelp at install time instead of linking it.
if(NOT WIN32)
    target_link_libraries(mce_core PRIVATE ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
    target_link_libraries(mce_core PUBLIC log)  # the logcat sink (log.cpp)
endif()
# mce/version.h reads the configure-time engine stamp (cmake/EngineId.cmake).
target_include_directories(mce_core PRIVATE "${MCE_GENERATED_INCLUDE_DIR}")

target_compile_definitions(mce_core PUBLIC
    SPDLOG_NO_EXCEPTIONS
    $<IF:$<CONFIG:Debug>,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE,SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>)
