# mce_rhi_metal (L2): the Metal backend of mce_rhi on macOS (ADR-0027) — the only Metal code in the
# engine. SDL3, glslang and SPIRV-Cross are PRIVATE: Metal/ObjC types and the shader translator never
# leak past this target (the Metal wall and the shader-cross wall in check_walls.py).

include("${PROJECT_SOURCE_DIR}/cmake/macos/MetalDependencies.cmake")

mce_add_library(mce_rhi_metal
    SOURCES
        src/metal_device.mm
        src/metal_translate.mm
        src/shader_cross.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        SDL3::SDL3-static
        glslang::glslang
        glslang::glslang-default-resource-limits
        spirv-cross-msl
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation")
# Objective-C++ under manual reference counting (metal_device.h explains why). Compiled through the
# CXX language so the project needs no OBJCXX toolchain setup and the .mm files get the same policy.
set_source_files_properties(src/metal_device.mm src/metal_translate.mm PROPERTIES
    LANGUAGE CXX
    COMPILE_OPTIONS "-xobjective-c++;-fno-objc-arc")
# SPIRV-Cross reports errors by throwing; this one file catches them and returns Results.
set_source_files_properties(src/shader_cross.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
