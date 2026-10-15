# mce_relay (M-NET2 step 6): the relay/lobby server. Rooms with codes, UDP (ENet) for native players
# and WebSocket for browsers, on one process. -selftest 1 runs a native and a WebSocket client
# through it over loopback and exits.
mce_add_executable(mce_relay UMBRELLA SOURCES main.cpp DEPS mce::mce_net)
if(MCE_BUILD_TESTS)
    add_test(NAME mce_relay_selftest COMMAND mce_relay -selftest 1)
endif()
