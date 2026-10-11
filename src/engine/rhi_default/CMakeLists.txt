# mce_rhi_default (L2): rhi::create_default_device, the one place that picks the backend —
# mce_rhi_metal on macOS, mce_rhi_gl everywhere else (MCE_RHI_BACKEND, set by the root
# CMakeLists.txt), or mce_rhi_sdlgpu when platform::gpu_api() says so (ADR-0038). Hosts link this
# instead of a backend.

set(_mce_default_backends mce::${MCE_RHI_BACKEND})
if(MCE_RHI_SDLGPU AND NOT MCE_MOBILE)
    list(APPEND _mce_default_backends mce::mce_rhi_sdlgpu)
endif()
mce_add_library(mce_rhi_default
    SOURCES
        src/default_device.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        ${_mce_default_backends})
if(MCE_RHI_SDLGPU)
    target_compile_definitions(mce_rhi_default PRIVATE MCE_HAS_RHI_SDLGPU=1)
endif()
if(MCE_MOBILE)
    target_compile_definitions(mce_rhi_default PRIVATE MCE_RHI_SDLGPU_ONLY=1)  # Android/iOS: no GL, no Metal backend
endif()
