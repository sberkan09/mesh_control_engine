# Runs on SDL's "dummy" video driver: no display needed, so this is a plain unit test everywhere.
mce_add_test(test_platform SOURCES test_platform.cpp test_review.cpp DEPS mce::mce_platform)
target_compile_definitions(test_platform PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# The review prompt's Android side (cmake/mobile/android_app.py stamping MceReview, M-MOBILE step 11c).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME android_review COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_android_review.py")
