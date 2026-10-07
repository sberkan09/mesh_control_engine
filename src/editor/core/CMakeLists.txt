# mce_editor_core (L5): headless editor logic (editor-tools.md §2.1). No ImGui, no window:
# CI builds and tests it on every leg. It sees the RHI interface only, never a backend.

mce_add_library(mce_editor_core
    SOURCES
        src/console_model.cpp
        src/stub_world.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_rhi)
