# M-NET3 — Multiplayer at scale (milestone record)

**Status:** active (added 2026-10-16; steps 1–2 done). Plan: ROADMAP §M-NET3; Linear SBA-123 … SBA-127.

**Why:** M-NET2 made multiplayer that feels local, but it leaves gaps that a shipped game hits (user guide 20.10):

- Every client receives every replicated entity in full, every send tick. Bytes grow with entities times
  clients, whether or not anything moved or the client can see it.
- There are no spectators. A rollback peer whose clock runs ahead only waits.
- Rooms need a code: there is no quick play and no room list. When the host leaves, the room closes.
- Through the relay every message pays two hops; there is no direct WebRTC connection.
- The 150 ms demo ran headless; there is no windowed multiplayer sample.

It needs no device. Every step gates headless in `local_ci.sh` over `SimNetwork`, loopback sockets and
headless Chromium.

**Not in scope:** dedicated-server hosting or orchestration, accounts and persistent player identity, voice
chat, anti-cheat beyond server authority.

## Steps

| # | Step | Linear | State |
|---|---|---|---|
| 1 | Delta snapshots and interest management: per-client snapshots, `NetRelevance` (radius around the client's owned entity; always relevant), delta coding against the newest acknowledged snapshot, per-client bandwidth counters | SBA-123 | done |
| 2 | Spectators in replication and rollback sessions; bounded clock sync for rollback peers that run ahead | SBA-124 | done |
| 3 | Matchmaking and host migration in `mce_relay`: quick play by mode and size, a room list, the host role handed on and the server rebuilt from the new host's mirror | SBA-125 | not started |
| 4 | WebRTC data channels (native over a fetched library, web over `RTCPeerConnection`), signalled through `mce_relay`, the relay as fallback | SBA-126 | not started |
| 5 | `samples/net_arena`: a windowed 2–4 player arena from scene data, the user guide, MCP reach and the demo | SBA-127 | not started |

**Demo:** four arena clients at 150 ms simulated latency, joined by quick play and recorded from an xvfb run.
The host leaves and the match goes on. Bytes down per client are measured against M-NET2's full snapshots.

**Gates:**
- Headless multi-client and arena runs hash the same across runs and Debug/Release.
- Delta snapshots give the same mirrors as full ones for the relevant entities.
- Host migration keeps every mirror matching the new host.
- Two native peers connect over loopback ICE; the browser probe joins a native peer directly.
- No sim code includes networking headers; `local_ci.sh` green on every step.

## Step 1 — delta snapshots and interest management

| Item | Where | Evidence |
|---|---|---|
| `ReplicationServer::write_snapshot_for(world, tick, peer, out)`: the client's own snapshot, full until it acknowledges one, then an "MSD" delta against the newest acknowledged (per entity: id gap, change mask, changed transform floats, extra and field blobs; a new entity whole). 32 snapshots kept per client; an older acknowledgement gives a full one | `mce/net/replication.h`, `replication.cpp` | `test_net` "after the first acknowledgement snapshots are deltas, far smaller, mirroring exactly" (50 still entities: under 130 bytes; spawns and despawns through deltas), "lost snapshots and late acknowledgements still decode against what the client has" |
| Acknowledgements carry the tick and the content hash (`write_ack` / `read_ack`), so a tick written twice (state changed after the last tick) is told apart; a reset client acknowledges nothing and gets a full snapshot; `forget_client` on disconnect | same | "a client that lost its history asks for a full snapshot", "the same tick written twice is told apart by its hash" |
| The client rebuilds full entries from the delta and its kept copy and mirrors them as before; a delta without its baseline is ignored (`missing_baseline`); the rebuilt content must match the server's hash, so a damaged delta changes nothing | same | "every truncation of a delta is rejected without queuing a command" (and a flipped byte) |
| `NetRelevance { radius }` (mce_ecs): an entity reaches a client only within the radius of an entity the client owns (`NetOwner`), or when it owns it; without it, everyone | `mce/ecs/replicated.h`, `replication.cpp` | "NetRelevance sends an entity only to clients with an owned entity close by" (enters, leaves, owned always, broadcast unaffected) |
| `client_stats(peer)`: snapshots, full ones, relevant entities, bytes. `write_snapshot` and `write_snapshot_for` return the content hash, `ReplicationClient::content_hash()` gives the applied one | same | same tests |
| `sample_net_headless` sends per-client deltas by default (acks on the input channel); `-delta 0` keeps the one broadcast | `samples/net_headless/main.cpp` | ctest `sample_net_headless_sim`, `_relay` (deltas) and `_broadcast`; `local_ci.sh` determinism stage (deltas, across runs and Debug/Release) |

Snapshot bytes in `sample_net_headless` (8 drifters that move every tick, one player per client), per-client
deltas against the same snapshots broadcast in full:

| Run | Snapshots applied as deltas | Bytes sent | As full broadcasts | Share |
|---|---|---|---|---|
| 4 clients, 75 ms ± 20, 5% loss, 600 ticks | 1777 of 1811 | 449,032 | 1,427,314 | 31% |
| 8 clients, 150 ms ± 40, 15% loss, 900 ticks | 4005 of 4116 | 1,711,007 | 5,357,724 | 32% |
| through a relay, 3 clients, 40 ms ± 10, 5% loss | 1607 of 1658 | 313,744 | 960,302 | 33% |

Notes:
- With `-delta 0` the sample's hash is the same as before this step: the broadcast path did not change.
- Everything in the sample moves every tick, so the saving is the transform's unchanged floats (z,
  rotation, scale) and the unchanged extra bytes. A scene where most entities stand still saves far more:
  2 bytes per still entity against 45 or more.
