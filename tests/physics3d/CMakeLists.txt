# test_physics3d: the World3D facade over Jolt (ADR-0003; physics.md §6 tests 6, 7, 10, 13): body
# lifecycle, stepping, queries, the camera sweep, the CharacterVirtual controller suite, same-binary
# determinism, joints (M-GAMEPLAY3D step 2), ragdolls (step 3), vehicles (step 4), and the compile-time 2D/3D mirror-signature test; NavMesh3D's bake, queries, obstacles
# and crowd (ADR-0040). No test names Jolt, Recast or Detour (the JPH and Recast walls).
mce_add_test(test_physics3d
    SOURCES
        test_world3d.cpp
        test_queries.cpp
        test_budget.cpp
        test_character.cpp
        test_mirror.cpp
        test_navmesh.cpp
        test_joints.cpp
        test_ragdolls.cpp
        test_vehicles.cpp
        test_contacts.cpp
    DEPS
        mce::mce_physics3d
        mce::mce_physics2d)

target_compile_definitions(test_physics3d PRIVATE MCE_NAV_SAMPLE="${CMAKE_SOURCE_DIR}/samples/07_gameplay3d/assets/navmesh/arena/nav_13.mcnav")
