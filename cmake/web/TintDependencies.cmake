# Tint, the WebGPU shader compiler from Dawn, for mce_rhi_webgpu (ADR-0039). Web builds only.
# Included by cmake/Dependencies.cmake when MCE_WEB is on.
#
# The engine's shaders are GLSL; the browser takes WGSL. mce_rhi_webgpu compiles GLSL to SPIR-V
# with the engine's glslang, and Tint's SPIR-V reader and WGSL writer turn that into WGSL at run
# time (shader_wgsl.cpp). Only those two Tint parts are built: no Dawn native backend, no other
# reader or writer. The WebGPU API itself comes from Emscripten's emdawnwebgpu port (--use-port).
#
# Dawn's own sources, without submodules: Tint needs only SPIRV-Headers, SPIRV-Tools and Abseil,
# which are fetched here at the revisions Dawn's DEPS pins for this tag. Dawn's third_party
# CMakeLists returns early under Emscripten before it adds SPIRV-Tools, so the SPIR-V targets are
# added first and Dawn finds them by name.

set(_mce_dawn_tag v20260930.214659)

set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG        86f980c731e62ae4eaf383d320449d71687936bf
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_Declare(spirv_tools
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG        35fe089ca8c614715bda29465ccfb093576331e2
    SYSTEM
    EXCLUDE_FROM_ALL)
# Abseil is only fetched here; Dawn adds it from DAWN_ABSEIL_DIR with its own warning options.
FetchContent_Declare(mce_absl
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG        20250814.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_Declare(dawn
    GIT_REPOSITORY https://github.com/google/dawn.git
    GIT_TAG        ${_mce_dawn_tag}
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""
    SYSTEM
    EXCLUDE_FROM_ALL)

foreach(_opt DAWN_BUILD_SAMPLES DAWN_BUILD_TESTS DAWN_BUILD_BENCHMARKS DAWN_BUILD_FUZZERS DAWN_BUILD_PROTOBUF
             DAWN_BUILD_NODE_BINDINGS DAWN_ENABLE_INSTALL DAWN_FETCH_DEPENDENCIES DAWN_USE_GLFW
             TINT_BUILD_TESTS TINT_BUILD_CMD_TOOLS TINT_BUILD_BENCHMARKS TINT_BUILD_FUZZERS TINT_BUILD_IR_BINARY
             TINT_BUILD_GLSL_WRITER TINT_BUILD_GLSL_VALIDATOR TINT_BUILD_HLSL_WRITER TINT_BUILD_MSL_WRITER
             TINT_BUILD_SPV_WRITER TINT_BUILD_WGSL_READER TINT_BUILD_NULL_WRITER
             TINT_ENABLE_IR_DUMPING TINT_ENABLE_IR_VALIDATION_ASSERTS TINT_ENABLE_INSTALL)
    set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(TINT_BUILD_SPV_READER ON CACHE BOOL "" FORCE)
set(TINT_BUILD_WGSL_WRITER ON CACHE BOOL "" FORCE)

block()
    # Abseil's headers test the deprecated __EMSCRIPTEN_major__ macro, which Emscripten turns into
    # a warning that Abseil's -Werror then fails on.
    string(APPEND CMAKE_C_FLAGS " -Wno-deprecated-pragma -w")
    string(APPEND CMAKE_CXX_FLAGS " -Wno-deprecated-pragma -w")
    FetchContent_MakeAvailable(spirv_headers spirv_tools mce_absl)
    set(DAWN_ABSEIL_DIR "${mce_absl_SOURCE_DIR}" CACHE STRING "" FORCE)
    set(DAWN_SPIRV_HEADERS_DIR "${spirv_headers_SOURCE_DIR}" CACHE STRING "" FORCE)
    set(DAWN_SPIRV_TOOLS_DIR "${spirv_tools_SOURCE_DIR}" CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(dawn)
endblock()

# What mce_rhi_webgpu links (shader_wgsl.cpp includes Tint headers as "src/tint/...").
add_library(mce_tint INTERFACE)
target_link_libraries(mce_tint INTERFACE
    tint_lang_spirv_reader tint_lang_wgsl_writer tint_lang_core_ir tint_api_common dawn_internal_config)
target_include_directories(mce_tint SYSTEM INTERFACE "${dawn_SOURCE_DIR}")
