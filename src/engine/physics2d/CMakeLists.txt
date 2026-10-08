# mce_physics2d (L2): hand-written 2D physics (physics.md; ROADMAP M3 rung-6 shape).
# Box2D is never linked here (D9); it may only appear in the physics test as an oracle (M3.5).
mce_add_library(mce_physics2d
    SOURCES
        src/collision.cpp
        src/dynamic_tree.cpp
        src/world2d.cpp
        src/module.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_refl
        mce::mce_ecs)
