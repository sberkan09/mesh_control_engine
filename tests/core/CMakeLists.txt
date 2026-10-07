mce_add_test(test_core
    SOURCES
        test_app.cpp
        test_containers.cpp
        test_convar.cpp
        test_hash.cpp
        test_math.cpp
        test_memory.cpp
        test_result.cpp
        test_state_stack.cpp
    DEPS
        mce::mce_core)
target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
