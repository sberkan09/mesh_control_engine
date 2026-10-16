# net — optional client-server networking: UDP hosts, the message codec, snapshot replication

`mce_net` is an **optional** L2 library in `src/engine/net/` (headers under `include/mce/net/`). Its
public dependencies are `mce_core` and `mce_ecs`; its transport is ENet 1.3.18, compiled into the
archive (`cmake/Dependencies.cmake`). It is **not** part of the `mce_engine` umbrella: a game that
wants multiplayer links it, every other game and every engine tool stays free of socket code
(ADR-0035).

It is linked by `test_net`, `sample_net_headless`, and any project that asks for it:

```cmake
mce_add_project(my_game
    ...
    INCLUDE_DIRS games/my_game/module
    DEPS mce::mce_net)        # links mce_net into the module, the project editor and MCP server
```

Against the installed SDK the same line works: the SDK ships `lib/libmce_net.a` and `find_package(MCE)`
defines `mce::mce_net`. Configure with `-DMCE_WITH_NET=OFF` to leave the module out of an engine build.

## Public surface

`mce/net/net.h` includes the three headers below.

`host.h` — `Host`, one UDP endpoint:

- `listen(HostDesc)` makes a server: bind address (`"127.0.0.1"`, or empty for every interface), port
  (`0` picks a free one, `bound_port()` reports it), `max_peers`, `channels`, bandwidth caps,
  `timeout_ms`.
- `open_client()` + `connect(host, port, user_data)` makes a client; the returned `PeerId` names the
  server.
- `poll(Event&, timeout_ms)` sends queued traffic and returns the next event: `Connected`,
  `Disconnected` (both carry the other side's `user_data`) or `Received` (`channel` + `data`, valid until
  the next `poll`).
- `send(peer, channel, bytes, Delivery)`, `broadcast(...)`, `flush()`. `Delivery::Reliable` (resent,
  ordered per channel), `Delivery::Sequenced` (unreliable, older-than-delivered dropped: snapshots,
  inputs), `Delivery::Unreliable` (unordered).
- `disconnect` (graceful, reported to both sides), `disconnect_now`, `close()`; `is_connected`,
  `connected_count`, `connected_peers`, `stats(peer)` (RTT, loss), `bytes_sent/received`.

`bytes.h` — `ByteWriter` / `ByteReader`: `u8/u16/u32/u64/i32/f32/bool`, LEB128 `varu32`, length-prefixed
`bytes`/`string`, raw `append`. Little-endian on the wire regardless of the machine. The reader fails
closed: a read past the end, an over-long varint or a length past the end sets `ok()` to false and every
later read returns zero.

`replication.h` — server-authoritative snapshots:

- `NetId { id, kind }`: the component that marks an entity as replicated. `id` is assigned by the
  server, `kind` is the game's own archetype number.
- `ReplicationServer::assign(cmd_or_world, entity, kind)` tags an entity; `write_snapshot(world, tick,
  writer)` writes every `NetId` + `LocalTransform` entity in id order.
- `ReplicationClient::apply(cmd, world, bytes)` mirrors one snapshot through `Commands`: spawns entities
  it has not seen (with `NetId` + `LocalTransform`), updates the rest, destroys those the snapshot no
  longer lists. `find(net_id)` gives the local entity; `reset(cmd, world)` drops the mirror (call it on
  disconnect).
- `ReplicationHooks`: `write_extra` / `read_extra` carry per-entity game state (health, animation)
  after the transform, length-prefixed; `on_spawn` adds the client-only parts (sprite, mesh) from
  `NetId::kind`; `on_despawn` runs before a mirrored entity is destroyed.
