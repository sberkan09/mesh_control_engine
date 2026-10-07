# test_physics2d: hand-computed collision/solver cases, controller suite, sensors, queries,
# ECS integration and determinism (physics.md §6 tests 1–5, 8, 9, 11). The Box2D v3 oracle
# (test 14, ADR-0016) joins at M3.5 and would live under tests/test_physics2d/ per the grep wall.
mce_add_test(test_physics2d
    SOURCES
        test_collision.cpp
        test_world.cpp
        test_controller.cpp
        test_module.cpp
    DEPS
        mce::mce_physics2d)
# White-box tests of the narrowphase/broadphase internals.
target_include_directories(test_physics2d PRIVATE ${PROJECT_SOURCE_DIR}/src/engine/physics2d/src)
