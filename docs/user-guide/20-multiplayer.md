# 20. Multiplayer (optional)

Networking is an **opt-in** module, `mce_net`. A game that does not ask for it is built exactly as
before, with no socket code in it. A game that does gets three things: a UDP connection layer, a
small message codec, and server-authoritative replication of entities. The reference page is
[`docs/engine/net.md`](../engine/net.md); the decision record is ADR-0035.

## 20.1 The model

One process is the **server**: it runs the real simulation. Players run **clients**: they send their
inputs to the server and show what the server sends back. Every tick the server sends a snapshot of the
entities you marked as replicated; each client mirrors it. Clients can be on different operating
systems, because they never simulate the shared state themselves.

## 20.2 Turn it on

In your project's `CMakeLists.txt`, add one line to the `mce_add_project` call:

```cmake
mce_add_project(my_game
    ...
    INCLUDE_DIRS games/my_game/module
    DEPS mce::mce_net)
```

Then `#include "mce/net/net.h"` in your game code. This works in the engine checkout and against the
installed SDK (`mce build`).

## 20.3 Connect

```cpp
// Server
net::Host server;
net::HostDesc d;
d.port = 27015;          // the port players connect to
d.max_peers = 8;
d.channels = 3;          // e.g. 0 = reliable messages, 1 = snapshots, 2 = inputs
server.listen(d);

// Client
net::Host client;
net::HostDesc c = net::Host::client_desc();
c.channels = 3;
client.open_client(c);
net::PeerId to_server = client.connect("192.168.1.20", 27015);
```

Call `poll()` once per frame on each side and handle the events:

```cpp
for (net::Event ev; host.poll(ev);) {
    switch (ev.kind) {
        case net::EventKind::Connected:    /* a player joined / we reached the server */ break;
        case net::EventKind::Disconnected: /* a player left / we lost the server */ break;
        case net::EventKind::Received:     /* ev.channel, ev.data */ break;
    }
}
```

Send with `send(peer, channel, bytes, delivery)` or `broadcast(channel, bytes, delivery)`. Pick the
delivery by what the message is:

| Delivery | Use it for |
|---|---|
| `Reliable` | things that must arrive once, in order: join, chat, "door opened", "level won" |
| `Sequenced` | state that is replaced every tick: snapshots, held-down inputs |
| `Unreliable` | fire-and-forget effects where order does not matter |

Write and read messages with `net::ByteWriter` and `net::ByteReader`. A reader that runs past the end
of a message sets `ok()` to false instead of crashing, so check it before acting on what you read, and
clamp numbers that come from the network.

## 20.4 Replicate entities

On the server, give each entity players should see a `NetId` with `ReplicationServer::assign`, and
broadcast a snapshot every tick:

```cpp
net::ReplicationServer rep;
rep.assign(world, enemy, kKindEnemy);   // kKindEnemy: your own number for "what this is"

world.tick(dt, ++tick);
writer.clear();
rep.write_snapshot(world, u32(tick), writer);
server.broadcast(1, writer.data(), net::Delivery::Sequenced);
```

On the client, apply the newest snapshot from a `FixedUpdate_Pre` system. New entities appear with
`NetId` and `LocalTransform`; use `on_spawn` to add what the client draws:

```cpp
net::ReplicationHooks hooks;
hooks.on_spawn = [](void*, Commands& cmd, Entity e, const net::NetId& id) {
    if (id.kind == kKindEnemy) cmd.add(e, enemy_sprite());
};
client_rep.set_hooks(hooks);
// in the system:
client_rep.apply(f.commands(), f.world(), latest_snapshot);
```

To send more than position, rotation and scale (health, an animation state), set `write_extra` on the
server and `read_extra` on the client. When the client disconnects, call `client_rep.reset(...)`.

**Smooth movement.** Applied as they land, snapshots make remote entities stutter: packets arrive
unevenly and some never arrive. Turn on interpolation and the client draws every entity slightly in
the past, blending the two snapshots around that moment:

```cpp
client_rep.set_interpolation({});                       // before the first apply
// in the system, every tick:
if (have_snapshot) client_rep.apply(f.commands(), f.world(), latest_snapshot);
client_rep.update(f.commands(), f.world(), f.dt);      // writes LocalTransform
```

