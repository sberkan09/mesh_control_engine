# mce_rhi_gl (L2): the OpenGL 4.5 core backend of mce_rhi — the only GL code in the engine.
# glad2 and SDL3 are PRIVATE: GL symbols never leak past this target (the GL wall).

mce_add_library(mce_rhi_gl
    SOURCES
        src/gl_device.cpp
        src/gl_translate.cpp
    PUBLIC_DEPS
        mce::mce_rhi
        mce::mce_platform
    PRIVATE_DEPS
        mce_glad
        SDL3::SDL3-static)
