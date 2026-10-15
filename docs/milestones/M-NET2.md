# M-NET2 — Multiplayer that games can ship (milestone record)

**Status:** active. Steps 1–2 merged into `dev_claude`; steps 3–7 to come (Linear SBA-82 … SBA-88).
**Why:** M-NET gives server-authoritative snapshots over ENet with no prediction or interpolation, so a moving player
feels the round trip (ROADMAP "Engine gap tracks"; comparison in the project's `assessments/motor-kiyaslama-eksikler.md`).
**Measured on:** the Linux CI container.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. Network test harness: simulated latency, jitter and loss; headless multi-client runner in `local_ci.sh`; net stats in the dev overlay | SBA-82 | done |
| 2. Snapshot interpolation for remote entities, jitter buffer, extrapolation limits | SBA-83 | done |
| 3. Client-side prediction and server reconciliation, misprediction smoothing | SBA-84 | — |
| 4. Lag compensation for hit checks (rewound colliders) | SBA-85 | — |
| 5. Deterministic rollback/lockstep mode with desync detection by hash | SBA-86 | — |
| 6. Sessions: `mce_relay` relay/lobby server, room codes; WebSocket/WebRTC transport | SBA-87 | — |
| 7. Replication as data: `NetReplicated` / `NetOwner`, per-field flags, MCP, user guide | SBA-88 | — |

## Step 1 — the network test harness

| Item | Where | Evidence |
|---|---|---|
| `net::Transport`: the interface game code sends and receives through; `Host` implements it, so the same loop runs over UDP or the simulated network. `LinkConditions` (latency, jitter, loss) and per-endpoint `TransportCounters` | `mce/net/transport.h`, `host.cpp` | `test_net` (the existing host tests run unchanged through the new interface) |
| `net::SimNetwork` / `SimEndpoint`: an in-memory network on a virtual clock. Messages are delayed by latency plus a uniform 0..jitter and lost at the link's rate from a seeded xorshift generator. `Reliable` is never lost (a lost send arrives a round trip later), `Reliable` and `Sequenced` keep channel order, stale `Sequenced` messages are dropped, `Unreliable` may reorder. Handshake, refusal when full and graceful disconnect follow `Host` | `mce/net/sim_network.h`, `sim_network.cpp`, `link_sim.h` | `test_net` "messages arrive after the link's latency, and the handshake takes a round trip", "jitter reorders unreliable messages but never reliable or sequenced ones", "loss drops sequenced and unreliable messages at its rate and delays reliable ones" (25% loss: 72–78% kept, every reliable message in order), "the same seed gives the same deliveries; another seed does not", "a full server refuses, and a disconnect reaches the other side after its messages" |
| `Host::set_conditions`: the same conditions on a real host's outgoing messages, on the wall clock (play a build "at 150 ms") | `host.h`, `host.cpp` | `test_net` "set_conditions holds outgoing messages back and drops unreliable ones" (a reliable message takes ≥ the latency; a sequenced one at loss 1.0 never arrives) |
| Headless multi-client runner: `sample_net_headless -sim 1 -clients N -latency -jitter -loss -seed` runs a server and N clients over `SimNetwork`, checks every client mirrors the server bit for bit and each player moved by its own input, and prints one hash of every world and the traffic counters | `samples/net_headless/main.cpp` | ctest `sample_net_headless_sim`; `local_ci.sh` determinism stage `stage_net_determinism`: 4 clients at 75 ms ± 20, 5% loss, 600 ticks, and 8 clients at 150 ms ± 40, 15% loss, 900 ticks, 3 runs each in Debug and Release with one hash |
| Net stats in the dev overlay: `NetStatsMeter` (peers, worst RTT and loss, bytes and messages per second, drops) publishes a `NET` line to the world's `DevStatsLines`, which the stock player draws under its stats line (F3, `r.stats`) | `mce/net/net_stats.h`, `mce/ecs/dev_stats.h`, `player.cpp` | `test_net` "rates over a window, published as the overlay's net line"; the sample prints the server's line |

Sample output (4 clients, 75 ms ± 20, 5% loss, 600 ticks):

```
  client 0: 442 snapshots applied, 12/12 entities match, player x 9.717 -> OK
  server NET 4 peers  RTT 170 ms +-20  LOSS 5.0%  UP 123.0 KB/s  DOWN 1.1 KB/s  DROP 127
net_headless: OK, hash …
```

Notes:
- The player draws `DevStatsLines` without linking `mce_net`: the resource lives in `mce_ecs`, and resources are
  not part of `World::state_hash`, so publishing never changes a replay.
- `Host::set_conditions` loses only `Sequenced` and `Unreliable` messages. ENet's own reliability resends what the
  wire loses, and dropping a reliable message above it would break its contract.
- `SimNetwork` replies to a connect (accept, refuse) are timed from the connect's arrival, so a handshake takes
  exactly one round trip inside a single `advance()`.

## Step 2 — snapshot interpolation

| Item | Where | Evidence |
|---|---|---|
| `ReplicationClient::set_interpolation(InterpolationDesc)`: `apply()` records each entity's transform with its server tick (16 per entity), `update(cmd, world, dt)` writes `LocalTransform` for a render time behind the server, lerping position and scale and slerping rotation between the two snapshots around it | `mce/net/replication.h`, `replication.cpp` | `test_net` "the render time blends the two snapshots around it" |
| Jitter buffer: the server's clock is estimated from the earliest arrivals; the adaptive delay is two snapshot intervals plus a decaying peak of how late snapshots run, within 50–350 ms (or fixed with `delay_s`). The render time is steered toward its target at most 10% faster or slower, so a buffer change never jumps | same | `test_net` "the adaptive buffer grows with arrival jitter" (floor at no jitter, +50 ms or more at 80 ms) |
| Extrapolation limit: past the newest snapshot an entity moves on with its last velocity for `max_extrapolation_s` (0.1 s), then holds the newest snapshot exactly | same | `test_net` "past the newest snapshot it extrapolates up to the limit, then holds it exactly" |
| Smoothness under jitter and loss: over `SimNetwork` at 60 ms ± 40 with 10% loss, the mean per-tick change in velocity drops from 0.133 (raw) to 0.004 (interpolated) | `tests/net/test_interpolation.cpp` | `test_net` "over a jittery, lossy link the mirror moves smoothly; raw snapshots stutter" (asserts at least 10×) |
| The sample interpolates by default (`-interp 0` shows raw snapshots), prints a drifter's smoothness next to the server's, and hashes the path each client drew. The determinism stage's two runs cover interpolation | `samples/net_headless/main.cpp` | 4 clients at 75 ms ± 20, 5% loss: 0.0013 interpolated vs 0.018 raw (server 0.0003) |