How far in the past it draws (the jitter buffer) adapts to the connection by default. It is two
snapshot intervals plus how late snapshots have been arriving, kept between 50 and 350 ms. Set
`InterpolationDesc::delay_s` to fix it instead, and `tick_rate` if the server does not tick at 60 Hz.
Changes to the buffer never make entities jump: the client speeds up or slows down by at most 10%.

When no new snapshot has arrived, an entity keeps moving at its last velocity for up to
`max_extrapolation_s` (0.1 s). After that it stops at the newest snapshot's position.
`interpolation_stats()` reports the buffer, the measured jitter, and how many entities were
interpolated, extrapolated or held. Interpolation runs on the client's own tick time, so a run over
`SimNetwork` stays deterministic.

On a 75 ms link with 20 ms of jitter and 5% loss, `sample_net_headless -sim 1` shows the difference.
A drifter's mean per-tick change in velocity is about 0.018 with `-interp 0` and about 0.001 with
interpolation. The server's own value is 0.0003.

**Your own player, without the round trip.** Remote entities can be drawn in the past; the one the
player controls cannot. Waiting for the server's snapshot puts a round trip plus the jitter buffer
between a key press and the move on screen. Client-side prediction (`mce/net/prediction.h`) runs the
player's inputs through the same step function the server uses and draws the result at once. The
server stays in charge:

```cpp
struct Input { f32 move_x; };                         // trivially copyable, small
struct State { LocalTransform transform; };           // what the step changes; may hold more
void step(void*, State& s, const Input& in, f32 dt);  // the SAME code on client and server

// client
net::Predictor<Input, State> predictor;
predictor.init({}, step);
client_rep.set_predicted(my_net_id);                  // snapshots stop drawing it
// when it first appears: predictor.reset(State{*client_rep.latest_transform(my_net_id)});
predictor.push(Input{axis});                          // every tick
if (predictor.write_inputs(w)) host.send(server, input_channel, w.data(), net::Delivery::Sequenced);
// on each snapshot of it, with the ack the server sent along (e.g. in write_extra):
predictor.reconcile(ack, State{*client_rep.latest_transform(my_net_id)});
predictor.smooth(f.dt);
f.commands().set(my_entity, predictor.displayed());

// server, one per player
net::InputReceiver<Input> inputs;
inputs.receive(packet, [&](const Input& in) { step(nullptr, state, in, dt); });
// write inputs.last_seq() next to the player's state in the snapshot: that is the ack
```

Each input packet repeats the newest unacknowledged inputs (8 by default), so one lost packet costs
nothing, and the server runs each input exactly once, in order. On a snapshot, `reconcile` drops the
inputs the server has run, starts again from the server's state and replays the rest. When the server
disagreed (an input it never got, a push only it knew about), the prediction corrects at once but the
drawn player glides to it: `smooth` removes the difference at `correction_rate` (10 per second by
default). An error larger than `snap_distance` (2 units) is a teleport and snaps. `stats()` counts
pending inputs, corrections and snaps.

The step must give the same result on both sides for the same state and input, so keep it free of
anything only one side has (the wall clock, random numbers without a shared seed). Inputs travel as
their raw bytes, so the server should treat them as untrusted: clamp them in the step function.

`sample_net_headless -sim 1` predicts each client's player; a gust that only the server knows about
pushes players back every two seconds. At 75 ms with 20 ms of jitter, the player is drawn about 200 ms
behind its input with `-predict 0` and about 10 ms with prediction (what remains is the gusts being
smoothed in).

**Fair hits.** Interpolation draws other entities in the past, so a player who fires at what they see
is aiming at where the target was. Checked against the server's present, a shot that looked dead on
misses. Lag compensation (`mce/net/lag_compensation.h`) keeps a short history of hit boxes on the
server and checks each shot against the moment its client was drawing:

