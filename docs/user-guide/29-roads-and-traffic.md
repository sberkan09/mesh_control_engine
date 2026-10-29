# 29. Roads and traffic

An open-world game needs streets that cars and people can use without hand-placed waypoints. In MCE a
city's streets are one asset, a **road network** (`.mcroads`): you list the junctions and the roads between
them, and the cook works out every lane, every turn through every junction, who gives way to whom and when
each traffic light is green. Traffic and crowds then drive and walk on the cooked lanes.

The design and its trade-offs are in [ADR-0046](../adr/0046-road-network.md); the plan is
[M-OPENWORLD](../milestones/M-OPENWORLD.md) track B.

## 29.1 A road network

Create one over MCP with `asset_create` (kind `RoadNetwork`, a path ending in `.mcroads`) or write the file
under `assets/`:

```json
{ "format": "mce-roads", "version": 1, "guid": "<uuid>", "name": "downtown",
  "drive": "right", "lane_width": 3.5, "speed": 13.9,
  "nodes": [
    {"name": "c", "at": [0, 0, 0], "control": "signal"},
    {"name": "n", "at": [0, 0, -120]}, {"name": "s", "at": [0, 0, 120]},
    {"name": "e", "at": [120, 0, 0]},  {"name": "w", "at": [-120, 0, 0]}
  ],
  "roads": [
    {"name": "north", "from": "c", "to": "n", "lanes": 1},
    {"name": "south", "from": "c", "to": "s", "lanes": 1},
    {"name": "east",  "from": "c", "to": "e", "lanes": [2, 2], "speed": 16, "sidewalk": 2.5},
    {"name": "west",  "from": "c", "to": "w", "lanes": [2, 2], "speed": 16, "points": [[-60, 0, 8]]}
  ],
  "signals": [
    {"node": "c", "amber": 3, "all_red": 1,
     "phases": [{"green": ["east", "west"], "seconds": 25}, {"green": ["north", "south"], "seconds": 15}]}
  ]
}
```

- **Nodes** are junctions and road ends: a unique `name` and a position `at` (metres, Y up).
- **Roads** join two different nodes. `lanes` is `[forward, backward]` (forward runs `from` to `to`) or one
  number for that many each way; `[2, 0]` is a two-lane one-way street. `points` bends the road between its
  nodes. `speed` (m/s), `lane_width` (m) and `sidewalk` (m beside each kerb, used by crowds) default to the
  file's values. `priority` (0-15) decides right of way at priority junctions.