- Replication as data (M-NET2 step 7): `NetReplicated { kind }` and `NetOwner { peer }` live in
  `mce/ecs/replicated.h` (mce_ecs, so scenes, the editor, cook and MCP know them without mce_net).
  `assign_tagged(world)` / `assign_tagged(cmd, world)` gives every `NetReplicated` entity without a
  `NetId` one. Reflected fields with `refl::attr::Replicated` travel after `extra` in each entry:
  varu32 component count, then per component its refl type id (u64), `replicated_layout()` (u32, a
  hash of the flagged fields' names, kinds, sizes and counts) and a length-prefixed blob of the flagged
  fields in declaration order; `Entity` fields go as the target's NetId (0 when not replicated). The
  client starts from its own value (or the type's default), writes the flagged fields with
  `Commands::set_raw`, maps NetIds back to its entities after every mirror exists, and registers a
  component type it has not seen. A type unknown on the client, or with another layout, is skipped
  (`ApplyResult::skipped_components`). Snapshot version 2.
- Per-client snapshots (M-NET3 step 1): `write_snapshot_for(world, tick, peer, out)` lists the
  entities relevant to `peer` (`NetRelevance { radius }`: within the radius of an entity the peer owns
  by `NetOwner`, or owned by it; no component or radius 0: always) and, once the client has
  acknowledged one (`ReplicationClient::write_ack` → `ReplicationServer::read_ack`: tick + content
  hash, so a tick written twice is told apart), codes them against it ("MSD" delta: per entity an id
  gap and a change mask, then the changed transform floats, extra and field blobs, or the whole entry
  for a new one). Both sides keep the last `kBaselines` (32) snapshots; an older acknowledgement, a
  reset client (it acknowledges tick 0) or `forget_client` gives a full snapshot. The client rebuilds
  the full entries and mirrors them as before; a delta whose baseline it lacks is ignored
  (`missing_baseline`), and the rebuilt content must match the hash the server wrote.
  `client_stats(peer)` counts snapshots, full ones, entities and bytes. `write_snapshot` (the
  broadcast) returns the same content hash, and `ReplicationClient::content_hash()` gives the applied one.
- Spectators (M-NET3 step 2): `set_view(peer, &point)` adds a point relevance is measured from for
  that peer (a camera; null removes it), `set_sees_all(peer, true)` makes every entity relevant to it.
  Both create the per-client record; `forget_client` drops them with the history.

`websocket.h` — `WebSocketHost` (M-NET2 step 6), a `Transport` over WebSocket with Host's shape:
`listen(WebSocketDesc)` / `open_client()` + `connect(host, port, user_data, path)`. Native builds speak
RFC 6455 over non-blocking TCP (plain `ws://`); a web build's client uses the browser's WebSocket
(`websocket_web.cpp`) and cannot listen. One binary message per send, `[channel][payload]`; every
Delivery is reliable and ordered (TCP). Pings give `stats()` its RTT and drop silent peers after
`timeout_ms`; a close frame carries the `disconnect` user_data; a message over `max_message` closes
the connection.

`rollback.h` — `RollbackSession` (M-NET2 step 5): peers that each run the whole game exchange inputs,
predict missing ones, roll back on a wrong guess and compare confirmed-state hashes. "MRB" packet
(version 2): sender, input size, ack, the sender's frame and clock-sync advantage (u8 valid + f32),
first frame, count, inputs, then up to 16 (frame, hash) pairs. M-NET3 step 2 adds:

- Spectators: `RollbackDesc::spectator` (local_player is then its index 0..7) runs only frames whose
  every input is known, up to 4 per `advance()`, never predicting. A player feeds it with
  `write_spectator_packet(s, out)` ("MRS": every player's confirmed inputs from what the spectator
  acknowledged, at most 32 frames or 1 KiB, plus hashes); the spectator's `write_packet(player)` is its
  acknowledgement ("MRA": index, frames it has). It checks the feeding player's hashes, so a spectator
  that computes something else is reported. A spectator further behind than the input ring (256
  frames) can no longer be fed (`write_spectator_packet` returns false).
- Clock sync (`RollbackDesc::time_sync`, on by default): each side's advantage (its frame minus the
  sender's frame when a packet arrived, smoothed over packets) travels back; half the difference
  between ours and theirs is how far ahead we run, latency cancelled. At most once every 10
  `advance()` calls a peer more than one frame ahead of anyone skips one (`stats().sync_waits`).

`relay.h` — rooms through a relay (M-NET2 step 6):

- `RelayServer::init(RelayDesc)`, `add_transport(t)` (any number: the `mce_relay` tool serves UDP and
  WebSocket at once), `update()` each loop, `stats()`.
- `RelayClient` is a `Transport` whose peers are a room's members: `start(link, relay_peer, {channels})`,
  `create_room(max)` / `join_room(code)`, `state()` (Idle, Requesting, InRoom, Refused, Closed),
  `room_code()`, `member_id()` (the host is 0), `refusal()`, `leave()`. It polls the link itself.
- Wire: one request or notice per message on the link's channel 0, data forwarded with the sender's
  Delivery (channel 0 reliable, channel 1 otherwise). The relay never reads game data.

## A minimal game loop

```cpp
// Server: once per tick, after World::tick.
for (net::Event ev; host.poll(ev);) { /* Connected: spawn a player + assign(); Received: inputs */ }
world.tick(dt, ++tick);
writer.clear();
replication.write_snapshot(world, u32(tick), writer);
host.broadcast(kSnapshotChannel, writer.data(), net::Delivery::Sequenced);

// Client: poll between ticks, keep the newest snapshot, apply it from a FixedUpdate_Pre system.
for (net::Event ev; host.poll(ev);)
    if (ev.kind == net::EventKind::Received && ev.channel == kSnapshotChannel) { pending.clear(); pending.append(ev.data); }
// in the system:
client.apply(f.commands(), f.world(), pending.data());
```

`samples/net_headless/main.cpp` is this loop complete: a server and a client in one process over
loopback, a client-driven player, and the bit-for-bit check.

## Rules and invariants

- **Opt-in.** `mce_net` never joins `MCE_ENGINE_LIBS`; only games, samples, projects and tests link it.
  `check_walls.py` enforces both, and confines ENet headers and symbols to `src/engine/net/src/`.
- **Single-threaded.** Nothing runs in the background; traffic moves only inside `poll()` / `flush()`.
- **Determinism (FR-RUN-040).** The network is input. The server's simulation never waits on a socket,
  and received bytes enter the world at a fixed point of the tick, so the server's state hashes and
  replays behave as without the network. Clients mirror; they are not expected to hash like the server.
- **Fail closed.** A malformed, truncated, stale or trailing-garbage snapshot is rejected whole before
  any command is queued (`ApplyResult::ok == false`). Messages a game parses itself should check
  `ByteReader::ok()` the same way and clamp values from the network (the sample clamps its input).
- **Ids are the server's.** Client entity handles differ from the server's; only `NetId::id` is
  shared. A client-side destroy of a mirrored entity is repaired by the next snapshot.
- **Replicated entities should be roots** (or have their parent replicated too): the snapshot carries
  `LocalTransform` as-is.

## How it is tested

- `test_net` (doctest): codec round-trips, varint sizes, short/hostile inputs; a client and a server
  over real loopback UDP (connect with user data, reliable messages both ways on two channels, 100
  ordered reliable messages, graceful disconnect on both sides, port clash, invalid descriptions);
  replication between two worlds (spawn/update/despawn, every truncation of a snapshot rejected with no
  command queued, stale tick ignored, hooks, local-destroy repair, `reset`, apply from inside a system).
  Replicated fields: flagged fields arrive and unflagged ones keep the client's value, `Entity`
  fields map to the client's entities (null when the target is not replicated or gone), a changed
  layout is skipped while the rest applies, `assign_tagged` in and out of a phase, `NetOwner` travels.
  Delta snapshots: smaller after the first acknowledgement and mirroring exactly, spawns and despawns
  through deltas, loss and late acknowledgements, a reset client and `forget_client`, the same tick
  written twice, every truncation and a flipped byte rejected, `NetRelevance` in and out of range,
  a spectator's view point and `set_sees_all`. Rollback: rollbacks and hashes agree with the game run
  in one place, lockstep, an injected desync; a spectator matches the players' hashes and catches
  its own desync; clock sync halves a 5%-fast peer's stalls and leaves peers on one clock alone;
  spectator packets are refused where they do not belong.
- `sample_net_rollback` (and `_drift`: a fast peer and two spectators), `sample_net_headless_spectators`
  (two watching clients mirror everything over 75 ms with loss).
- `sample_net_headless_300`: 300 ticks of a server and client; the client's mirror must match the
  server bit for bit and the client-driven player must have moved.
- The SDK contract test (`tests/sdk`) builds a template project with `DEPS mce::mce_net` against the
  installed SDK and runs it headless.

## Design records

- ADR-0035 (`docs/adr/0035-optional-networking.md`): why client-server over lockstep, why ENet, the
  opt-in linking, the snapshot format.
- REQUIREMENTS §3.14 FR-NET; N1/N7 reopened on 2026-10-10 by the user's decision.

## Known gaps

- No client-side prediction, reconciliation or interpolation buffer: a client shows the newest
  snapshot as-is (smooth enough on a LAN at 60 Hz; visible steps at high latency).
- Full snapshots only: no delta compression, no interest management (every client gets every entity).
- No encryption or authentication (put a TLS proxy in front of `mce_relay`'s WebSocket port for `wss://`); no
  matchmaking, host migration or WebRTC. Players who cannot accept connections meet through `mce_relay`.
- The editor does not run multiplayer sessions: play-in-editor is one world. Test with a server host
  and a client host (or one process, as the sample does).
- A shipped game that links `mce_net` must carry ENet's MIT notice in its credits.
