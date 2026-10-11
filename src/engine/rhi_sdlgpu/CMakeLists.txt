# mce_rhi_sdlgpu (L2): the SDL3 GPU backend of mce_rhi (ADR-0038) — Vulkan on Linux and Android,
# Metal on iOS. SDL3 and glslang are PRIVATE: SDL GPU types and the shader translator never leak
# past this target (the SDL GPU wall and the shader-cross wall in check_walls.py).

mce_add_library(mce_rhi_sdlgpu
    SOURCES
        src/sdlgpu_device.cpp
        src/sdlgpu_translate.cpp
        src/shader_spirv.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        SDL3::SDL3-static
        glslang::glslang
        glslang::glslang-default-resource-limits)
