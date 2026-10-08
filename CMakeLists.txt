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
# The engine libraries (and their static third-party deps) are linked into the mce_engine shared
# library that game modules and their hosts share (scripting.md §3.1), so everything is PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MCE_BUILD_TESTS     "Build unit tests"                         ON)
option(MCE_ENABLE_TRACY    "Compile the Tracy client in (on-demand)"  ON)
option(MCE_FORCE_ASSERTS   "Keep MCE_ASSERT active in release builds" OFF)
option(MCE_WARNINGS_AS_ERRORS "Treat warnings as errors"              ON)
option(MCE_ASAN            "Build with AddressSanitizer"              OFF)
option(MCE_BUILD_SAMPLES   "Build the sample ladder (samples/)"       ON)
option(MCE_BUILD_EDITOR    "Build the editor (mce_editor_core/layer + mce_editor)" ON)
option(MCE_GPU_TESTS       "Register tests that need a display + GL 4.5 driver" OFF)
option(MCE_PHYSICS_ORACLE  "Link Box2D v3 into test_physics2d as the solver oracle (ADR-0016)" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(MceTarget)
include(MceShip)
include(Dependencies)
include(EngineId)

if(MCE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src/engine/core)
add_subdirectory(src/engine/refl)
add_subdirectory(src/engine/asset)
add_subdirectory(src/engine/ecs)
add_subdirectory(src/engine/physics2d)
add_subdirectory(src/engine/physics3d)
add_subdirectory(src/engine/platform)
add_subdirectory(src/engine/input)
add_subdirectory(src/engine/rhi)
add_subdirectory(src/engine/rhi_gl)
add_subdirectory(src/engine/render)
add_subdirectory(src/engine/audio)
add_subdirectory(src/engine/scene)
add_subdirectory(src/engine/kit)
add_subdirectory(tools/cook)
add_subdirectory(src/module)
add_subdirectory(src/player)

if(MCE_BUILD_EDITOR)
    add_subdirectory(src/editor)
endif()

if(MCE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(MCE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
