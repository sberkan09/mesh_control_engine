#pragma once
// mce_net, the optional networking module (ADR-0035). Link mce::mce_net to use it; a game that
// does not carries no socket code (mce_engine never contains it).
//
//   transport.h    Transport: what game code sends and receives through; LinkConditions
//   host.h         Host: a UDP endpoint (server or client) with reliable / sequenced / unreliable
//                  channels over ENet; set_conditions plays it over a simulated bad network
//   sim_network.h  SimNetwork / SimEndpoint: an in-memory network on a virtual clock with simulated
//                  latency, jitter and loss, deterministic per seed (tests, headless runs)
//   net_stats.h    NetStatsMeter: rates and RTT for the dev overlay (ecs/dev_stats.h)
//   bytes.h        ByteWriter / ByteReader: the little-endian message codec
//   replication.h  NetId, ReplicationServer / ReplicationClient: server-authoritative snapshots
//   websocket.h    WebSocketHost: a Transport over WebSocket, the one a web build can open
//   relay.h        RelayServer / RelayClient: rooms with codes through a relay (mce_relay)
//   rtc.h          RtcHost: a room whose members also connect directly over WebRTC data channels

#include "mce/net/bytes.h"
#include "mce/net/host.h"
#include "mce/net/lag_compensation.h"
#include "mce/net/net_stats.h"
#include "mce/net/prediction.h"
#include "mce/net/relay.h"
#include "mce/net/replication.h"
#include "mce/net/rollback.h"
#include "mce/net/rtc.h"
#include "mce/net/sim_network.h"
#include "mce/net/transport.h"
#include "mce/net/websocket.h"
