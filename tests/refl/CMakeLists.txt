mce_add_test(test_refl
    SOURCES
        test_property_path.cpp
        test_registry.cpp
        test_second_tu.cpp
    DEPS
        mce::mce_refl)
target_include_directories(test_refl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
