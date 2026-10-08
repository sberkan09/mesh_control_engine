# mce_gameplay_kit (L3): the starter-kit components a scene authored in the editor is made of
# (FR-ENT-080: Sprite, TextLabel, Camera2DComponent) and their render extract — the "extract
# lives with the gameplay kit" half of mce_render's contract (renderer.h). Reflected with
# MCE_COMPONENT, so the cook, the scene loader and the editor inspector see them with zero
# per-type code.
mce_add_library(mce_gameplay_kit
    SOURCES
        src/sprite_extract.cpp
    PUBLIC_DEPS
        mce::mce_ecs
        mce::mce_refl
        mce::mce_asset
        mce::mce_render)
