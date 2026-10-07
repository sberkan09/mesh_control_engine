cmake_minimum_required(VERSION 3.28)

project(MeshControlEngine
    VERSION 0.0.1
    DESCRIPTION "Mesh Control Engine (MCE)"
    LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset <name>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MCE_BUILD_TESTS     "Build unit tests"                         ON)
option(MCE_ENABLE_TRACY    "Compile the Tracy client in (on-demand)"  ON)
option(MCE_FORCE_ASSERTS   "Keep MCE_ASSERT active in release builds" OFF)
option(MCE_WARNINGS_AS_ERRORS "Treat warnings as errors"              ON)
option(MCE_ASAN            "Build with AddressSanitizer"              OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(MceTarget)
include(Dependencies)

if(MCE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src/engine/core)
add_subdirectory(src/engine/refl)

if(MCE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
