# mce_net (ADR-0035): the byte codec, a loopback client-server session over real UDP sockets, and
# snapshot replication between two worlds.
mce_add_test(test_net UMBRELLA
    SOURCES
        test_bytes.cpp
        test_host.cpp
        test_interpolation.cpp
        test_replication.cpp
        test_sim_network.cpp
    DEPS
        mce::mce_net)
