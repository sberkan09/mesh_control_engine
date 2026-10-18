# hlod_city (M-OPENWORLD A4): a procedural city extracted without a GPU with every area loaded and with HLOD proxies
# for the far areas; the run checks that HLOD submits under half the draws over a quarter of the entities (counters, not GPU time).
mce_add_executable(sample_hlod_city SOURCES main.cpp DEPS mce::mce_gameplay_kit mce::mce_render mce::mce_asset mce::mce_ecs mce::mce_scene)
if(MCE_BUILD_TESTS)
    add_test(NAME sample_hlod_city COMMAND sample_hlod_city -blocks 8 -near 150)
    set_tests_properties(sample_hlod_city PROPERTIES PASS_REGULAR_EXPRESSION "hlod_city: ok")
endif()
