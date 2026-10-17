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

`kit::road_debug_lines` returns every lane as lines, connectors coloured by their light, for an overlay.

## 29.5 Mistakes the cook catches

The cook stops, naming the file, line and column, on: an unknown node or road name, a road from a node to
itself, two nodes or roads with one name, `lanes` outside 0-8 (or both 0), more than 16 roads at a node, a
road shorter than its two junctions need, a `signal` node without a signal (or a signal on a node with another
control), a green entry that matches no turn, and a turn that is green in no phase.
