# test_physics3d: the World3D facade over Jolt (ADR-0003; physics.md §6 tests 6, 7, 10, 13): body
# lifecycle, stepping, queries, the camera sweep, the CharacterVirtual controller suite, same-binary
# determinism, and the compile-time 2D/3D mirror-signature test; NavMesh3D's bake, queries, obstacles
# and crowd (ADR-0040). No test names Jolt, Recast or Detour (the JPH and Recast walls).
mce_add_test(test_physics3d
    SOURCES
        test_world3d.cpp
        test_queries.cpp
        test_character.cpp
        test_mirror.cpp
        test_navmesh.cpp
    DEPS
        mce::mce_physics3d
        mce::mce_physics2d)
