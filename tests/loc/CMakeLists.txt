# mce_loc (M-MOBILE step 11a): the string-table chunk, the language fallback chain, plural rules,
# placeholders, Turkish-aware case mapping and the saved language choice.
mce_add_test(test_loc
    SOURCES
        test_loc.cpp
    DEPS
        mce::mce_loc
        mce::mce_ecs_scene)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
target_compile_definitions(test_loc PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")

# Localized app names for the phone builds (cmake/mobile/app_names.py, used by scripts/ship.py).
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME app_names COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_app_names.py")
