# M-FUZZ: deterministic mutation tests over the parsers of untrusted bytes (docs/milestones/M-FUZZ.md).
# They read the packs the samples cook, so they run after a full build; MCE_FUZZ_ROOT is the build tree.
mce_add_test(test_fuzz
    SOURCES
        test_chunk_readers.cpp
    DEPS
        mce::mce_asset
        mce::mce_ui
        mce::mce_input
        mce::mce_loc
        mce::mce_monetize
        mce::mce_liveops
        mce::mce_physics3d)
target_include_directories(test_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_fuzz PROPERTIES ENVIRONMENT "MCE_FUZZ_ROOT=${CMAKE_BINARY_DIR}")
