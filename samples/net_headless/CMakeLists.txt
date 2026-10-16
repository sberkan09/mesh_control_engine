# The optional networking module's running demo (ADR-0035): a server and a client in one process
# over loopback UDP, replicating a small scene and a client-driven player; with -sim 1, several clients
# over SimNetwork's simulated latency, jitter and loss (M-NET2 step 1); with -relay 1, through a relay room
# (step 6). Not a ladder rung.
if(TARGET mce_net)
    mce_add_executable(sample_net_headless UMBRELLA SOURCES main.cpp DEPS mce::mce_net)
    if(MCE_BUILD_TESTS)
        add_test(NAME sample_net_headless_300 COMMAND sample_net_headless -ticks 300)
        add_test(NAME sample_net_headless_sim COMMAND sample_net_headless -sim 1 -clients 4 -latency 75 -jitter 20
                 -loss 0.05 -ticks 600)
        add_test(NAME sample_net_headless_relay COMMAND sample_net_headless -sim 1 -relay 1 -clients 3 -latency 40
                 -jitter 10 -loss 0.05 -ticks 600)
        # M-NET3 step 1: the default sends per-client delta snapshots; this keeps the one-broadcast path.
        add_test(NAME sample_net_headless_broadcast COMMAND sample_net_headless -sim 1 -delta 0 -clients 4
                 -latency 75 -jitter 20 -loss 0.05 -ticks 600)
        # M-NET3 step 2: spectators get per-client snapshots of everything and must mirror it.
        add_test(NAME sample_net_headless_spectators COMMAND sample_net_headless -sim 1 -clients 3 -spectators 2
                 -latency 75 -jitter 20 -loss 0.05 -ticks 600)
    endif()
endif()
