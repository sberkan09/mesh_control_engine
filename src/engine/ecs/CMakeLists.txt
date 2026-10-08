# mce_ecs (L1): archetype tables, generational entities, queries, phases, Commands, events.
# Component identity is mce::kTypeHash<T>; mce_refl records carry the same hash
# (refl::TypeRecord::type_hash), so reflected components can also be reached type-erased.
mce_add_library(mce_ecs
    SOURCES
        src/world.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_refl)
