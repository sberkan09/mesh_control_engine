# mce_scene (L2): the runtime scene load path (assets.md §6, FR-ENT-060): a cooked CHNK_SCENE
# becomes entities in a World. Reflection-driven: component columns are matched to the process's
# mce_refl registry by type name and written type-erased, so a new reflected component needs no
# loader changes (FR-DX-020).
mce_add_library(mce_scene
    SOURCES
        src/scene.cpp
    PUBLIC_DEPS
        mce::mce_ecs
        mce::mce_asset
        mce::mce_refl)
