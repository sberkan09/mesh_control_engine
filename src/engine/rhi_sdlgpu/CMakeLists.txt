# mce_rhi_sdlgpu (L2): the SDL3 GPU backend of mce_rhi (ADR-0038) — Vulkan on Linux and Android,
# Metal on iOS. SDL3, glslang and (Apple) SPIRV-Cross are PRIVATE: SDL GPU types and the shader
# translator never leak past this target (the SDL GPU wall and the shader-cross wall in check_walls.py).

set(_mce_sdlgpu_sources src/sdlgpu_device.cpp src/sdlgpu_translate.cpp src/shader_spirv.cpp)
set(_mce_sdlgpu_deps)
if(APPLE)
    # SDL GPU's Metal driver takes MSL: SPIR-V -> MSL through SPIRV-Cross (shader_msl.h).
    include("${PROJECT_SOURCE_DIR}/cmake/macos/MetalDependencies.cmake")
    list(APPEND _mce_sdlgpu_sources src/shader_msl.cpp)
    list(APPEND _mce_sdlgpu_deps spirv-cross-msl)
endif()

mce_add_library(mce_rhi_sdlgpu
    SOURCES
        ${_mce_sdlgpu_sources}
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        SDL3::SDL3-static
        glslang::glslang
        glslang::glslang-default-resource-limits
        ${_mce_sdlgpu_deps})
if(APPLE)
    # SPIRV-Cross reports errors by throwing; this one file catches them and returns Results.
    set_source_files_properties(src/shader_msl.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
endif()
