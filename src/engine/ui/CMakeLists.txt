# mce_ui (L4): the runtime UI (docs/architecture/ui.md). Retained widget documents drawn through
# mce_render's SpriteBatch; no ImGui (FR-UI-048), no GL. Gameplay never links or includes it.

mce_add_library(mce_ui
    SOURCES
        src/ui.cpp
    PUBLIC_DEPS
        mce::mce_render
        mce::mce_input
        mce::mce_core)
