# mce_gameplay_kit (L3): the starter-kit components a scene authored in the editor is made of
# (FR-ENT-080: Sprite, TextLabel, Camera2DComponent) and their render extract — the "extract
# lives with the gameplay kit" half of mce_render's contract (renderer.h). Reflected with
# MCE_COMPONENT, so the cook, the scene loader and the editor inspector see them with zero
# per-type code.
mce_add_library(mce_gameplay_kit
    SOURCES
        src/anim3d.cpp
        src/bodies3d.cpp
        src/captions.cpp
        src/coop.cpp
        src/presence.cpp
        src/proclevel.cpp
        src/effects.cpp
        src/kit3d.cpp
        src/kit3d_extract.cpp
        src/nav3d.cpp
        src/joints3d.cpp
        src/ragdolls3d.cpp
        src/vehicles3d.cpp
        src/seats3d.cpp
        src/combat3d.cpp
        src/sensors3d.cpp
        src/particles.cpp
        src/pathfinding.cpp
        src/procgen.cpp
        src/sequence.cpp
        src/skeleton2d.cpp
        src/skeleton2d_anim.cpp
        src/skeleton2d_ik.cpp
        src/sprite_anim.cpp
        src/sprite_extract.cpp
        src/static_merge3d.cpp
        src/stock.cpp
        src/timeline.cpp
        src/ai.cpp
        src/ai_tasks.cpp
        src/perception.cpp
        src/cinematic.cpp
        src/video.cpp
        src/render_textures.cpp
        src/views.cpp
    PUBLIC_DEPS
        mce::mce_ecs
        mce::mce_refl
        mce::mce_asset
        mce::mce_render
        mce::mce_ecs_scene
        mce::mce_scene  # kit::UiFlow loads a scene (scene::request_change)
        mce::mce_audio
        mce::mce_input  # CameraFollow3D orbits by the Look input
        mce::mce_physics2d
        mce::mce_physics3d
        mce::mce_monetize  # mce/gameplay.h reaches the purchase and ad services through the kit
        mce::mce_liveops  # ... the analytics events and the consent state
        mce::mce_loc)  # ... and the game's text in the player's language
