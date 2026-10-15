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

## 20.7 What it does not do yet

Clients show the newest snapshot without prediction or smoothing, every client receives every
replicated entity, and traffic is not encrypted. There are no lobbies, matchmaking or NAT traversal:
players connect to an address and port. If you ship a game that links `mce_net`, list ENet (MIT
licence) in your `CREDITS.md`.
