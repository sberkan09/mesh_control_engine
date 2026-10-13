# mce_monetize (M-MONETIZE step 1): the catalog chunk, the purchase and ad services over a scripted
# provider, entitlements across a relaunch, and record/replay of store and ad events.
mce_add_test(test_monetize
    SOURCES
        test_fake.cpp
        test_monetize.cpp
    DEPS
        mce::mce_monetize
        mce::mce_ecs_scene)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")
target_compile_definitions(test_monetize PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
