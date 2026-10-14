# mce_liveops (M-LIVEOPS step 1): the consent gate, analytics events and their file backend, crash
# reports sent on the next launch.
mce_add_test(test_liveops
    SOURCES
        test_liveops.cpp
    DEPS
        mce::mce_liveops)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
target_compile_definitions(test_liveops PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
