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

# Saves, replays, input recordings and scripts: the player's own files.
mce_add_test(test_fuzz_player_data
    SOURCES
        test_player_data.cpp
    DEPS
        mce::mce_ecs_scene
        mce::mce_input)
target_include_directories(test_fuzz_player_data PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# What a peer can send: replication snapshots and acknowledgements, rollback packets, relay messages.
if(MCE_WITH_NET)
    mce_add_test(test_fuzz_net UMBRELLA
        SOURCES
            test_net.cpp
        DEPS
            mce::mce_net)
    target_include_directories(test_fuzz_net PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Luau source through the checks and the compiler, and bytecode through the host.
if(MCE_WITH_SCRIPT)
    mce_add_test(test_fuzz_script UMBRELLA
        SOURCES
            test_script.cpp
        DEPS
            mce::mce_script
            mce::mce_script_compiler)
    target_include_directories(test_fuzz_script PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