- Deltas are byte-level (changed floats travel whole). Quantising positions is not done here.

## Step 2 — spectators and clock sync

| Item | Where | Evidence |
|---|---|---|
| Rollback spectators: `RollbackDesc::spectator` runs only fully known frames (up to 4 per `advance()`), no input, no prediction; a player feeds it with `write_spectator_packet` ("MRS", every player's confirmed inputs from the spectator's ack, at most 32 frames / 1 KiB, plus hashes), the spectator acknowledges with `write_packet` ("MRA"); it compares the feeder's hashes | `mce/net/rollback.h`, `rollback.cpp` | `test_net` "a spectator runs only confirmed frames and checks the player's hashes" (10% loss; no prediction, its hash equals the player's and the game run in one place), "a spectator that computes something else is caught" (at the sabotaged frame; players unaffected) |
| Clock sync (`time_sync`, default on): packets carry the sender's frame and its smoothed advantage (version 2); half the difference of the two views is the skew, latency cancelled; more than a frame ahead skips one tick, at most every 10 calls (`sync_waits`); `frame_advantage(p)` | same | "clock sync keeps a fast peer from running into the prediction limit" (5% fast: stalls halved or better, the slow peer hardly waits), "two peers on the same clock do not wait for each other" |
| Packets in the wrong place are refused: a feed at a player, a player packet or an ack at a spectator, a spectator index out of range, truncation | same | "spectator packets go only where they belong" |
| Replication spectators: `ReplicationServer::set_view(peer, &point)` (relevance also from a camera point) and `set_sees_all(peer, true)`; `forget_client` drops them | `mce/net/replication.h`, `replication.cpp` | "a spectator sees what its camera sees, or everything" |
| `sample_net_rollback`: one spectator by default (`-spectators N`), `-drift N` (peer 0 plays an extra frame every N ticks), `-sync 0/1`; spectators must end on the players' hash | `samples/net_rollback/main.cpp` | ctest `sample_net_rollback`, `_drift` (two spectators); `local_ci.sh` determinism stage |
| `sample_net_headless -spectators N` (sim, direct): clients that send WATCH get every entity as per-client deltas and must mirror the server bit for bit | `samples/net_headless/main.cpp` | ctest `sample_net_headless_spectators` (3 players, 2 spectators, 75 ms ± 20, 5% loss) |

`sample_net_rollback -drift 30` (4 peers, 100 ms ± 20, 5% loss, peer 0 plays 1 extra frame every 30 ticks):

| | Peer 0 stalls | Peer 0 sync waits | Frames replayed, all peers |
|---|---|---|---|
| `-sync 0` | 40 | 0 | 5,678 |
| clock sync | 10 | 33 | 4,946 |

Notes:
- A spectator joins from frame 0 and stays within the 256-frame input ring; joining a match in progress needs
  a state snapshot first, which is not done here.
- With the same clock on every peer the sample's 4 peers still skip a few ticks (1–5 each) because jitter and
  loss move the estimate; two peers on a clean link skip none (the test).

