mce_add_test(test_input
    SOURCES
        test_actions.cpp
        test_quantization.cpp
        test_replay.cpp
        test_gamepad_rebind.cpp
    DEPS
        mce::mce_input)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
target_compile_definitions(test_input PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
