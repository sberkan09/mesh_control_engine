# mce_ecs (L1): archetype tables, generational entities, queries, phases, Commands, events,
# change ticks and the ChildOf transform hierarchy (v0.5). Reflection (mce_refl) names the
# engine components so cooked scene/prefab columns can find them.
mce_add_library(mce_ecs
    SOURCES
        src/world.cpp
        src/hierarchy.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_refl)

# mce_ecs_scene (L2): cooked scene/prefab chunks -> live rows (instantiate = bulk row copy +
# handle remap + overrides) and the save-game reflection dump. Sits above both mce_ecs and
# mce_asset so the ECS itself never includes an asset header (ecs-gameplay.md §6 layering).
mce_add_library(mce_ecs_scene
    SOURCES
        src/prefab.cpp
        src/save.cpp
    PUBLIC_DEPS
        mce::mce_ecs
        mce::mce_asset)
