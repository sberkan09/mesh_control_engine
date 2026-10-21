# mce_net (L3, optional — ADR-0035): client-server networking over UDP (ENet), a little-endian byte
# codec, snapshot replication of LocalTransform for entities tagged with NetId, and SimNetwork, an
# in-memory network with simulated latency, jitter and loss for deterministic tests (M-NET2 step 1), and
# rooms through a relay with a WebSocket transport for browsers (step 6).
#
# Opt-in: not in MCE_ENGINE_LIBS, so libmce_engine never contains it. A game links it explicitly
# (mce_add_project(... DEPS mce::mce_net), or mce::mce_net against the SDK). UMBRELLA: where game
# modules are supported its engine dependencies resolve against libmce_engine, so the copy linked
# into a game module shares the host's engine instead of making a second one. ENet's objects are
# folded into this archive, so mce_net is the one library a game adds.
# WebSocket: native TCP sockets, or the browser's WebSocket in a web build.
if(MCE_WEB)
    set(mce_net_websocket_sources src/websocket_web.cpp)
else()
    set(mce_net_websocket_sources src/socket.cpp src/websocket.cpp)
endif()
# WebRTC (M-NET3 step 4): libdatachannel natively (its objects folded in like ENet's), the browser's
# RTCPeerConnection on the web, or none (every pair stays on the relay).
if(MCE_WEB)
    set(mce_net_rtc_sources src/rtc_web.cpp)
elseif(MCE_WITH_RTC)
    set(mce_net_rtc_sources src/rtc_native.cpp
        $<TARGET_OBJECTS:datachannel-static> $<TARGET_OBJECTS:juice-static> $<TARGET_OBJECTS:usrsctp>)
else()
    set(mce_net_rtc_sources src/rtc_none.cpp)
endif()
mce_add_library(mce_net UMBRELLA
    SOURCES
        src/host.cpp
        src/lag_compensation.cpp
        src/net_stats.cpp
        src/prediction.cpp
        src/relay.cpp
        src/replication.cpp
        src/rollback.cpp
        src/rtc.cpp
        src/sim_network.cpp
        src/ws_crypto.cpp
        ${mce_net_websocket_sources}
        ${mce_net_rtc_sources}
        $<TARGET_OBJECTS:mce_enet>
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs)
target_include_directories(mce_net SYSTEM PRIVATE $<TARGET_PROPERTY:mce_enet,INTERFACE_INCLUDE_DIRECTORIES>)
if(WIN32)
    target_link_libraries(mce_net PUBLIC ws2_32 winmm)
endif()
if(MCE_WITH_RTC AND NOT MCE_WEB)
    target_include_directories(mce_net SYSTEM PRIVATE $<TARGET_PROPERTY:datachannel-static,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(mce_net PRIVATE RTC_STATIC)
    find_package(Threads REQUIRED)
    target_link_libraries(mce_net PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()
if(MCE_WEB)
    target_link_options(mce_net INTERFACE -lwebsocket.js)  # websocket_web.cpp: the browser's WebSocket
endif()

# The relay server (M-NET2 step 6): rooms with codes over UDP and WebSocket, for players who cannot
# accept connections (home routers, browsers). A tool, but it lives here: only mce_net links mce_net.
if(NOT MCE_MOBILE AND NOT MCE_WEB)
    add_subdirectory(relay)
endif()
if(MCE_WEB)
    add_subdirectory(web_probe)  # the browser side of the relay selftest (M-WEB)
endif()
