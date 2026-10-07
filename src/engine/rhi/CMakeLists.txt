# mce_rhi (L2): the backend-neutral render hardware interface (rhi-renderer.md §2.1-2.3).
# No GL, no SDL: mce_render and the editor link this; only mce_rhi_gl implements it.

mce_add_library(mce_rhi
    SOURCES
        src/command_buffer.cpp
        src/types.cpp
    PUBLIC_DEPS
        mce::mce_core)
