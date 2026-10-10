# The optional networking module's running demo (ADR-0034): a server and a client in one process
# over loopback UDP, replicating a small scene and a client-driven player. Not a ladder rung.
if(TARGET mce_net)
    mce_add_executable(sample_net_headless UMBRELLA SOURCES main.cpp DEPS mce::mce_net)
    if(MCE_BUILD_TESTS)
        add_test(NAME sample_net_headless_300 COMMAND sample_net_headless -ticks 300)
    endif()
endif()
