# mce_rhi_webgpu (L2): the WebGPU backend of mce_rhi for browser builds (ADR-0039). The WebGPU C API
# comes from Emscripten's emdawnwebgpu port; glslang and Tint translate the engine's GLSL to WGSL.
# All three are PRIVATE: no WebGPU type or shader translator leaks past this target (the WebGPU wall
# in check_walls.py).

# The translator sits in its own archive: Dawn's include/ (which Tint's targets export) carries a
# native webgpu.h that must not shadow the port's in the device sources.
add_library(mce_rhi_webgpu_wgsl STATIC src/shader_wgsl.cpp)
target_link_libraries(mce_rhi_webgpu_wgsl PUBLIC mce::mce_core PRIVATE
    glslang::glslang glslang::glslang-default-resource-limits mce_tint)
mce_apply_compile_policy(mce_rhi_webgpu_wgsl)

mce_add_library(mce_rhi_webgpu
    SOURCES
        src/webgpu_device.cpp
        src/webgpu_translate.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        mce_rhi_webgpu_wgsl)
target_compile_options(mce_rhi_webgpu PRIVATE --use-port=emdawnwebgpu)
# The port's JavaScript library and C++ glue are linked into whatever executable uses the backend.
target_link_options(mce_rhi_webgpu INTERFACE --use-port=emdawnwebgpu)
