# ADR-0046 — Road networks as data: .mcroads, cooked lanes and connectors, traffic on top

**Status:** Accepted (2026-10-17). **Milestone:** M-OPENWORLD track B (B1 road graph, B2 traffic, B3 crowds).
**Plan:** `docs/milestones/M-OPENWORLD.md`. **Reference:** `src/engine/asset/include/mce/asset/roads_format.h`
(CHNK_ROADS), `src/engine/asset/cook/src/roads_importer.cpp`, `src/engine/kit/include/mce/kit/roads.h` (B1b),
`src/engine/kit/include/mce/kit/traffic.h` (B2).

**Context.** A GTA-style city needs cars that keep to their lane, turn at junctions, stop at red lights and give
way, and pedestrians that walk the pavements and cross at junctions. The engine has a car (`Vehicle3D`) and a
waypoint driver that laps a chain of `Waypoint3D` entities, which is enough for a race but not for a street
grid: a city of a few hundred junctions would need thousands of hand-placed waypoints, nothing says who has right
of way, and nothing describes the pavements. Unreal leaves this to the Zone Graph and Mass plugins, Unity to
third-party assets, Godot to the game. MCE's edge is data an agent can author over MCP, a cook that checks it,
and deterministic simulation, so the road layer is a cooked asset whose runtime is a few queries and components.

**Options weighed.**

| Option | For | Against |
|---|---|---|
| **Junctions and roads as authored data; the cook derives every lane, connector, conflict and signal bit** | A city is a few hundred nodes and roads an agent can write; the cook decides the hard geometry once and checks it; runtime is plain array walks | The cook's turn rules must be good enough that authors rarely override them |
| Hand-authored lane graph (every lane and connector a record) | Full control | Ten times the data, easy to leave a lane dangling, no agent writes it correctly |
| Waypoint entities (extend `Waypoint3D`) | Exists today | Thousands of entities, no notion of lanes, junctions or yielding, scene files balloon |
| Derive roads from the navmesh | No new asset | Navmeshes have no direction, lanes or junction rules |

## Decisions

1. **A `.mcroads` is JSON the cook turns into `CHNK_ROADS`.** The source has `nodes` (junctions and road ends,
   each a name and a position, optional `control` and `radius`), `roads` (a name, `from` and `to` nodes, optional
   shape `points`, `lanes` as `[forward, backward]`, `speed`, `lane_width`, `sidewalk`, `priority`), `signals`
   and `no_turns`. `drive` picks the side traffic keeps to. The chunk is flat tables (nodes, roads, lanes, points,
   conflicts, signals, phases) in its own header, `roads_format.h`, not in `formats.h`.
2. **Everything a car drives on is a lane.** A road lane runs one way along one road, trimmed back from the
   junction at each end by the junction radius (from the roads' widths and the angles between them). A connector
   is a lane across one junction, a cubic curve from the end of one road lane to the start of another, sampled at
   8 segments. A road lane's successors are the connectors leaving its end, a contiguous run; a connector has
   exactly one successor. Followers, route planners and occupancy lists treat the two the same.
3. **The cook chooses the turns.** Straight on maps lanes from the kerb side (two lanes into one merge, one into
   two forks), the turn towards the kerb leaves from the outermost lane, the turn across traffic from the
   innermost, and a lane with no turn borrows its neighbour's. A two-way dead end gets a U-turn. `no_turns`
   removes a turn. Turns are classed by the angle between the lanes (within 35 degrees is straight).
4. **Right of way is cooked, not computed.** Connectors of one junction that cross or merge into one lane
   conflict (forks from one lane do not). Each connector gets a `rank` (straight 3, towards the kerb 2, across 1,
   U-turn 0; plus 4 for the priority road at a priority junction) and the Yield or Stop flag from its junction's
   control. Junctions with three or more roads default to `priority`; with all roads at equal priority nobody
   carries Yield and rank alone decides. A runtime driver yields to a conflicting connector that is occupied or
   that a car of higher rank is about to enter.
5. **Signals are a cycle of phases, a pure function of time.** A phase lists roads or single `from>to` turns that
   are green; amber and all-red follow every phase. The cook stores each connector's green as a bit per phase and
   rejects a signal that leaves a turn red forever. Runtime light state is `(time + offset) mod cycle`, with the
   time held in a saved component, so lights replay and save exactly.
6. **The network lives in its own space.** Positions are metres relative to the entity whose `RoadNetwork3D`
   places it (translation only), so a floating-origin rebase (M-OPENWORLD A2) moves one entity.
7. **Limits**: 8 lanes each way, 16 roads per node, 65535 nodes, 16 phases per signal; the reader checks every
   index, range, number and the lane graph's shape, so a bad payload never reaches a driver.

**Not now.** Roundabouts as a control (build them from priority junctions), lane changes between parallel lanes
on one road (B2 keeps a car in its lane until a junction), on-street parking, road meshes generated from the
network (the render-scale track owns city geometry), pedestrian crossings as data (B3 derives them from the
sidewalk widths at junctions).