```cpp
// server
net::LagCompensator lag;
lag.init({60.0, 0.25});                               // tick rate, longest rewind in seconds
world.add(target, net::HitBox{{0.5f, 1.0f, 0.5f}, {0, 1, 0}});  // half extents, offset (local)
lag.record(world, tick);                              // after each tick, the snapshot's tick number
// on a shot from a client:
net::RewindHit hit;
if (lag.raycast(shot.view_tick, shot.origin, shot.direction, range, hit, shooter_net_id)) {
    // hit.net_id, hit.entity, hit.distance, hit.point
}

// client, when firing at a target:
shot.view_tick = client_rep.drawn_tick(target_net_id);  // the server tick it shows
```

Entities take part when they have `NetId`, `LocalTransform` and `HitBox`, a box in the entity's own
space that turns and scales with it. The history is blended the same way the client draws it, so a
shot at the drawn centre of a target hits it. `transform_at` gives the rewound transform for other
checks (a melee swing, an area).

The rewind is limited by `max_rewind_s`. A view older than that (a very slow link, or a cheat) is
checked at the oldest moment allowed, and a view in the future is checked at the present. Keep the
limit near your worst expected round trip plus the jitter buffer: a long rewind lets a laggy player
hit someone who already ducked behind a wall.

`sample_net_headless -sim 1` fires at a moving drifter every half second from each client. At 75 ms
with 20 ms of jitter, every rewound shot hits and about 30% would hit at the server's present. At
150 ms none would. A reliable shot that is lost and resent can arrive after the rewind window; the
sample counts those separately.

## 20.5 Try it

`samples/net_headless` runs a server and a client in one process over loopback and checks that the
client's copy matches the server exactly:

```
build/linux-gcc-debug/samples/net_headless/sample_net_headless -ticks 300
```

To test your own game on one machine, run a server host and a client host connecting to
`127.0.0.1`. The editor's play mode runs a single world, so it does not play multiplayer sessions.

## 20.6 Test on a bad network

Loopback is faster than any real connection. Two tools let you see how your game behaves on a slow,
jittery or lossy one.

**`Transport`.** `Host` and `SimEndpoint` are both `net::Transport`s, with the same `poll`, `send`,
`broadcast`, `flush` and `disconnect`. Write your server and client against `net::Transport&`, and the
same code runs over real UDP or over the simulated network.

**`SimNetwork`.** An in-memory network on a virtual clock. Every message is delayed by the link's latency
plus some jitter, and may be lost:

```cpp
net::SimNetwork network(/*seed*/ 1);
network.set_default_conditions({.latency_ms = 75, .jitter_ms = 20, .loss = 0.05f});
net::SimEndpoint& server = network.add_endpoint(8, 3);   // up to 8 peers, 3 channels
net::SimEndpoint& client = network.add_endpoint(1, 3);
const net::PeerId to_server = client.connect(server);
// each tick: poll and send on every endpoint, tick the worlds, then
network.advance(16);                                     // milliseconds of virtual time
```

The delivery mode decides what loss and jitter do. `Reliable` messages are never lost: a lost one
arrives a round trip later. `Reliable` and `Sequenced` messages keep their order. A `Sequenced`
message older than one already delivered is dropped. `Unreliable` messages can arrive in any order.
`set_conditions(from, to, ...)` sets one direction of one link.

Nothing depends on the wall clock, so the same seed gives the same deliveries every time. A server
and several clients in one process give the same state hashes on every run and in Debug and Release.
That makes it the place to test multiplayer code headless:

```
sample_net_headless -sim 1 -clients 4 -latency 75 -jitter 20 -loss 0.05 -ticks 600
```

`local_ci.sh`'s determinism stage runs this sample at 75 ms and at 150 ms with 15% loss, and checks
that every client mirrors the server and that the hash never changes.

**`Host::set_conditions`.** Plays a real build over a simulated bad network. It holds each outgoing
message for the latency plus jitter before ENet sends it, and drops `Sequenced` and `Unreliable`
messages at the loss rate. It uses the wall clock, so set it on both the server and the client for
the full round trip. `sample_net_headless -latency 75 -loss 0.05` runs the UDP sample this way.

**Net stats.** `net::NetStatsMeter` turns a transport's counters into peers, round trip, loss,
bandwidth and dropped messages per second. Call `meter.update(transport, frame_seconds)` each frame and
`meter.publish(world)`, and the stock player's stats overlay (F3, `-r.stats 1`) shows a `NET` line
under the render stats:

