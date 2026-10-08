# glslang + SPIRV-Cross, pinned by tag (ADR-0017 policy; ADR-0028 for why).
#
# The engine's shaders are GLSL 450 core source (rhi::ShaderSource). macOS caps OpenGL at 4.1, so the
# Metal backend translates them at pipeline creation: glslang to SPIR-V (OpenGL client semantics),
# SPIRV-Cross to MSL. Included only by src/engine/rhi_metal/CMakeLists.txt, so only macOS builds fetch
# them; private to mce_rhi_metal (the "shader-cross wall" in scripts/ci/check_walls.py).

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        16.6.0
    GIT_SHALLOW    TRUE
    SYSTEM)

set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG        vulkan-sdk-1.4.363.0
    GIT_SHALLOW    TRUE
    SYSTEM)
FetchContent_MakeAvailable(glslang spirv_cross)
foreach(t glslang glslang-default-resource-limits spirv-cross-core spirv-cross-glsl spirv-cross-msl)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_compile_options(${t} PRIVATE -w)
    endif()
endforeach()
