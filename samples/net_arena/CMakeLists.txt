# M-NET3's running demo (step 5): a 2-4 player top-down arena. The arena is scene data (NetReplicated
# walls and spawn pads); the server makes the players; clients predict their own and interpolate the
# others; shots are lag compensated; players find each other through mce_relay's quick play.
#   sample_net_arena -headless 1 ...   a whole match over SimNetwork (CI, deterministic)
#   sample_net_arena -serve 1 ...      a dedicated server on UDP
#   sample_net_arena -connect host:port  one windowed player (one process per player)
#   sample_net_arena                   windowed, local: you and three bots, split across the screen
if(TARGET mce_net)
    mce_add_executable(sample_net_arena UMBRELLA SOURCES main.cpp
                       DEPS mce::mce_net mce::mce_asset mce::mce_ecs_scene mce::mce_platform mce::mce_render mce::mce_rhi_default)
    target_include_directories(sample_net_arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
    mce_sample_pack(sample_net_arena)
    if(MCE_BUILD_TESTS)
        add_test(NAME sample_net_arena_sim COMMAND sample_net_arena -headless 1 -clients 4 -latency 150 -jitter 20
                 -loss 0.03 -ticks 900)
        set_tests_properties(sample_net_arena_sim PROPERTIES PASS_REGULAR_EXPRESSION "net_arena: OK, hash [0-9a-f]+")
        add_test(NAME sample_net_arena_two COMMAND sample_net_arena -headless 1 -clients 2 -latency 60 -jitter 10
                 -loss 0.0 -ticks 600)
        set_tests_properties(sample_net_arena_two PROPERTIES PASS_REGULAR_EXPRESSION "net_arena: OK, hash [0-9a-f]+")
        if(MCE_GPU_TESTS)
            add_test(NAME sample_net_arena_gpu COMMAND sample_net_arena -frames 240 -platform.error_box 0 -r.vsync 0)
            set_tests_properties(sample_net_arena_gpu PROPERTIES LABELS gpu)
        endif()
    endif()
endif()