```
NET 1 peer  RTT 152 ms +-30  LOSS 5.0%  UP 3.2 KB/s  DOWN 41.0 KB/s  DROP 12
```

`publish` writes the world's `DevStatsLines` resource (`mce/ecs/dev_stats.h`). Any module can add a
line there the same way.

## 20.7 Rollback for small games

Everything above is server-authoritative: one machine runs the game and the others mirror it. A
fighting game or a two-to-four player arena often works better the other way round. Every peer
runs the whole game, and only inputs travel. `net::RollbackSession` (`mce/net/rollback.h`) makes
that feel immediate on a slow link:

- Your own input is used after a short delay (`input_delay`, 2 frames).
- Other players' inputs that have not arrived yet are guessed (their last known input), so the
  game never waits for them.
- When a real input arrives that differs from the guess, the session loads the state saved before
  that frame and plays the frames since again with the real input. This is a rollback.
- It waits only when a peer falls more than `max_prediction` frames behind (8 by default). With
  `max_prediction` 0 it is lockstep: it never guesses and waits for every input.

```cpp
net::RollbackSession session;
session.init({.players = 2, .local_player = me, .input_size = sizeof(Input)},
             {&game, save_state, load_state, advance_frame});
// every tick:
session.add_local_input(Span<const u8>(reinterpret_cast<const u8*>(&input), sizeof input));
session.advance();                                   // false: waiting for a slow peer
for (u32 p = 0; p < players; ++p)
    if (p != me && session.write_packet(p, w)) host.send(peer_of[p], 0, w.data(), net::Delivery::Unreliable);
// on a packet from any peer:
session.receive(event.data);
```

You write three functions. `save_state` writes everything the simulation reads, `load_state` puts it
back, and `advance_frame` runs one frame with every player's input. `advance_frame` also runs again
during a rollback, so play sounds and spawn effects from the newest state after `advance()` returns,
not from inside it. The simulation must be deterministic: the engine's fixed timestep, no wall
clock, randomness from a seed in the state.

**Desyncs are caught.** Once every input up to a frame is known, each peer hashes its saved state
after that frame and sends the hash with its inputs. If two peers computed different states,
`desynced()` turns true on both and `desync_frame()` names the first frame that differed. That is
where to start looking for the non-deterministic code.

`samples/net_rollback` runs four peers over a simulated 100 ms link with 20 ms of jitter and 5% loss:

```
build/linux-gcc-debug/samples/net_rollback/sample_net_rollback
```

Each peer rolls back about 230 times in 900 frames (8 frames at most) and waits for 11 frames, and
every peer ends with the same hash. `-prediction 0` shows lockstep at the same latency: it plays only
about a third of the frames, waiting the rest of the time. `-desync_at 300` makes one peer compute
something else at frame 300, and every peer reports a desync at frame 300.

**Spectators.** Someone who only watches runs a session with `.spectator = true` (and their spectator
number, 0 to 7, as `local_player`). A spectator has no input and never guesses: it plays a frame
once every player's input for it is known, a little behind the players. One player feeds it:

```cpp
// the feeding player, every tick, for each spectator s it feeds:
if (session.write_spectator_packet(s, w)) host.send(spectator_peer[s], 0, w.data(), net::Delivery::Unreliable);
// the spectator, every tick:
watcher.advance();
if (watcher.write_packet(feeding_player, w)) host.send(player_peer, 0, w.data(), net::Delivery::Unreliable);
```

The spectator checks the player's state hashes like a player would, so `desynced()` works for it too.
The sample adds one spectator by default; `-spectators N` adds more.

**Clocks that drift.** Two machines never tick at exactly the same rate. Without help, the faster
peer keeps running into `max_prediction` and stalls in bursts, and the other keeps rolling back deep.
With `time_sync` (on by default) the packets carry how far ahead each peer sees the other, and a peer
that is more than a frame ahead skips a tick now and then (`stats().sync_waits`), so the gap closes
in small steps. `-drift 30` makes the sample's peer 0 play an extra frame every 30 ticks. With
`-sync 0` it stalls 40 times; with clock sync it skips 33 ticks spread over the run and stalls 10
times, and every peer replays fewer frames.

