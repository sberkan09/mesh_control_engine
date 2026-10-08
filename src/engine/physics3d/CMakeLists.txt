# mce_physics3d (L2): World3D, a facade over embedded Jolt Physics 5 (ADR-0003; ROADMAP M5b).
# The only target that links Jolt, privately: Jolt's headers, defines and -msse4.2 stay inside.
mce_add_library(mce_physics3d
    SOURCES
        src/world3d.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs
    PRIVATE_DEPS
        Jolt)
