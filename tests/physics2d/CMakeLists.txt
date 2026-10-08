# test_physics2d: hand-computed collision/solver cases, controller suite, sensors, queries,
# ECS integration and determinism (physics.md §6 tests 1–5, 8, 9, 11), plus the Box2D v3 oracle
# scenes (test 14, ADR-0016) when MCE_PHYSICS_ORACLE is on. This is the only target that may link
# Box2D, and oracle/ the only directory that may name it (scripts/ci/check_walls.py).
set(_physics2d_sources
    test_collision.cpp
    test_manifold_cases.cpp
    test_world.cpp
    test_solver.cpp
    test_controller.cpp
    test_module.cpp)
if(MCE_PHYSICS_ORACLE)
    list(APPEND _physics2d_sources oracle/test_box2d_oracle.cpp)
endif()
mce_add_test(test_physics2d
    SOURCES ${_physics2d_sources}
    DEPS
        mce::mce_physics2d)
if(MCE_PHYSICS_ORACLE)
    target_link_libraries(test_physics2d PRIVATE box2d::box2d)
endif()
# White-box tests of the narrowphase/broadphase internals.
target_include_directories(test_physics2d PRIVATE ${PROJECT_SOURCE_DIR}/src/engine/physics2d/src)
