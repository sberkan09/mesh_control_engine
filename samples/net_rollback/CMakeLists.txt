# Rollback netcode with the optional networking module (M-NET2 step 5): peers that each run the
# whole game exchange only inputs over SimNetwork, roll back wrong guesses and compare state hashes
# to catch a desync. Not a ladder rung.
if(TARGET mce_net)
    mce_add_executable(sample_net_rollback UMBRELLA SOURCES main.cpp DEPS mce::mce_net)
    if(MCE_BUILD_TESTS)
        add_test(NAME sample_net_rollback COMMAND sample_net_rollback)
        add_test(NAME sample_net_rollback_lockstep COMMAND sample_net_rollback -peers 2 -prediction 0 -frames 300)
        add_test(NAME sample_net_rollback_desync COMMAND sample_net_rollback -peers 3 -desync_at 300 -frames 600)
    endif()
endif()
