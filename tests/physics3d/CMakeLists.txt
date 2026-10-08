# test_physics3d: the World3D facade over Jolt (ADR-0003; physics.md §6 tests 6, 7, 10, 13): body
# lifecycle, stepping, queries, the camera sweep, the CharacterVirtual controller suite, same-binary
# determinism, and the compile-time 2D/3D mirror-signature test. No test names Jolt (JPH wall).
mce_add_test(test_physics3d
    SOURCES
        test_world3d.cpp
        test_queries.cpp
        test_character.cpp
        test_mirror.cpp
    DEPS
        mce::mce_physics3d
        mce::mce_physics2d)
