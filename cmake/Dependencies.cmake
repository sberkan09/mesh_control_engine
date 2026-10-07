# Third-party dependencies, pinned by git tag (ADR-0017: FetchContent instead of vcpkg).
#
# Policy (ARCHITECTURE §7): mce_core links only spdlog + Tracy, ever. Every new dependency
# needs an ADR and an entry here. Cook-time-only deps must never reach mce_engine.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---- spdlog (logging facade backend) ---------------------------------------
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(SPDLOG_ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
    SYSTEM)

# ---- Tracy (profiler client) -------------------------------------------------
set(TRACY_ENABLE ${MCE_ENABLE_TRACY} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)
FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(spdlog tracy)

# ---- doctest (tests only) ----------------------------------------------------
if(MCE_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)   # header-only: we only need the include dir
    FetchContent_MakeAvailable(doctest)

    add_library(mce_doctest INTERFACE)
    target_include_directories(mce_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
    # USE_STD_HEADERS: doctest's std::ostream forward declaration breaks MSVC 14.5x headers.
    target_compile_definitions(mce_doctest INTERFACE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
        DOCTEST_CONFIG_USE_STD_HEADERS)
    add_library(mce::doctest ALIAS mce_doctest)
endif()
