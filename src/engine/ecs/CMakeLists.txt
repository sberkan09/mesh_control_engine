# mce_ecs (L1): archetype tables, generational entities, queries, phases, Commands, events,
# change ticks and the ChildOf transform hierarchy (v0.5). Component identity is
# mce::kTypeHash<T>; mce_refl records carry the same hash (refl::TypeRecord::type_hash), so
# reflected components can also be reached type-erased.
mce_add_library(mce_ecs
    SOURCES
        src/world.cpp
        src/hierarchy.cpp
        src/origin.cpp
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
