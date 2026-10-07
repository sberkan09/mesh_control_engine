mce_add_test(test_ecs
    SOURCES
        test_world.cpp
        test_commands.cpp
        test_events.cpp
        test_determinism.cpp
    DEPS
        mce::mce_ecs)
