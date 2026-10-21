# M-MONETIZE's running demo (SBA-21): a headless runner with a shop against the fake store and ad
# network. Not a ladder rung; desktop only (the phone build is the stock player with a .mcstore).
mce_add_executable(sample_shop_demo SOURCES main.cpp DEPS mce::mce_monetize mce::mce_asset mce::mce_ecs_scene)
mce_sample_pack(sample_shop_demo)  # assets/store.mcstore: the store catalog

if(MCE_BUILD_TESTS)
    add_test(NAME sample_shop_demo_300 COMMAND sample_shop_demo -ticks 300)
    # Purchase, cancel, restore, relaunch and record/replay end to end (shop_demo_test.cmake).
    add_test(NAME sample_shop_demo_flows
             COMMAND ${CMAKE_COMMAND} -DSHOP=$<TARGET_FILE:sample_shop_demo>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/flows -P ${CMAKE_CURRENT_SOURCE_DIR}/shop_demo_test.cmake)
endif()
