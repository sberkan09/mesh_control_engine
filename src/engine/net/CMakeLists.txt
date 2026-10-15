# mce_net (L3, optional — ADR-0035): client-server networking over UDP (ENet), a little-endian byte
# codec, snapshot replication of LocalTransform for entities tagged with NetId, and SimNetwork, an
# in-memory network with simulated latency, jitter and loss for deterministic tests (M-NET2 step 1).
#
# Opt-in: not in MCE_ENGINE_LIBS, so libmce_engine never contains it. A game links it explicitly
# (mce_add_project(... DEPS mce::mce_net), or mce::mce_net against the SDK). UMBRELLA: where game
# modules are supported its engine dependencies resolve against libmce_engine, so the copy linked
# into a game module shares the host's engine instead of making a second one. ENet's objects are
# folded into this archive, so mce_net is the one library a game adds.
mce_add_library(mce_net UMBRELLA
    SOURCES
        src/host.cpp
        src/net_stats.cpp
        src/replication.cpp
        src/sim_network.cpp
        $<TARGET_OBJECTS:mce_enet>
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs)
target_include_directories(mce_net SYSTEM PRIVATE $<TARGET_PROPERTY:mce_enet,INTERFACE_INCLUDE_DIRECTORIES>)
if(WIN32)
    target_link_libraries(mce_net PUBLIC ws2_32 winmm)
endif()
