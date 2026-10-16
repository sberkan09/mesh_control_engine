# M-NET2 — Multiplayer that games can ship (milestone record)

**Status:** done. Steps 1–7 merged into `dev_claude` (Linear SBA-82 … SBA-88); see "Demo and gates" for what is
shown headless and what the M-WEB track verifies.
**Why:** M-NET gives server-authoritative snapshots over ENet with no prediction or interpolation, so a moving player
feels the round trip (ROADMAP "Engine gap tracks"; comparison in the project's `assessments/motor-kiyaslama-eksikler.md`).
**Measured on:** the Linux CI container.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. Network test harness: simulated latency, jitter and loss; headless multi-client runner in `local_ci.sh`; net stats in the dev overlay | SBA-82 | done |
| 2. Snapshot interpolation for remote entities, jitter buffer, extrapolation limits | SBA-83 | done |
| 3. Client-side prediction and server reconciliation, misprediction smoothing | SBA-84 | done |
| 4. Lag compensation for hit checks (rewound colliders) | SBA-85 | done |
| 5. Deterministic rollback/lockstep mode with desync detection by hash | SBA-86 | done |
| 6. Sessions: `mce_relay` relay/lobby server, room codes; WebSocket/WebRTC transport | SBA-87 | done (WebSocket; WebRTC not built) |
| 7. Replication as data: `NetReplicated` / `NetOwner`, per-field flags, MCP, user guide | SBA-88 | done |

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

## Step 3 — client-side prediction and server reconciliation

| Item | Where | Evidence |
|---|---|---|
| `net::Predictor<Input, State>`: `push()` numbers each input and steps the predicted state at once; `write_inputs()` sends the newest unacknowledged inputs (`redundancy`, 8) in a `MIN` packet; `reconcile(ack, server_state)` drops acknowledged inputs, restarts from the server's state and replays the rest; a stale ack is ignored | `mce/net/prediction.h`, `prediction.cpp` | `test_net` "when the server agrees, reconcile leaves the prediction bit for bit" (30 ticks, packets 6 ticks late, 0 corrections) |
| `net::InputReceiver<Input>`: the server runs each input once, in sequence order, across lost packets and late duplicates; a gap wider than the redundancy skips the lost inputs; malformed packets (wrong size, another input type, empty) run nothing; `last_seq()` is the ack | same | `test_net` "inputs travel with redundancy and the server runs each once, in order", "a gap wider than the redundancy skips the lost inputs", "malformed input packets run nothing" |
| Misprediction smoothing: a correction keeps the drawn transform where it was and removes the difference at `correction_rate` (10/s), position and rotation; below 0.1 mm it is gone and the drawn transform is the prediction bit for bit; past `snap_distance` (2) it snaps | same | `test_net` "a misprediction is replayed at once and smoothed on screen" (0.3 error: no drawn step over 0.06, exact after 2 s), "an error beyond the snap distance snaps" |
| `ReplicationClient::set_predicted(net_id)`: snapshots and interpolation leave that entity's `LocalTransform` to the game; `latest_transform(net_id)` is the server's word for `reconcile`; `InterpolationStats::predicted` counts it. Repeats of the newest snapshot no longer feed the jitter estimate | `mce/net/replication.h`, `replication.cpp` | `test_net` "the replication client leaves a predicted entity to the game" |
| The sample predicts each client's player (`-predict 0` draws it from snapshots). The server sends a WELCOME with the player's net id, acks inputs in the player's extra bytes, and pushes players back every 2 s with a gust only it knows about. The end-of-run check also waits for every input to be acknowledged and the smoothing to finish, then compares bit for bit | `samples/net_headless/main.cpp` | 4 clients at 75 ms ± 20, 5% loss: player drawn 12 ms behind its input predicted vs 201 ms from snapshots, 5 corrections (the gusts), 0 snaps; 8 clients at 150 ms ± 40, 15% loss: 7 corrections, every input arrived |

Notes:
- Input packets carry the inputs' raw bytes (trivially copyable, little-endian on every target), so
  the step function validates them; the server trusts no client.
- The server runs inputs as they arrive, one step each, the Quake usercmd model: a client that sends
  more inputs than ticks moves faster. Capping inputs per server tick is left to the game for now.

## Step 4 — lag compensation for hit checks

| Item | Where | Evidence |
|---|---|---|
| `net::LagCompensator`: `record(world, tick)` keeps every `NetId` + `LocalTransform` + `HitBox` entity per tick in a ring sized by `max_rewind_s`; `raycast(view_tick, origin, dir, range, hit, ignore)` finds the nearest box at that moment, blended between ticks the way `ReplicationClient` draws; `transform_at` gives the rewound transform for other checks | `mce/net/lag_compensation.h`, `lag_compensation.cpp` | `test_net` "a shot at where the client saw the target hits when rewound, misses at present", "transforms between ticks, and entities that came or went" |
| `HitBox`: an oriented box in the entity's local space (half extents, offset), turned and scaled with it; `ray_hits_box` is the slab test | same | `test_net` "rays against oriented, scaled boxes" |
| Rewind limit: a view older than `max_rewind_s` is checked at the oldest allowed moment, a future view (or NaN) at the present | same | `test_net` "the rewind is limited, and a view from the future is the present" |
| `ReplicationClient::drawn_tick(net_id)`: the server tick an entity shows (the render tick while interpolated or extrapolated, the snapshot's tick while held), sent with a shot; `render_tick()` for the whole view | `mce/net/replication.h`, `replication.cpp` | `test_net` "over a 100 ms link, shots at the drawn target hit only when rewound" (24 of 24 rewound, 0 at present) |
| The sample: every client fires at drifter 1 from the origin every half second with its drawn tick; the server counts hits rewound and at its present. The end-of-run check also waits for the clients to hold the server's newest snapshot bytes, not just its tick (a late input can change the state after the last tick) | `samples/net_headless/main.cpp` | 4 clients at 75 ms ± 20, 5% loss: 64 of 64 rewound, 19 at present; 8 clients at 150 ms ± 40, 15% loss: every shot inside the 1 s rewind hits, 0 at present; 16 seeds checked |

Notes:
- A reliable shot lost several times at 15% loss can reach the server after the rewind window; it is
  then checked at the oldest allowed moment and usually misses. The sample counts those.
- Rewinding trusts the client's view tick within the limit. A cheater can claim the oldest allowed
  view; the limit is the bound on what that buys.

## Step 5 — rollback and lockstep with desync detection

| Item | Where | Evidence |
|---|---|---|
| `net::RollbackSession`: input delay, prediction of missing remote inputs (their last known input), rollback on a wrong guess (load the state saved before that frame, simulate forward again), a stall only past `max_prediction`; `max_prediction` 0 is lockstep. The game supplies save, load and advance callbacks | `mce/net/rollback.h`, `rollback.cpp` | `test_net` "two peers 6 ticks apart roll back on wrong guesses and stay in sync" (a confirmed frame equals the game run in one place with every input known), "four peers over a jittery, lossy link stay in sync", "lockstep waits instead of guessing" |
| Inputs travel in `MRB` packets that repeat every frame the receiver has not acknowledged, with an ack of its inputs, so loss costs nothing; acks never run ahead of what was sent | same | the 10%-loss test above; `test_net` "malformed packets and bad setups are refused" |
| Desync detection: once every input up to a frame is known, each peer hashes its saved state after it and sends the newest 16 hashes; a different hash for the same frame sets `desynced()` and `desync_frame()` (the first) on both sides | same | `test_net` "a desync is reported at the frame it happened, on both sides" (frame 100) |
| `samples/net_rollback`: N peers, each with its own `World` (bodies steered by seeded input scripts, a shared ball), a full mesh over `SimNetwork`; checks every peer hashed the same recent frame identically, or with `-desync_at F` that every peer caught it at F. ctest runs the default, lockstep and desync cases; `local_ci.sh`'s determinism stage runs the default and lockstep across runs and Debug/Release | `samples/net_rollback/main.cpp`, `scripts/ci/local_ci.sh` | 4 peers at 100 ms ± 20, 5% loss, 900 frames: about 230 rollbacks each (deepest 8), 11 stalled frames, all agree; lockstep at the same latency: 106 frames in 300 ticks; desync at 300: caught by every peer at 300 |

Notes:
- The state is the game's own: the sample saves its bodies' transforms and velocities. A general
  `World` snapshot (every component, entities spawned and destroyed during a rollback) is not built;
  it would make rollback a drop-in for any scene.
- There is no time synchronisation: a peer whose clock runs ahead waits more often instead of being
  slowed down smoothly.

## Step 6 — rooms through a relay, and a WebSocket transport

| Item | Where | Evidence |
|---|---|---|
| `net::WebSocketHost`: a Transport over RFC 6455 WebSocket, server (`listen`) and client (`connect`), native over non-blocking TCP; binary messages `[channel][payload]`, masked client frames, 7/16/64-bit lengths, fragments, ping/pong round trip in `stats()`, close frames carrying the disconnect `user_data`, a size limit (1009) and handshake and silence timeouts | `mce/net/websocket.h`, `src/websocket.cpp`, `src/socket.cpp`, `src/ws_crypto.cpp` | `test_net` "the handshake's accept key matches RFC 6455's example", "a client connects over loopback, messages of every length cross, then it disconnects" (0 B to 70,000 B both ways), "the server closing tells the client, and a full server turns a client away", "a message over the limit closes the connection; nobody listening fails the connect" |
| The web build's client over the browser's WebSocket (`emscripten_websocket_*`, `-lwebsocket.js`); `listen()` fails there | `src/websocket_web.cpp` | not compiled by `local_ci.sh` (no Emscripten in the CI container). The M-WEB track compiles it in its web-release build (`-Werror`, `MCE_WITH_NET=ON`) and runs it in headless Chromium with its `mce_relay_web_probe` (SBA-65, see `M-WEB.md`) |
| `net::RelayServer`: rooms with six-character codes (no 0/O/1/I, seeded), up to 8 members, host is member 0, joiners take the lowest free id; forwarding to one member or all with the sender's delivery; refusals (no such room, full, already in a room, bad request, too many rooms); the host leaving closes the room, the host can remove a member; served over any number of Transports at once | `mce/net/relay.h`, `src/relay.cpp` | `test_net` "a host makes a room, others join with its code, and members are peers", "codes are case-insensitive, and wrong codes, full rooms and double joins are refused", "the host can remove a member, a lost link closes the room, and room limits hold" |
| `net::RelayClient`: a Transport whose peers are the room's members, so replication, prediction and rollback run through a room unchanged | same | `test_net` "snapshot replication runs unchanged through a room"; `sample_net_headless -sim 1 -relay 1` (ctest `sample_net_headless_relay`; `local_ci.sh` determinism stage across runs and Debug/Release): 3 clients at 40 ms ± 10 per hop, 5% loss, 600 ticks, every mirror matches, 48 of 48 shots hit when rewound |
| `mce_relay`: UDP (ENet) on 7777 and WebSocket on 7778 in one process, counters in the log; `-selftest 1` puts a native client (UDP) and a WebSocket client in one room over loopback, sends both ways, checks a third is refused and that the room closes with its host | `src/engine/net/relay/` (under `src/engine/net/` because only mce_net may link mce_net) | ctest `mce_relay_selftest` |
| Socket wall: socket headers only in `src/engine/net/src/` | `scripts/ci/check_walls.py` | `check_walls.py` |

Notes:
- WebRTC is not built: a browser-to-browser or browser-to-native connection without the relay needs
  ICE/STUN/DTLS, a large dependency for a solo track. Through the relay, web and native builds
  already play together; WebRTC would only take the relay hop out.
- The relay and WebSocket speak plain `ws://`. A page served over `https://` may only open `wss://`,
  so a deployment puts a TLS proxy (Caddy, nginx) in front of the WebSocket port; the web client
  takes a `wss://host` address as it is.
- Over WebSocket, Sequenced and Unreliable messages are delivered reliably (TCP); a lossy browser link
  queues instead of dropping.
- The gate "a web build joins a native host through the relay": M-WEB's `mce_relay_web_probe` runs two
  `RelayClient`s over `websocket_web.cpp` in headless Chromium against a native `mce_relay`; one makes a
  room, the other joins, messages cross both ways ("relay web probe: OK"), from M-WEB's SBA-65; `local_ci.sh` has no
  Emscripten, so here the selftest covers the same path with the native WebSocket client.

## Step 7 — replication as data

| Item | Where | Evidence |
|---|---|---|
| `refl::attr::Replicated` (`FieldFlags::Replicated`): a field opts in to replication the way `Save` opts in to saves | `mce/refl/type.h`, `mce/refl/macros.h` | `test_net` "flagged fields reach the client, other fields keep the client's values" |
| `NetReplicated { kind }` and `NetOwner { peer }` in mce_ecs (no network code), so scenes, the editor, cook and MCP know them; `NetOwner::peer` is replicated | `mce/ecs/replicated.h`, registered by `world.cpp` and the tools' `engine_components.cpp` | `test_net` "NetReplicated entities get ids from assign_tagged, and NetOwner travels" |
| `ReplicationServer::assign_tagged(world)` / `(cmd, world)`: a NetId for every `NetReplicated` entity without one, kind from the component | `mce/net/replication.h`, `replication.cpp` | same (outside a phase, then through `Commands` for an entity spawned later) |
| Snapshot version 2: each entry's flagged fields after its extra bytes (type id, layout hash, field bytes); `Entity` fields as NetIds mapped back to the client's entities once every mirror exists; unflagged fields keep the client's value; a component missing on the client is added, an unknown type registered | `replication.cpp`, `Commands::set_raw` (`world.h`, `world.cpp`) | `test_net` "flagged fields reach the client…" (hp and the target arrive, a client-only field keeps 9.0 across snapshots, a target that is not replicated or gone arrives null) |
| A component whose flagged fields differ between server and client builds is skipped and counted (`ApplyResult::skipped_components`), never misread; the rest of the snapshot applies | `replication.cpp` (`replicated_layout`) | `test_net` "a component laid out differently on the client is skipped, not misread" |
| MCP: `refl_describe_component` marks flagged fields `"replicated": true` (and `"save": true`) | `src/editor/mcp/src/values.cpp` | `test_mcp` "discover components from mce_refl" (`NetOwner.peer` replicated, `NetReplicated.kind` not) |
| User guide 20.9 "Replication as data" (the old 20.9 is 20.10) | `docs/user-guide/20-multiplayer.md`, `docs/engine/net.md` | — |

Notes:
- Fields go as raw bytes in declaration order. That is safe because the layout hash covers each
  flagged field's name, kind, size and count, and every engine target is little-endian.
- Every flagged field of every replicated entity is in every snapshot (full snapshots, as before);
  there is no delta compression or interest management yet.

## Demo and gates

| Item | State |
|---|---|
| Demo: a 2–4 player game that feels local at 150 ms | shown headless: `sample_net_headless -sim 1 -clients 4 -latency 150 -jitter 40 -loss 0.15 -ticks 900`: each player is predicted, drawn 11 ms behind its input against 401 ms with `-predict 0`, every mirror matches the server and 104 of 104 rewound shots hit; no windowed arena was built |
| Demo: a rollback sample stays in sync at 100 ms | `samples/net_rollback`, 4 peers at 100 ms ± 20, 5% loss, 900 frames: all agree |
| Demo: a web build joins a native host through the relay | M-WEB's `mce_relay_web_probe` in headless Chromium (`scripts/web/probe_smoke.cjs <dir> relay <mce_relay>`, SBA-65): OK |
| Gate: headless multi-client runs give stable hashes | `local_ci.sh` determinism stage: same hash across runs and Debug/Release, direct and through the relay |
| Gate: rollback detects an injected desync | `sample_net_rollback -desync_at 300` (ctest), `test_net` |
| Gate: no sim code includes networking headers | `check_walls.py` net opt-in link wall and socket wall |
| Gate: `local_ci.sh` green on every step | `--task` on each PR, `--batch` on steps 3 and 6 and at the milestone end (step 7) |
