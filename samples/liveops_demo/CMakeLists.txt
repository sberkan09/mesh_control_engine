# M-LIVEOPS's running demo (SBA-25): a headless level runner wired to game services, remote config
# and analytics against local stand-ins. Not a ladder rung; desktop only (the phone build is the
# stock player with a .mcservices and a .mcconfig).
mce_add_executable(sample_liveops_demo SOURCES main.cpp DEPS mce::mce_liveops mce::mce_asset mce::mce_ecs_scene)
mce_sample_pack(sample_liveops_demo)  # assets/game.mcservices and assets/remote.mcconfig

if(MCE_BUILD_TESTS)
    add_test(NAME sample_liveops_demo_300 COMMAND sample_liveops_demo -ticks 300)
    # Achievements, scores, cloud restore, remote config, consent and record/replay end to end.
    add_test(NAME sample_liveops_demo_flows
             COMMAND ${CMAKE_COMMAND} -DDEMO=$<TARGET_FILE:sample_liveops_demo> -DMCE_COOK=$<TARGET_FILE:mce_cook>
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/flows -P ${CMAKE_CURRENT_SOURCE_DIR}/liveops_demo_test.cmake)
endif()
