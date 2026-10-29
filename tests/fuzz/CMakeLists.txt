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

    # The WebSocket transport over loopback TCP, a raw socket on the other end.
    mce_add_test(test_fuzz_websocket UMBRELLA
        SOURCES
            test_websocket.cpp
        DEPS
            mce::mce_net)
    target_include_directories(test_fuzz_websocket PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
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

# The importers over mutated source files of the samples (cook-side code; never linked into a player).
mce_add_test(test_fuzz_importers UMBRELLA
    SOURCES
        test_importers.cpp
        test_roads.cpp
        test_items.cpp
        test_mesh_import.cpp
        test_media_import.cpp
        test_defaults.cpp
        test_binary_import.cpp
    DEPS
        mce::mce_asset_cook
        mce_meshopt)  # test_mesh_import.cpp encodes its meshopt fixtures (the "meshopt wall" allows that file)
target_include_directories(test_fuzz_importers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(test_fuzz_importers PRIVATE MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The command line and console, and (with the editor) .mcperf captures read back.
mce_add_test(test_fuzz_misc UMBRELLA
    SOURCES
        test_misc.cpp
    DEPS
        mce::mce_core)
target_include_directories(test_fuzz_misc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(MCE_BUILD_EDITOR)
    target_link_libraries(test_fuzz_misc PRIVATE mce::mce_editor_core)
    target_compile_definitions(test_fuzz_misc PRIVATE MCE_FUZZ_HAS_EDITOR_CORE=1)
endif()

# The editor's own small files: recent list, camera bookmarks, .mceproj, locale tables in the strings grid (M-HARDEN2).
if(MCE_BUILD_EDITOR)
    mce_add_test(test_fuzz_editor_files UMBRELLA
        SOURCES
            test_editor_files.cpp
        DEPS
            mce::mce_editor_core)
    target_include_directories(test_fuzz_editor_files PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(test_fuzz_editor_files PROPERTIES ENVIRONMENT "MCE_FUZZ_ROOT=${CMAKE_BINARY_DIR}")

    # The tilemap and terrain document readers (M-HARDEN2 2).
    mce_add_test(test_fuzz_editor_documents UMBRELLA
        SOURCES
            test_editor_documents.cpp
        DEPS
            mce::mce_editor_core)
    target_include_directories(test_fuzz_editor_documents PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # The behaviour-tree editor over hostile files and edit runs (M-HARDEN2 3).
    mce_add_test(test_fuzz_editor_trees UMBRELLA
        SOURCES
            test_editor_trees.cpp
        DEPS
            mce::mce_editor_core)
    target_include_directories(test_fuzz_editor_trees PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(test_fuzz_editor_trees PROPERTIES ENVIRONMENT "MCE_FUZZ_ROOT=${CMAKE_BINARY_DIR}")
endif()

# Scene instantiate over mutated scene chunks (the kit's components are linked so real columns resolve).
mce_add_test(test_fuzz_scene UMBRELLA
    SOURCES
        test_scene.cpp
    DEPS
        mce::mce_scene
        mce::mce_gameplay_kit)
target_include_directories(test_fuzz_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(test_fuzz_scene PROPERTIES ENVIRONMENT "MCE_FUZZ_ROOT=${CMAKE_BINARY_DIR}")

# The ASan stage's gate: a sanitizer report inside a fuzz test is a failure (UBSan reports are only printed by default),
# and a run has a time budget. Iteration counts are fixed by each test's default (MCE_FUZZ_ITERS overrides them for a long
# run by hand); the timeout stops a hang a mutant caused and keeps the whole group inside the stage's budget. Labelled
# `fuzz` so `ctest -L fuzz` runs just these.
set(MCE_FUZZ_TESTS test_fuzz test_fuzz_player_data test_fuzz_net test_fuzz_script test_fuzz_importers
    test_fuzz_websocket test_fuzz_misc test_fuzz_scene test_fuzz_editor_files
    test_fuzz_editor_documents test_fuzz_editor_trees)
foreach(_t IN LISTS MCE_FUZZ_TESTS)
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "fuzz" TIMEOUT 600)
        if(MCE_ASAN)
            set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                "UBSAN_OPTIONS=set:halt_on_error=1:print_stacktrace=1:suppressions=${PROJECT_SOURCE_DIR}/cmake/ubsan.supp")
        endif()
    endif()
endforeach()