## 20.8 Rooms through a relay, and web players

Players at home usually cannot accept connections: their router does not forward a port to them,
and a browser cannot open a UDP socket at all. A **relay** solves both. Everybody connects out to
`mce_relay`, a small server you run somewhere public. The host makes a room and gets a six-letter
code to share. The others join with that code, and the relay passes messages between the members.
Nobody forwards a port, and a native build and a web build can play in the same room.

Start the relay. It listens for native players on UDP port 7777 and for browsers on WebSocket port
7778:

```
build/linux-gcc-debug/src/engine/net/relay/mce_relay
build/linux-gcc-debug/src/engine/net/relay/mce_relay -selftest 1   # a native and a web client meet, then exit
```

In the game, connect to the relay instead of to a server, and use a `net::RelayClient` as the
Transport. Everything written against Transport (replication, prediction, rollback) works through
it unchanged:

```cpp
net::Host link;                        // a web build uses net::WebSocketHost and port 7778
link.open_client();
net::RelayClient room;
room.start(link, link.connect("relay.example.com", 7777), {.channels = 3});
room.create_room(4);                   // the host
room.join_room("K7TQ2M");              // everybody else, with the code the host shows
// once room.state() is InRoom: room.room_code() is the code to show; poll the room, not the link
for (net::Event ev; room.poll(ev);) { /* Connected / Received / Disconnected, per member */ }
```

Members are peers: the host starts as peer 0, and joiners get the lowest free number.
A client therefore sends to the host (`room.host_id()`, peer 0 unless the host changed) where it used
to send to the server. When a member leaves, the
others see it disconnect. When the host leaves, the room closes and `state()` becomes `Closed`.
A wrong code, a full room or a relay with too many rooms gives `Refused`, and `refusal()` says
which.

`net::WebSocketHost` is also usable on its own. It is a Transport over WebSocket, server or client,
for a game server that browsers connect to directly. Over WebSocket every message is reliable and
ordered, because it runs on TCP.

`sample_net_headless -sim 1 -relay 1` plays the server and its clients through a relay room over the
simulated network. Each message takes two trips (to the relay and from it), so `-latency` counts
twice.

**Quick play and the room list.** Players who have no code can ask the relay for a game. `quick_play`
puts the player in the fullest open room of that mode and size, or makes one with the player as host.
`list_rooms` fetches the rooms a lobby screen can show:

```cpp
room.quick_play("arena", 4);           // then the same states as create_room / join_room
room.list_rooms("arena");              // an empty mode lists every mode
// once room.rooms_received() has gone up:
for (const net::RelayRoomInfo& r : room.rooms()) { /* r.code, r.mode, r.members, r.max_members */ }
// a room of your own that others can find:
room.create_room(4, {.listed = true, .host_migration = true, .mode = "arena"});
```

Only listed rooms show up in the list or take quick-play players. A room made with a code stays
private.

**When the host leaves.** Normally the room closes with its host. A room made with `host_migration`
(and every quick-play room) goes on instead: the relay names the lowest member still in it the new
host. Every member's `on_host_change` callback runs from inside `poll()`, before the old host's
`Disconnected` arrives:

```cpp
net::RelayClientDesc desc;
desc.channels = 3;
desc.on_host_change = [](void* user, net::PeerId new_host) { static_cast<Game*>(user)->host_changed(new_host); };
desc.user = &game;
room.start(link, relay_peer, desc);

// in host_changed: if new_host == room.member_id(), this machine is the server now:
server_rep.adopt(world, client_rep);   // the mirror becomes the server world; returns the last tick
// ...then add back what the snapshots did not carry (server-only components, AI, timers).
// Everybody else: send to new_host from now on, and tell it which player entity was yours.
```

`adopt` keeps every entity's `NetId`, puts back the transform of the newest snapshot, and continues
the ids after the highest one. The other clients' mirrors carry on: their next snapshots from the
new host are full, then deltas again. Anything only the old host knew and never sent is lost, so put
what matters in the snapshot (`Replicated` fields) or rebuild it from what is there.

