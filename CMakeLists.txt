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
option(MCE_BUILD_PROJECTS  "Build every projects/<name>/ game (off for an SDK-only build: install.sh)" ON)
option(MCE_TEMPLATE_PROBE  "Stamp the project template into the build tree and build it (tests the generator)" ON)
option(MCE_PHYSICS_ORACLE  "Link Box2D v3 into test_physics2d as the solver oracle (ADR-0016)" ON)
option(MCE_WITH_NET        "Build the optional networking module mce_net (ENet; a game opts in, ADR-0035)" ON)
# ADR-0038: the SDL3 GPU backend (Vulkan on Linux/Android, Metal on iOS). Built on Linux so every
# local_ci.sh run exercises it through lavapipe; selected at run time with MCE_RHI=sdlgpu.
if(APPLE AND NOT IOS)
    set(_mce_sdlgpu_default OFF)  # macOS keeps mce_rhi_metal
else()
    set(_mce_sdlgpu_default ON)
endif()
option(MCE_RHI_SDLGPU      "Build the SDL3 GPU RHI backend mce_rhi_sdlgpu (ADR-0038)" ${_mce_sdlgpu_default})

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(MceMobile)  # MCE_MOBILE; on Android, executables become the activity's shared library
include(MceTarget)
include(MceShip)
include(MceProject)
include(Dependencies)
include(EngineId)

# The engine as one image (scripting.md §3.1, src/module): mce_engine is every runtime engine
# library linked whole into one shared library, so a host and the game module it loads share one
# engine instance. On Windows the umbrella needs the M4 export decorations (MCE_API + the
# explicit-instantiation ledger, finding F33), so modules are not supported there yet and every
# tool links the static libraries instead (mce_engine_deps in cmake/MceTarget.cmake).
# Phones link the game in too: iOS forbids loading code, and an Android build ships one library.
if(WIN32 OR MCE_MOBILE)
    set(MCE_GAME_MODULES_SUPPORTED OFF)
else()
    set(MCE_GAME_MODULES_SUPPORTED ON)
endif()
if(MCE_MOBILE)
    if(NOT MCE_RHI_SDLGPU)
        message(FATAL_ERROR "Android/iOS builds need MCE_RHI_SDLGPU=ON (ADR-0038)")
    endif()
    set(MCE_RHI_BACKEND mce_rhi_sdlgpu)  # the only backend on a phone
elseif(APPLE)
    set(MCE_RHI_BACKEND mce_rhi_metal)  # ADR-0028: rhi::create_default_device picks the native backend
else()
    set(MCE_RHI_BACKEND mce_rhi_gl)
endif()
set(MCE_ENGINE_LIBS
    mce_core mce_refl mce_asset mce_ecs mce_physics2d mce_physics3d mce_platform mce_input
    mce_rhi ${MCE_RHI_BACKEND} mce_rhi_default mce_render mce_ui mce_audio mce_scene mce_ecs_scene mce_gameplay_kit
    mce_module_host)
if(MCE_RHI_SDLGPU AND NOT MCE_MOBILE)
    list(APPEND MCE_ENGINE_LIBS mce_rhi_sdlgpu)
endif()
if(MCE_GAME_MODULES_SUPPORTED)
    # Relocatable run paths (scripts/ship.py): every binary looks next to itself first, and the
    # build-tree path to libmce_engine is recorded relative to $ORIGIN, so a shipped folder never
    # depends on (or silently loads from) the build tree. Set before any executable is defined.
    set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
    if(APPLE)
        set(CMAKE_BUILD_RPATH "@loader_path")  # macOS spelling of $ORIGIN
    else()
        set(CMAKE_BUILD_RPATH "\$ORIGIN")
    endif()
endif()

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
if(NOT MCE_MOBILE)
    add_subdirectory(src/engine/rhi_gl)
endif()
if(APPLE AND NOT MCE_MOBILE)
    add_subdirectory(src/engine/rhi_metal)  # ADR-0028: the macOS backend
endif()
if(MCE_RHI_SDLGPU)
    add_subdirectory(src/engine/rhi_sdlgpu)  # ADR-0038: the mobile backend, opt-in on desktop
endif()
add_subdirectory(src/engine/rhi_default)
add_subdirectory(src/engine/render)
add_subdirectory(src/engine/ui)
add_subdirectory(src/engine/audio)
add_subdirectory(src/engine/scene)
add_subdirectory(src/engine/kit)
if(MCE_WITH_NET)
    # Opt-in (ADR-0035): not in MCE_ENGINE_LIBS, so mce_engine and every game that does not link
    # mce::mce_net stay network-free.
    add_subdirectory(src/engine/net)
endif()
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tools/cook)  # a cross build cooks with the host's (MCE_HOST_COOK, cmake/MceMobile.cmake)
endif()
add_subdirectory(src/module)
add_subdirectory(src/player)

if(MCE_BUILD_EDITOR)
    add_subdirectory(src/editor)
endif()

if(MCE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# Projects made with scripts/new_project.py (assets.md §4.4): every projects/<name>/ with a CMakeLists.txt.
set(_mce_projects "")
if(MCE_BUILD_PROJECTS)
    file(GLOB _mce_projects LIST_DIRECTORIES true "${PROJECT_SOURCE_DIR}/projects/*")
endif()
foreach(_dir IN LISTS _mce_projects)
    if(EXISTS "${_dir}/CMakeLists.txt")
        add_subdirectory("${_dir}")
    endif()
endforeach()

# The generator's own test (ROADMAP M8 "the generator produces a compiling, cooking, shipping
# project"): a fresh project stamped from templates/project at configure time and built like any
# other, so a template change that stops compiling, cooking or running fails every CI preset.
if(MCE_TEMPLATE_PROBE AND MCE_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_probe_root "${CMAKE_BINARY_DIR}/template_probe")
    file(GLOB_RECURSE _template_files CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/templates/project/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/scripts/new_project.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/new_project.py" TemplateProbe --dest "${_probe_root}"
                --seed 1 --force
        RESULT_VARIABLE _probe_rc OUTPUT_QUIET)
    if(NOT _probe_rc EQUAL 0)
        message(FATAL_ERROR "scripts/new_project.py failed to stamp the template probe")
    endif()
    add_subdirectory("${_probe_root}/TemplateProbe" "${CMAKE_BINARY_DIR}/template_probe_build")
endif()

if(MCE_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# `cmake --install`: the SDK (tools, libmce_engine, headers, find_package(MCE), the project
# template and the mce command). Last, so every target it installs exists. Phones ship apps, not an SDK.
if(NOT MCE_MOBILE)
    include(MceInstall)
endif()
