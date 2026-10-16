# mce_net (ADR-0035): the byte codec, a loopback client-server session over real UDP sockets, and
# snapshot replication between two worlds; WebSocket over loopback TCP and rooms through a relay.
mce_add_test(test_net UMBRELLA
    SOURCES
        test_bytes.cpp
        test_delta.cpp
        test_host.cpp
        test_interpolation.cpp
        test_lag_compensation.cpp
        test_prediction.cpp
        test_relay.cpp
        test_replicated_fields.cpp
        test_replication.cpp
        test_rollback.cpp
        test_rtc.cpp
        test_sim_network.cpp
        test_websocket.cpp
    DEPS
        mce::mce_net)
