# mce_physics3d (L2): World3D, a facade over embedded Jolt Physics 5 (ADR-0003; ROADMAP M5b), and
# NavMesh3D, navigation over embedded Recast & Detour (ADR-0039; M-GAMEPLAY3D).
# The only target that links Jolt (privately) and compiles Recast & Detour (folded into this
# archive): their headers, defines and flags stay inside.
mce_add_library(mce_physics3d
    SOURCES
        src/navmesh3d.cpp
        src/world3d.cpp
        $<TARGET_OBJECTS:mce_recast>
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs
    PRIVATE_DEPS
        Jolt)
target_include_directories(mce_physics3d SYSTEM PRIVATE $<TARGET_PROPERTY:mce_recast,INTERFACE_INCLUDE_DIRECTORIES>)
