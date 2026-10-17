# ADR-0048 — Pedestrian crowds as data on the road network's pavements

**Status:** Accepted (2026-10-17). **Milestone:** M-OPENWORLD track B, step B3. **Builds on:** ADR-0046 (road networks).
**Reference:** `src/engine/kit/include/mce/kit/crowd.h`, `src/engine/kit/src/crowd.cpp`, `tests/kit/test_crowd.cpp`.

**Context.** A city street needs people on its pavements: hundreds around the player, who cross at junctions when the
lights let them, and who cost little when they are far away or out of sight. The engine's character movers
(`CharacterController3D`, navmesh agents, behaviour trees) are built for a few characters that think; a crowd needs
thousands of walkers that only follow a path.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **A pedestrian is a few numbers walking a pavement edge graph derived from the `.mcroads`; one system steps all of them** | No new asset (`sidewalk` is already cooked); no tree or physics per walker; deterministic and tiny to save | Walkers do not avoid each other or react to the world |
| Navmesh agents (`nav3d`) per pedestrian | Obstacle avoidance for free | Needs a baked navmesh of the city, a path query per walker, far too heavy for hundreds |
| Behaviour trees per pedestrian (`AiBrain`) | Rich behaviour | A tree per walker; the cost is the thing a crowd must avoid |
| Mass-style ECS fragments with separate processors | Scales furthest | A new execution model; this engine has one system per concern |

## Decisions

1. **Pavement edges come from the road data.** For each road with `sidewalk > 0`, one edge beside each kerb: the untrimmed
   centreline offset by the carriageway's half width on that side (from `forward`, `backward`, `lane_width` and the drive
   side) plus half the sidewalk, mitred at bends, trimmed back by the junction radius (read from where the cooked lanes
   stop). Built once per network (rebuilt when its hash changes) and kept in a resource, not in the state hash.
2. **Junctions are hops.** At the end of an edge a pedestrian picks, by its own random stream, another edge end at the
   same node (round the corner), an end on the far side of a road (a crossing) or its own end (turn round). A hop is a
   crossing when its straight line, sampled at nine points, lies inside the carriageway strip of a road meeting the node;
   the roads crossed (up to three) are stored with the hop. `cross` weights crossing hops against corner hops.
3. **Lights decide crossings, nothing else does.** A crossing starts when, for every road it crosses, every straight-on
   connector entering from or leaving onto that road is red with more time left than the crossing needs (plus a
   second). Turning traffic is not considered: it yields to pedestrians (B2's job). At an unsignalled node nobody waits.
4. **No behaviour tree, no physics.** The system writes `LocalTransform` flat on the pavement (the network's height) and
   a turn-rate-limited yaw. `pace` and `mode` (Walk, Wait, Cross) are readable by an animation graph.
5. **Level of detail is stepping frequency.** Within `near_radius` of the focus a pedestrian steps every tick, within
   `far_radius` every 4th, beyond every 16th, with the time that passed (`lag`), staggered by `entity index + tick`.
   Beyond `despawn_radius` it is removed. `CrowdStats` (near/mid/far, waiting, crossing, stepped, spawned) is the budget
   counter the A1 perf runs read.
6. **Spawning is a function of saved state.** Candidate edges depend only on the focus's 32 m cell and the network;
   each attempt hashes `(seed, spawner, serial)` and `serial` is saved on the spawner, so a replay or a resumed save
   spawns the same pedestrians. With no focus the crowd fills the network and never despawns.
7. **Prefab or bare.** `prefab` is instantiated with overrides for the pedestrian's state and transform (it needs a
   `LocalTransform` and a `Pedestrian3D`); with none, a bare entity with just those two.

**Not now.** Avoiding other pedestrians, cars hitting or yielding to pedestrians (B2 needs a crowd query; C3 reacts to
violence), pedestrians reacting to the player or gunfire, terrain-following height (flat networks only), zebra crossing
as data, sitting/idling, indoor routes, per-pedestrian animation selection (the prefab's graph reads `pace`).
