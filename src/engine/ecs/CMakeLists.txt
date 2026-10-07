# mce_ecs (L1): archetype tables, generational entities, queries, phases, Commands, events.
# mce_refl joins at M2 (MCE_COMPONENT); until then component identity is mce::kTypeHash<T>.
mce_add_library(mce_ecs
    SOURCES
        src/world.cpp
    PUBLIC_DEPS
        mce::mce_core)