`sample_net_headless -sim 1 -relay 1 -host_leaves 300` shows it. The host leaves at tick 300, and
the relay hands the room to a client. That client's mirror becomes the server, the others get their
players back with a JOIN that names them, and at the end every remaining client matches the new host
bit for bit.

## 20.9 Replication as data

You can mark what replicates in the scene instead of writing `assign` and `write_extra` code. Add
`NetReplicated` to every entity the clients should see, and set its `kind` to your own archetype
number, the same one `on_spawn` receives. After the server loads the scene, one call gives all of
those entities their `NetId`:

```cpp
rep.assign_tagged(world);              // after loading; again each tick to pick up new entities
rep.assign_tagged(f.commands(), f.world());   // the same from inside a system
```

Fields opt in to replication with the `Replicated` attribute, the way `Save` opts them in to save
games:

```cpp
struct Health { f32 hp = 100; f32 regen = 1; Entity attacker; };
MCE_COMPONENT(Health, (hp, Replicated), regen, (attacker, Replicated))
```

Every snapshot carries the flagged fields of every component on a replicated entity. The client
writes them into its own copy and adds the component when the entity does not have it yet. Fields
without the flag keep the client's own value, so a component can mix server state with things only
the client uses. An `Entity` field arrives as the client's own entity for the same object, or as
null when the target is not replicated. `NetOwner { peer }` names the player that controls an entity
and is replicated itself, so each client can find its own player. Through a relay room the peer is
the member number, which is the same on every member.

Both builds must agree on a component's flagged fields. When a client runs a build where they differ
(a field renamed, added or resized), that component is skipped and counted in
`ApplyResult::skipped_components` instead of being read wrongly, and the rest of the snapshot still
applies. Flagged fields cost bytes in every snapshot for every entity, so flag what clients draw or
show, not the whole component. `refl_describe_component` over MCP marks these fields
`"replicated": true`, so an agent can see and set up replication without reading code.

## 20.10 Send each client less

A broadcast snapshot sends every replicated entity in full to every client, every tick. With
per-client snapshots the server writes each client its own, and two things shrink it. First, only the
entities that client needs are listed. Second, each entity is coded against the last snapshot that
client confirmed: an entity that did not change costs two bytes, and a moving one only the parts of
its transform that changed. In `sample_net_headless` that cuts the snapshot traffic to about a third.

```cpp
// server, every send tick, instead of broadcast:
for (net::PeerId peer : players) {     // your connected peers
    writer.clear();
    rep.write_snapshot_for(world, u32(tick), peer, writer);
    server.send(peer, 1, writer.data(), net::Delivery::Sequenced);
}
// server, when a client's acknowledgement arrives (and forget_client(peer) on disconnect):
rep.read_ack(peer, reader);
// client, with its inputs every tick:
client_rep.write_ack(writer);
```

`client_rep.apply(...)` reads both kinds. Until the first acknowledgement arrives, and whenever the
last one is too old, the client gets a full snapshot. If a client resets, its next acknowledgement
says so and the server starts over with a full snapshot.

To keep far-away entities off a client, give them `NetRelevance { radius }`. Such an entity goes only
to clients that own an entity (`NetOwner`) within that radius, and always to the client that owns it.
Entities without `NetRelevance`, or with radius 0, go to everyone. An entity that leaves a client's
range disappears from that client's world and comes back when it is in range again. An `Entity`
field that points at an entity the client does not have arrives as null.

A spectator owns nothing, so it would see only the entities without `NetRelevance`. Give it a camera
with `rep.set_view(peer, &camera_position)` (call it again as the camera moves) or let it see
everything with `rep.set_sees_all(peer, true)`, for a caster or a replay recorder. It then gets
per-client snapshots like a player and acknowledges them the same way.
`sample_net_headless -sim 1 -clients 3 -spectators 2` adds two clients that watch without playing.

## 20.11 What it does not do yet

Traffic is not encrypted: the relay and WebSocket speak plain `ws://`, so put a
TLS proxy in front of `mce_relay` for pages served over `https://`. Quick play matches by mode and
size only, with no skill rating or region. There is no WebRTC or direct peer-to-peer connection:
through a relay every message passes through it. If you ship a game that links `mce_net`, list ENet (MIT
licence) in your `CREDITS.md`.