- **`drive`** is the side traffic keeps to: `"right"` (default) or `"left"`. Lane 0 is the innermost lane,
  beside the centre line (or beside the other direction's lanes).
- Positions are relative to the entity that places the network, so the network moves with it.

## 29.2 What the cook builds

For each road it builds one lane per direction and lane count, offset to the drive side and trimmed back
from both junctions by the junction's **radius** (from the widths and angles of the roads meeting there; set
a node's `radius` to override it). Across each junction it builds **connectors**, curved lanes from the end
of a lane to the start of another:

- straight on, each lane continues in the lane at the same place from the kerb (two lanes into one merge,
  one lane into two forks);
- the turn towards the kerb (right when driving right) leaves from the outermost lane, the turn across
  traffic from the innermost lane;
- a lane that would have no turn takes its neighbour's turns;
- a two-way road's dead end gets a U-turn;
- `"no_turns": [{"node": "c", "from": "south", "to": "west"}]` leaves a turn out.

A connector's speed is capped by its tightest bend, so turning cars slow down.

## 29.3 Right of way and traffic lights

Connectors through one junction that cross or merge **conflict**; a driver gives way on them. Who goes first
comes from the node's `control`:

| control | Default where | Who goes |
|---|---|---|
| `none` | one or two roads (bends, dead ends) | nobody yields; merging lanes take turns |
| `priority` | three or more roads | the roads with the highest `priority` go; the others yield. With equal priorities nobody yields and straight on goes before turns |
| `stop` | set it | every approach stops at the line, then yields |
| `signal` | a `signals` entry | the lights; a turn across traffic yields to oncoming traffic on a shared green |

A signal is a cycle of `phases`. Each phase is green for `seconds`, then amber for `amber` seconds, then red
everywhere for `all_red` seconds before the next phase. A `green` entry is a road (every turn from it) or one
turn written `"from>to"` (a protected left, say). `offset` shifts the cycle so neighbouring lights make a green
wave. Every turn at a signalled junction must be green in some phase, or the cook stops.

## 29.4 Placing a network and asking it questions

Put a `RoadNetwork3D` component on an entity and set its `roads` to the `.mcroads` asset. The network sits at
the entity's position (its rotation and scale are ignored). `install_stock` installs the road kit; it runs the
traffic lights on the component's `time`, which saves and replays with the scene. Clear `lights` to freeze them.

Game code reaches the lanes through `mce/kit/roads.h`:

```cpp
kit::RoadNet net;
if (kit::road_net_first(world, net)) {                 // or kit::road_net(world, entity, net)
    kit::RoadNearestQuery q;
    q.heading = car_forward;                           // only lanes going this way
    kit::RoadSpot spot;
    if (kit::road_nearest(net, car_position, spot, q)) {
        math::vec3 dir;
        const math::vec3 ahead = kit::road_lane_point(net, spot.lane, spot.s + 10.0f, &dir);
        f32 left = 0.0f;
        const kit::RoadLight light = kit::road_light(net, next_connector, &left);  // Green, Amber or Red
    }
    Array<u32> route;
    kit::road_route(*net.def, from_lane, to_lane, route);  // the quickest lanes, junctions included
}
```

`kit::road_debug_lines` returns every lane as lines, connectors coloured by their light, for an overlay. The
editor draws them while a 3D game plays (View > Roads).

Over MCP, `roads_generate` writes a street grid to start from (junctions `n<i>_<j>`, roads `x<i>_<j>` along +x and
`z<i>_<j>` along +z, signals with the x roads green first), and `roads_info` cooks a network and reports what the
cook built and what traffic would trip on: one-way lanes that end with nowhere to go (`no_way_on`) and lanes cut off
from the rest of the network (`cut_off`).

## 29.5 Traffic

A car joins traffic with a `TrafficDriver3D` next to its `Vehicle3D` (and `RigidBody3D`). Every fixed tick
`"traffic_drivers"` (ahead of `"kit3d_vehicles"`) writes its throttle, steer and brake:

- **Its lane.** The car is projected onto its lane, and onto the next one once it passes the end. A car more
  than `lost_distance` off its lanes re-snaps to the nearest lane running its way (`state` Lost until it does).
- **Its turns.** At the end of a road lane it takes one of the connectors leaving it, picked by a hash of
  `seed` and the junctions it has passed, so a replay turns the same way. U-turns only when nothing else goes.
- **Its speed.** The intelligent driver model (`accel`, `decel`, `headway`, `min_gap`) against the nearest
  thing ahead within its next two lanes: the car in front (`leader`, state Following), a stop line, or a bend
  (the connector's speed, braked to in time). Lane limits are scaled by `speed_factor` and capped by `max_speed`.
- **Stop lines.** It does not enter a connector on red, or on amber while it can still stop (state Waiting);
  at a stop sign until it has stood at the line for a second; or while a conflicting connector is occupied,
  or a car that goes first (higher rank, or the same rank and sooner) would get there within `yield_time`
  seconds of it (state Yielding).
- **Pedestrians.** A `Pedestrian3D` crossing a road (`mode` Cross, chapter 29.6) within a car's lanes ahead
  stops it 3 m short until the pedestrian is across (state Yielding). Signalled crossings wait for the lights,
  so in practice this is the unsignalled junctions.
- **Its wheel.** Pure pursuit at a point `look_ahead` metres (at least 0.6 s) down its lanes.

`TrafficSpawner3D` keeps `max_cars` alive between `min_radius` and `spawn_radius` of `focus` (the player),
on road lanes, `spacing` metres apart, at most `per_tick` a tick, and removes its cars beyond `despawn_radius`
or lost or flipped for `stuck_after` seconds. A car is the `prefab` (its root needs `Vehicle3D`), or, with no
prefab, whatever the game's `TrafficSpawnHook` resource spawns:

```cpp
world.set_resource(kit::TrafficSpawnHook{[](Frame& f, math::vec3 at, math::quat rot, u32 index) {
    return f.commands().spawn(LocalTransform{at, rot, {1, 1, 1}}, body_for(index), car_for(index));
}});
```

A driver's state lives in its component, so traffic replays to the same `state_hash` and saves.

## 29.6 Pedestrian crowds

Roads with a `sidewalk` have pavements, and a crowd walks them. Add a **`CrowdSpawner3D`** to any entity (a
`Crowd` entity beside the network is the usual place); it keeps up to `max_count` **`Pedestrian3D`** walking the
pavements of a `RoadNetwork3D` around a focus:

| Field | Meaning |
|---|---|
| `network` | the entity with the `RoadNetwork3D`; empty: the first one |
| `focus` | the entity the crowd gathers around (your player); empty: the first camera. With neither the crowd fills the whole network and never goes away |
| `prefab` | spawned per pedestrian; it needs a `LocalTransform` and a `Pedestrian3D`. Empty: a bare entity (handy for tests and for a stand-in capsule you add yourself) |
| `max_count`, `per_tick` | how many walkers at once; how many spawn attempts a tick makes |
| `spawn_min`, `spawn_max` | nobody spawns closer to the focus than `spawn_min` (no pop-in under the player's nose) or farther than `spawn_max` |
| `despawn_radius` | walkers farther than this from the focus are removed; keep it above `spawn_max` |
| `near_radius`, `far_radius` | level of detail (below) |
| `speed_min`, `speed_max` | walking speed in m/s, picked per pedestrian |
| `cross` | 0-1: how willing they are to cross a road instead of going round the corner |
| `seed` | the random stream; the same seed replays the same crowd |

A pedestrian has no behaviour tree and needs no physics body: it is a few numbers (which pavement, how far along, which
way) that one system advances, so hundreds cost less than a handful of AI characters. It writes the entity's
`LocalTransform` (on the network's plane, facing the way it walks) and two fields an animation graph can read:
`pace` (m/s it moves at now, 0 while waiting) and `mode` (`Walk`, `Wait` at a kerb, `Cross`). Give the prefab a
walk/idle graph keyed on `pace`.

**A body for hits.** The prefab may add a kinematic `RigidBody3D` (a capsule, sphere or box) so that cars collide with
walkers and shots hit them. The crowd then stands the walker on it: the entity's origin is the body's centre (a capsule's
`radius + half_height` above the pavement), so hang the model that far below (a 1.8 m capsule, model at y = -0.9). The
crowd drives the body by velocity, aimed at each step's pose so it arrives by the walker's next step: contacts see a real
walking speed, and mid and far walkers, which step every 4th or 16th tick, glide between steps instead of jumping. A
walker turns round when a dynamic body (a parked car, a crate) stands where it is going, instead of shoving it aside; one
that starts inside such a body walks out. Dead walkers' bodies stop, despawned walkers' bodies go, and a walker moved from
outside (a script, the editor, a loaded save) is put back on its pavement, body and all.

**At junctions** a pedestrian goes round the corner, across a road or turns back, chosen by its own random stream. A
crossing waits at the kerb until every road it crosses has no straight-on traffic with a green or amber light and
enough red left to get across; an unsignalled junction never makes anyone wait (traffic gives way to them, B2).

**Level of detail.** A pedestrian within `near_radius` of the focus steps every tick, within `far_radius` every 4th,
beyond that every 16th, with the time that has passed, so far walkers are cheap and still arrive where they are going.
`kit::crowd_stats(world)` returns last tick's counters (pedestrians, near/mid/far, waiting, crossing, stepped,
spawned, despawned) for budgets.

A hand-placed `Pedestrian3D` (no `crowd`) snaps to the nearest pavement and walks it with the defaults. The crowd is
deterministic: positions, directions and the spawner's `serial` are saved, so a replay or a loaded save continues
exactly.

Not yet: pedestrians do not avoid each other, react to the player, or follow terrain height (flat networks), and cars
do not yet slow for them (traffic, B2).

## 29.7 A whole city: samples/09_openworld

`samples/09_openworld` puts a road network, traffic, a crowd, getting into cars, a gun and the police together in one
scene (`assets/scenes/city.mcscene`, written by `gen_assets.py`). Its pieces, as a recipe for your own city:

- **The network** is `assets/roads/city.mcroads`: a 5 × 5 grid of junctions 250 m apart (1 km²), the middle row and column
  are avenues (two lanes each way, faster, higher `priority`), the nine inner junctions are `signal` with the avenues'
  green longer and the lights offset for a wave. One `RoadNetwork3D` entity places it at the origin. The road surface you
  see is ordinary tinted boxes: the network is data for cars and walkers, it draws nothing.
- **Traffic** is one `TrafficSpawner3D` (`focus` = the player, a car prefab with `Vehicle3D` + `TrafficDriver3D`, 24 cars,
  45-170 m from the player). **The crowd** is one `CrowdSpawner3D` (70 walkers, a prefab with `Pedestrian3D`, a kinematic
  capsule and `Health3D` team 3, so the police can see a shot walker as a crime).
- **The player** is a `CharacterController3D` with `Rider3D` (E gets in the parked car and out again), `Health3D` team 1 and
  `Wanted3D`. The camera is an orbit `CameraFollow3D` that also carries the `Weapon3D` (owner = the player): shots leave
  along the view, and the camera follows the car while the player sits in it. It starts at `yaw` π/2, looking west along
  the avenue: pick the start yaw so the first frame shows the road. At yaw 0 this camera hung over the avenue looking into
  a block, and the traffic and crowd, which spawn 45 m and 25 m away, were never in frame.
- **Police** are four `Police3D` characters with a `NavAgent3D` and a `Weapon3D` on the plaza, on a `NavMeshSurface3D`
  baked at start-up around the middle blocks (cell 0.5 m). They stay idle until a witnessed crime gives the player a
  wanted level; then they pursue, and from level 2 shoot.
- `drive.input` is the headless script (walk to the car, get in, drive, brake with `TriggerL`, get out); the input map
  `assets/input/openworld.mcinput` adds `Interact`, `Reload`, `Sprint` and `TriggerL` to the stock actions.

The art is the ledger's content set: `kenney-car-kit` (sedan, sports sedan, taxi, SUV, delivery van and police car for the
parked cars; the traffic prefab uses the sedan), `kenney-blocky-characters` (player, police, crowd) and
`kenney-city-kit-commercial` / `-suburban` (the lot buildings, stretched to each lot's box). `mce content add <pack>
samples/09_openworld --only <glob>` copied only the files in use, so the sample stays small; `gen_assets.py` writes their
`.mceasset` sidecars and the scene points at them by GUID. The collision boxes are still plain boxes: the models are
visual only, and the car wheels do not spin and the characters do not animate yet.

Not in the sample yet: missions and the minimap ([M-MISSION](../milestones/M-MISSION.md)), baked HLOD for the skyline
(the render-scale thread), traffic-light visuals that follow the light state, police cars, a skinned or animated character and audio
(engine, gun and siren sounds still need ledger entries). Dead pedestrians vanish after a fraction of a second: the crowd does not look at `Health3D` yet.

## 29.8 Mistakes the cook catches

The cook stops, naming the file, line and column, on: an unknown node or road name, a road from a node to
itself, two nodes or roads with one name, `lanes` outside 0-8 (or both 0), more than 16 roads at a node, a
road shorter than its two junctions need, a `signal` node without a signal (or a signal on a node with another
control), a green entry that matches no turn, and a turn that is green in no phase.
