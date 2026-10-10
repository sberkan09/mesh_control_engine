# ADR-0035: Optional networking module (`mce_net`)

**Status:** Accepted (2026-10-10). **Milestone:** M-NET (off-tier, opt-in). **Requirements:** FR-NET-010..050;
reopens N1 and the networking part of N7 (REQUIREMENTS §5) by the user's decision of 2026-10-10.
**Reference:** `docs/engine/net.md`, user guide chapter 20.

**Context.** REQUIREMENTS ruled networking out (N1 "multiplayer/netcode of any kind", N7 "networking ...") because
the anchor games are single-player. On 2026-10-10 the user asked for multiplayer support that a game developer can
add when they want it, without it being forced on games that do not. So the requirement is an *opt-in module*:
the engine library, the tools and every game that does not ask for networking must stay exactly as they are.

## Decisions

1. **Client-server, server-authoritative, not deterministic lockstep.** Lockstep needs bit-identical simulation
   on every peer. MCE's determinism is per platform (ADR-0007: Linux, Windows and macOS hash differently, and Jolt
   is only cross-platform deterministic under its own flag), so a Windows player and a Mac player would desync.
   With an authoritative server, clients only mirror what the server says, so they can run any platform and
   any frame rate, and a misbehaving client cannot change the shared state except through the messages the game
   accepts. The server's own simulation stays deterministic and replayable as before; the network is input to it,
   applied at a fixed point of the tick (FR-RUN-040 holds on the server).
2. **ENet for the transport.** UDP with per-channel reliable-ordered, unreliable-sequenced and unreliable
   delivery, fragmentation, RTT estimates and timeouts: what a game needs and nothing else. MIT licensed, about
   6.5 k lines of C, no dependencies beyond the OS socket API (Winsock on Windows), and builds unchanged on Linux,
   Windows and macOS. Rejected: raw sockets (reliability, fragmentation and timeouts would be ours to write and
   debug), GameNetworkingSockets and yojimbo (larger, more dependencies, protobuf/libsodium for features a small
   game does not need), TCP (head-of-line blocking stalls every snapshot behind one lost packet).
   Pinned `v1.3.18` in `cmake/Dependencies.cmake`; its sources compile into `mce_net` itself (an object library
   with warnings off, like Jolt and meshoptimizer), so the module is one archive in the tree and in the SDK.
3. **Opt-in at link level.** `mce_net` is **not** in `MCE_ENGINE_LIBS`: `libmce_engine` contains no socket code.
   It is a static `UMBRELLA` library (cmake/MceTarget.cmake): where game modules are supported its engine calls
   resolve against `libmce_engine`, so linking it into a game module does not create a second engine. A project
   asks for it with `mce_add_project(... DEPS mce::mce_net)`; the SDK installs `libmce_net.a` and an imported
   `mce::mce_net` target. Built when `MCE_WITH_NET` is ON (the default); OFF removes the module, its tests and the
   sample. Walls (`scripts/ci/check_walls.py`): ENet headers and symbols only in `src/engine/net/src/`; `mce_net`
   only linked from games, samples, projects and tests, and never listed in `MCE_ENGINE_LIBS`.
4. **Three layers, each usable alone.** `Host` (one endpoint: server via `listen`, client via
   `open_client` + `connect`; `poll` returns Connected / Disconnected / Received events; no ENet type in the
   header). `ByteWriter` / `ByteReader` (explicit little-endian, LEB128 varints, a reader that fails closed:
   any out-of-range read sets `ok() == false` and returns zeros). `ReplicationServer` / `ReplicationClient`
   (full snapshots of every entity tagged `NetId`, its `LocalTransform` plus optional game bytes from hooks). A
   game can use only `Host` and its own messages, or add replication on top.
5. **Full snapshots on a sequenced channel, applied through `Commands`.** No deltas and no acks: a lost
   snapshot is superseded by the next one, and a stale one is dropped by ENet's sequencing and again by the
   snapshot tick check. Sizes are fine for the scale MCE targets (≈ 50 bytes per entity; a few hundred entities
   at 20–60 Hz). The client validates the whole snapshot before queuing any command, so a truncated or hostile
   packet changes nothing; it mirrors through `Commands`, so `apply()` works inside a FixedUpdate_Pre system and
   lands at that phase's sync point like any other structural change.
6. **Single-threaded, polled.** `Host::poll` runs where the game calls it (a system or the host loop); there is
   no network thread, so nothing touches the World concurrently and there are no locks in gameplay code.

## Consequences

- Games that do not link `mce_net` are unchanged: same `libmce_engine`, same state hashes, same ship zip.
- What the module does not do yet (each a later, separate decision): client-side prediction and
  reconciliation, interpolation buffers, delta compression and interest management, encryption and
  authentication, NAT traversal, lobbies/matchmaking, and the editor (play-in-editor runs one world; testing a
  multiplayer game means running a server and a client host). Accounts, telemetry, achievements, cloud saves and
  DRM stay out under N7.
- Tests: `test_net` (codec, a loopback client-server session over real UDP sockets, replication between two
  worlds including malformed and stale snapshots) and `sample_net_headless_300` (server and client in one
  process; the client's mirror must equal the server's replicated state bit for bit). The SDK contract test
  builds a template project with `DEPS mce::mce_net` against the installed SDK and runs it.
