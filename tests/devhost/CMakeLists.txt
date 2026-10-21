if(MCE_GAME_MODULES_SUPPORTED)
    # Links the umbrella (never static engine libraries) so it can host game modules.
    add_executable(mce_devhost main.cpp)
    target_link_libraries(mce_devhost PRIVATE mce::mce_engine)
    target_compile_definitions(mce_devhost PRIVATE MCE_DEVHOST_MODULES=1)
    mce_apply_compile_policy(mce_devhost)
    if(TARGET modtest_good)
        # Sample-ladder rule for modules: load + 300 headless fixed steps (scripting.md §6.5).
        add_test(NAME devhost_module_300 COMMAND mce_devhost -module $<TARGET_FILE:modtest_good> -ticks 300)
        set_tests_properties(devhost_module_300 PROPERTIES PASS_REGULAR_EXPRESSION "module modtest v3, 3 entities"
            RESOURCE_LOCK modtest_module)  # shares libmodtest_good_loaded_N.so with test_module
    endif()
else()
    mce_add_executable(mce_devhost SOURCES main.cpp DEPS mce::mce_core)
    target_compile_definitions(mce_devhost PRIVATE MCE_DEVHOST_MODULES=0)
endif()
add_test(NAME devhost_headless_10k COMMAND mce_devhost -ticks 10000)
