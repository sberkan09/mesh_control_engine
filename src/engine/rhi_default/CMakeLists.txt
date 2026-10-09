# mce_rhi_default (L2): rhi::create_default_device, the one place that picks the native backend —
# mce_rhi_metal on macOS, mce_rhi_gl everywhere else (MCE_RHI_BACKEND, set by the root
# CMakeLists.txt). Hosts link this instead of a backend.

mce_add_library(mce_rhi_default
    SOURCES
        src/default_device.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        mce::${MCE_RHI_BACKEND})
