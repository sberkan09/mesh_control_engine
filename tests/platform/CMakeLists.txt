# Runs on SDL's "dummy" video driver: no display needed, so this is a plain unit test everywhere.
mce_add_test(test_platform SOURCES test_platform.cpp test_review.cpp test_haptics.cpp test_power.cpp test_screen_reader.cpp DEPS mce::mce_platform)
target_compile_definitions(test_platform PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# The Android sides of the review prompt and haptics (cmake/mobile/android_app.py stamping MceReview and
# MceHaptics, M-MOBILE steps 11c-11d).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME android_bridges COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_android_bridges.py")
