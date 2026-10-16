# M-NET3 — Multiplayer at scale (milestone record)

**Status:** active (added 2026-10-16; step 1 in progress). Plan: ROADMAP §M-NET3; Linear SBA-123 … SBA-127.

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
| 1 | Delta snapshots and interest management: per-client snapshots, `NetRelevance` (radius around the client's owned entity; always relevant), delta coding against the newest acknowledged snapshot, per-client bandwidth counters | SBA-123 | in progress |
| 2 | Spectators in replication and rollback sessions; bounded clock sync for rollback peers that run ahead | SBA-124 | not started |
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
