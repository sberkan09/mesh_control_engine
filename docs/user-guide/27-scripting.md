# 27. Scripting in Luau

Gameplay rules can be written in Luau, a sandboxed Lua dialect, instead of C++. A script is an asset:
the cook compiles it to bytecode, a scene names it, and the stock player runs it with no game module
and no C++ compiler. The track is being built (ROADMAP M-SCRIPT, ADR-0041); this chapter describes what
works today and grows with it.

Scripting needs a build with `MCE_WITH_SCRIPT` (on in the desktop, phone and web presets).

## 27.1 A script

Put a `.luau` file anywhere under `assets/`. Give it a sidecar (`importer: "script"`) the way any asset gets one: the
editor and `mce build` / `mce play` do it for you, and the bare cook does it with `--adopt` (section 5.4; without it a
file with no sidecar is a cook error). The cook compiles it into a `Script` asset (a `CHNK_SCRIPT` chunk of Luau bytecode).
A syntax error fails the cook with the file, line and column, like any asset error:

```
assets/scripts/push.luau:2:13: error: Expected identifier when parsing expression, got '=' [cook.script]
```

A script must define `update()`, which runs once per fixed tick:

```lua
-- assets/scripts/push.luau: moves every LocalTransform one unit along x per tick
function update()
    mce.each("LocalTransform", function(e)
        local p = mce.get(e, "LocalTransform", "position")
        mce.set(e, "LocalTransform", "position", p + vector.create(1, 0, 0))
    end)
end
```

## 27.2 Running it from a scene

A `ScriptSystem` component names one script. Put it on its own entity (by convention named
`Script: <name>`, next to the World Settings):

```json
{ "name": "Script: push", "components": { "ScriptSystem": { "script": "guid:<the script's GUID>", "phase": "Pre", "order": 0 } } }
```

| Field | Default | Meaning |
|---|---|---|
| `script` | — | the `Script` asset whose `update()` runs |
| `phase` | `Pre` | `Pre`: before physics (FixedUpdate_Pre); `Post`: after it (FixedUpdate_Post). A script that calls `mce.move_and_collide` or sets 2D velocities needs `Post`: the bodies are made by the first physics step |
| `order` | 0 | scripts of a phase run lowest first; ties run in ECS order |
| `enabled` | true | off: the script is not loaded |
| `during_cutscenes` | false | on: the script keeps running while a cutscene holds the game (a pause hold still stops it). Scripts with it on run after the others of their phase, whatever their `order`; scripts without it are held during a cutscene, as before |

`mce_player`, the editor's play mode and MCP's `run_headless` run a scene's scripts the same way. The
set is read again whenever it changes (a scene change, an edit), and the new set loads from the
mounted assets between two ticks.

## 27.3 The `mce` API

Components and events are named by their reflected type name (`"LocalTransform"`, `"TriggerEnter"`); a
name the engine does not know, or a field a type does not have, is an error that names it. Entities are
numbers, and `nil` is the null entity.

**Fields**

| Call | Does |
|---|---|
| `mce.field("Comp", "field", element?)` | a handle to one field, looked up once; call it at the script's top level. For an array field such as `ScriptVars.v`, `element` (1-based) makes a handle to that one slot, so `mce.get` returns that element's value (a number for `ScriptVars`) |
| `mce.get(e, h, ...)` | the values of one or more fields (handles, or `"Comp", "field"` pairs) in one call |
| `mce.set(e, h, v, ...)` | writes one or more fields (`h, v, h2, v2, ...`) and marks the components changed |
| `mce.each("A", "B", ..., fn)` | calls `fn(e)` for every entity with all the components, in the order a Query visits |

One call may mix components: `mce.get(e, "LocalTransform", "position", "ScriptVars", "v")` returns both values and
`mce.set(e, "LocalTransform", "position", p, "ScriptVars", "v", t)` writes both. A script cannot keep a variable between
ticks; the place for a value that must survive is a component field, and for a game with no C++ of its own
that is `ScriptVars` (27.8).

Field values: numbers, booleans, enums by name (`"Dynamic"`), `Vec2`/`Vec3` as vectors, `Vec4`, `Quat` and
`Color` as four numbers (or a `{x, y, z, w}` table inside a struct), strings, entities, asset references as
`"guid:<uuid>"` and nested structs and arrays as tables. Handles are the fast path: the bench's 2,000
entities read three fields and write two per tick in about 0.35 ms (Release).

```lua
local POS = mce.field("LocalTransform", "position")
local VEL = mce.field("Mover", "velocity")

function update()
    mce.each("LocalTransform", "Mover", function(e)
        local p, v = mce.get(e, POS, VEL)
        mce.set(e, POS, p + v * mce.dt)
    end)
end
```

**Entities.** Structural changes are recorded and happen at the next sync point, like `Commands` in C++:
the new entity is alive at once, its components appear after the phase.

| Call | Does |
|---|---|
| `mce.spawn({Comp = {...}, Tag = true})` | a new entity; each component starts from its defaults with the named fields set |
| `mce.add(e, "Comp", {...})` | adds the component (or updates it, keeping the fields not named) |
| `mce.remove(e, "Comp")`, `mce.destroy(e)` | removes a component, destroys the entity |
| `mce.has(e, "Comp")`, `mce.alive(e)` | what the entity has now |
| `mce.find(name)` | the entity a scene gave that name (`"name"` in the `.mcscene`), or `nil`; the first live one in scene order if several share it. Entities spawned at run time have no name, and a scene change forgets the old names |
| `mce.param(e, name, default?)` | the number the scene gave this entity for `name` in its `ScriptParams` (chapter 27.8), or `default` when it has none; without a default a missing component or name is an error |
| `mce.instantiate("guid:<prefab>", pos?)` | spawns a prefab, its root at `pos`; the root entity, or `nil` (logged) |

**Events and signals.** Events are readable from the tick after they were pushed.

| Call | Does |
|---|---|
| `mce.events("TriggerEnter")` | this tick's events as an array of tables: `TriggerEnter`/`TriggerExit` (`sensor`, `other`), `ContactBegin2D` (`a`, `b`, `point`, `normal`, `approach_speed`), `ContactEnd2D`, `TriggerEnter3D`/`TriggerExit3D` (`trigger`, `other`), `JointBreak3D` |
| `mce.emit("Type", {...})` | pushes an event of any reflected event type the world has |
| `mce.signal("name", e?)` | a named signal that sequences' and behavior trees' wait nodes hear |
| `mce.signals("name")` | the emitters of this tick's signals of that name (sequences, timelines, behavior trees) |
| `mce.make_noise(position, loudness?, source?, team?)` | a noise perceivers hear on the next tick (`loudness` metres for hearing 1, default 10; perceivers of `team` ignore it); needs the AI kit's perception |
| `mce.play_sound("guid:<sfx>", gain_db?)` | a one-shot sound |

**Input.** `mce.pressed(action)`, `mce.held(action)`, `mce.released(action)`, `mce.axis(action)` (a
number) and `mce.axis2(action)` (a vector) read the input map's actions by name (chapter 11). An action the
map does not define reads as not pressed and zero.

**Physics.**

| Call | Does |
|---|---|
| `mce.raycast2d(origin, dir, max, skip?)` | the first hit, `{entity, point, normal, distance}`, or `nil`; `skip` is an entity to ignore |
| `mce.overlap2d(min, max, skip?)` | the entities whose shapes' boxes overlap the box |
| `mce.near(center, radius, component?, skip?)` | the entities with a `LocalTransform` within `radius` of `center` (and with `component`, if given), nearest first; no physics world needed, so a pickup, an aggro range or a touch test is one call (a 2D game has z = 0) |
| `mce.within(min, max, component?, skip?)` | the entities with a `LocalTransform` inside the box from `min` to `max` (edges included; a 2D game has z = 0, so give `min.z <= 0 <= max.z`), optionally with `component`, in storage order; no physics world needed. `min` above `max` is an error |
| `mce.move_and_collide(e, delta)` | moves a character body; `{position, grounded, ground, ceiling, wall_left, wall_right}` |
| `mce.teleport2d(e, position)` | puts a 2D body at `position` with no sweep (a respawn, a level's start); writing `LocalTransform.position` does not move a body, the physics step writes the body's position back over it |
| `mce.velocity2d(e)`, `mce.set_velocity2d(e, v)`, `mce.impulse2d(e, v, point?)` | a 2D body's velocity, and pushes; with a world-space `point` the impulse spins the body |
| `mce.force2d(e, f)` | a force on the body's origin, kept until the next physics step (call it every tick for a thruster) |
| `mce.angular_velocity2d(e)`, `mce.set_angular_velocity2d(e, w)` | radians per second |
| `mce.awake2d(e)`, `mce.set_awake2d(e, awake)`, `mce.set_enabled2d(e, enabled)` | sleep (needs sleeping on in the physics config) and switching a body off: a disabled body neither moves nor collides |
| `mce.overlap_point2d(p, skip?)` | is there a shape at the point |
| `mce.raycast_all2d(origin, dir, max, skip?)` | every hit along the ray, nearest first, each `{entity, point, normal, distance}` |
| `mce.circle_cast2d(from, to, radius, skip?)`, `mce.box_cast2d(from, to, half, skip?)` | the first thing a circle or box hits on its way, `{entity, point, normal, fraction}` (0 to 1 along the move), or `nil` |
| `mce.find_path(from, to, options?)` | a walk from `from` to `to` around the static colliders: an array of vectors (the last one is `to`), or `nil` when `to` is blocked or unreachable. Options: `cell` (grid cell, metres, default 0.5), `radius` (how wide the walker is, default 0.25), `diagonal` and `smooth` (both default true). The grid is built from the static colliders and rebuilt when they change; with none, the answer is `{to}` |

The 2D body functions raise "the entity has no 2D body" for an entity without one (a body exists from the first physics step).
| `mce.raycast3d(origin, dir, max, skip?)` | the same for the 3D physics world |
| `mce.overlap_sphere3d(center, radius, skip?)`, `mce.overlap_box3d(center, half, skip?)` | the entities whose shapes touch a sphere or an axis-aligned box (at most 256) |
| `mce.sphere_cast3d(from, to, radius, skip?)` | the first thing a sphere hits on its way, `{entity, point, normal, position, fraction}`, or `nil` |
| `mce.velocity3d(e)`, `mce.set_velocity3d(e, v)`, `mce.angular_velocity3d(e)`, `mce.set_angular_velocity3d(e, w)`, `mce.impulse3d(e, impulse)` | a 3D body's velocities and a push at its centre of mass |
| `mce.teleport3d(e, position, rotation?)` | puts a 3D body or character at `position` with no sweep; `rotation` (see below) defaults to the body's own; writing `LocalTransform.position` alone does not move a body |
| `mce.rotation3d(e)` / `mce.set_rotation3d(e, rotation)` | a 3D body's rotation, and turning it in place |
| `mce.look_rotation3d(from, to)` | the rotation that turns forward from `from` toward `to` (roll 0); pure, no entity |

A rotation in Luau is a `vector` of Euler angles in degrees: `x` pitch, `y` yaw, `z` roll, applied yaw, then pitch, then roll.
Forward is -Z and up is +Y, as for the camera, so a yaw of 0 faces -Z, +90 faces -X and a pitch of +90 looks straight up.
There is no quaternion type; straight up or down has no yaw of its own (it reads back as 0 and the turn goes into roll).
| `mce.find_path3d(from, to)` | the corner points along the baked navmesh after leaving `from`, ending at `to` (like `mce.find_path`), and `true` as a second result when the path is partial (the target cannot be reached; it ends at the closest point); `nil` when there is no path; an error when the world has no navmesh |

A walking 3D character needs no call: set `CharacterController3D.move` and `jump` and the kit's own system moves it with the right slope, step and push rules. The 3D body functions raise "the entity has no 3D body" for an entity without one.

**Game plumbing.**

| Call | Does |
|---|---|
| `mce.save(slot)` | writes the entities that have a `SaveKey` (fields marked `Save`) to the slot now; `true`, or `false` and a reason. A slot is a name: 1 to 64 letters, digits, `_` or `-`, never a path |
| `mce.load(slot)` | `true` when the slot exists; the host applies it right after this tick, on top of the scene that is loaded. The tick counter is not restored |
| `mce.has_save(slot)` | does the slot exist |
| `mce.set_time_scale(s)`, `mce.time_scale()` | game speed against the wall clock, 0.1 to 4 (slow motion, hit-stop); it scales the player's clock, never `dt`, so a headless run ignores it |
| `mce.shake(trauma)` | adds trauma (0..1, total capped at 1) to every `CameraShake2D` and `CameraShake3D` |
| `mce.burst(e, count?)` | fires the entity's `ParticleBurst` once, with `count` particles if given |

Stopping or fading a sound has no call of its own: play it from an entity with an `AudioSource` and a `fade_s`, then destroy the entity (or set its `playing` field) and the voice fades out.

**The rest.**

| Call | Does |
|---|---|
| `mce.dt`, `mce.tick()` | the fixed step in seconds and the current tick (from 0) |
| `mce.random()` | a number in [0, 1) that depends only on the seed, the tick, the script and the call |
| `mce.load_scene(target)` | switches scene after this tick (a source path or `"guid:<uuid>"`; section 4.5) |
| `mce.quit()` | ends the run after this tick, like the window's close button |
| `mce.log(msg, level?)` | a line in the engine log, `[<name>] script: <msg>` (the name of the script that wrote it); `level` is `"info"` (default), `"warn"` or `"error"`, and warnings and errors count in a `.mctest`'s `log_warnings` / `log_errors` checks |

**Missions and map markers.** Two more global tables, `mission` and `marker`, are thin over the mission kit (section 9.13) and the map markers (9.15). They keep nothing: the state is the `Mission` and `MapMarker` components and the `MissionSignal` event, so saves, replays and the editor see it. A world without the kit pieces gets an error that says so; a bad argument is a Luau error that names the call, and it disables only that script.

| Call | Does |
|---|---|
| `mission.start(e)` | an idle mission starts on the next tick (sets `Mission.start`); `true`, or `false` when it is already active, complete or failed |
| `mission.fail(e)` | fails an active mission now and sends `MissionFailed` with `reason` 3 (a script; 0 is the time limit, 1 `fail_on_death`, 2 a vanished objective); `true`, or `false` in any other state |
| `mission.status(e)` | `state, objective, elapsed, done`: `"idle"`, `"active"`, `"complete"` or `"failed"`; the active objective entity (`nil` unless active); seconds since it started; how many objectives are done |
| `mission.signal(name, amount?)` | a `MissionSignal` for the active `signal` objectives (`amount` a whole number from 1, default 1; `name` up to 31 characters). The script runs after the kit's mission system in a tick, so the signal counts two ticks later |
| `marker.add(e, {icon?, color?, layer?, label?, always_visible?, follow?})` | puts a `MapMarker` on the entity at the next sync point. `color` is `{r, g, b}`, `{r, g, b, a}` or a vector (`colour` works too); `layer` 0 to 31; `label` up to 31 characters; `follow` an `Objective` or `Mission` entity. Fields left out keep the current marker's, or the defaults; an unknown field is an error |
| `marker.remove(e)` | takes the `MapMarker` off at the next sync point (nothing happens without one) |

**Inventories** (M-INVENTORY 4). The `inventory` global is thin over the kit's `Inventory`, `Equipment` and `Container` (`docs/engine/kit.md`). It keeps no state: changes go in as the kit's input events and land a couple of ticks later, reads look straight at the component. Slots count from 1 (1 to 24). A world without `install_inventory` gets an error that says so.

| Call | What it does |
|---|---|
| `inventory.add(e, id, qty?)` | queues a `GiveItem` (`qty` a whole number from 1 to 1000000, default 1); what does not fit is reported by `ItemAdded.remainder` |
| `inventory.remove(e, id, qty?)` | queues a `TakeItem` |
| `inventory.count(e, id)` | units of the item across all slots; 0 for an id the catalog does not have |
| `inventory.has(e, id, qty?)` | `true` when the entity carries at least `qty` (default 1) |
| `inventory.weight(e)` | carried weight in thousandths of a kilogram |
| `inventory.slot(e, i)` | `id, qty` of slot `i`; `nil, 0` when it is empty |
| `inventory.equip(e, i, where?)` | queues an `EquipItem` from slot `i`; `where` is `"head"`, `"body"`, `"hands"`, `"feet"`, `"weapon"`, `"offhand"`, `"accessory1"` or `"accessory2"`, or left out to take the first the item's `equip.*` tags allow |
| `inventory.unequip(e, where)` | queues an `UnequipItem` |
| `inventory.use(e, i)` | queues a `UseItem`; an item tagged `consumable` loses one unit |
| `inventory.drop(e, i, qty?)` | takes `qty` (default the whole stack) out of slot `i` now and spawns an `ItemPickup3D` 1.5 m along +x of the entity; returns how many were dropped |
| `inventory.open(container, opener)` | queues an `OpenContainer`: the chest rolls its table on the first open and the opener takes what fits |
| `inventory.can_craft(e, recipe, station?)` | `true` when the recipe (an id from the `.mcitems` `recipes`) could be made now: every input present, room for every output, and a `station` entity with the recipe's station tag when it names one; `false` for an unknown recipe |
| `inventory.craft(e, recipe, station?)` | queues a `CraftItem` (`station` nil: by hand). The craft is atomic; `ItemCrafted` or `CraftFailed` (with a reason) follows. A timed recipe at a station completes when its time is up |
| `status.add(e, id, kind, amount, seconds, source?)` | queues a timed effect on an entity with `Health3D` and `StatusEffects`. `kind`: `"regen"` or `"poison"` (`amount` hp per second, positive; poison hits in half-second pulses and credits `source`), `"armor"` or `"max_health"` (`amount` is added while active and given back at the end, ±1e6), `"cooldown"` (changes nothing; items with a `cooldown` stat start `cd:<item>`, so `status.has(e, "cd:potion")` is true while it cools down). `seconds` 0.01 to 3600. Adding an id that is active refreshes it instead of stacking. `StatusApplied`, `StatusEnded` or `StatusRefused` follows |
| `status.has(e, id)` | `true` while the effect is active |
| `status.clear(e, id?)` | queues the end of one effect, or of every effect without an `id` (`StatusEnded` with `expired` 0) |

```lua
-- Start the mission when the player presses Interact, and mark the depot on the map.
function update()
    mce.each("Mission", function(m)
        local state, objective, elapsed = mission.status(m)
        if state == "idle" and mce.pressed("Interact") then
            mission.start(m)
            marker.add(m, { icon = 2, color = { 1, 0.8, 0.1 } })   -- a bare marker on a mission follows its active objective
        elseif state == "active" and elapsed > 120 then
            mission.fail(m)
        end
    end)
end
```

**The world clock.** The `clock` global is thin over the first `WorldClock` (section 9.16) and keeps nothing: the hour, the day, the pause and the speed are the component's fields, so saves, replays and the editor see them. A scene without a `WorldClock` entity gets an error that says so; a bad argument is a Luau error that names the call, and it disables only that script.

| Call | Does |
|---|---|
| `clock.hour()` | the hour of the day, 0 to 24 |
| `clock.day()` | whole days passed since the start |
| `clock.set(hour)` | writes `WorldClock.hour` (0 to 24); the clock re-seats on its next tick and carries on from there. `DuskStarted` / `DawnStarted` do not fire for a jump of more than 6 hours |
| `clock.pause(paused)` | `WorldClock.paused` (a boolean) |
| `clock.speed(scale)` | `WorldClock.time_scale`, clamped to 0 to 100 like the component |
| `clock.paused()` | `true` while the clock stands still |

```lua
-- Skip to the morning when the player presses Interact at the bed.
function update()
    if mce.pressed("Interact") and clock.hour() > 20 then
        clock.set(6)
    end
end
```

**The weather.** The `weather` global is thin over the first `Weather` (section 9.19) and keeps nothing: the target, the transition length and the blend are the component's fields, so saves, replays and the editor see them. A scene without a `Weather` entity gets an error that says so; an unknown kind or a NaN is a Luau error that names the call, and it disables only that script.

| Call | Does |
|---|---|
| `weather.kind()` | the kind in effect: `"clear"`, `"overcast"`, `"rain"`, `"storm"` or `"snow"` |
| `weather.target()` | the kind the weather is moving to |
| `weather.set(kind, seconds?)` | writes `Weather.target`; with `seconds` it also writes `transition_seconds` (clamped to 0 to 3600). The old kind fades out, then the new one fades in |
| `weather.intensity()` | 0 to 1, the blend of the kind in effect |
| `weather.wind()` | 0 to 1 |
| `weather.wetness()` | 0 to 1 |
| `weather.wet_strength()`, `weather.wet_darken()`, `weather.wet_puddles()`, `weather.wet_puddle_scale()` | read the first `WetSurfaces` (section 9.19): `strength`, `darken`, `puddles` and `puddle_scale_m`. A scene without a `WetSurfaces` raises an error that names the call |
| `weather.set_wet{strength=, darken=, puddles=, puddle_scale=}` | writes those fields of the first `WetSurfaces`; every key is optional and each value is clamped to the field's range (strength 0 to 4, darken 0 to 1, puddles 0 to 1, puddle_scale 0.1 to 100 m). An unknown key or a value that is not a number is an error that names the call, and nothing is written |
| `weather.sway_amplitude()`, `weather.sway_frequency()`, `weather.sway_gust()`, `weather.sway_flutter()` | read the four gains of the first `WindSway` (the foliage sway at `weather.wind()` 1, section 9.19): `amplitude_gain`, `frequency_gain`, `gust_gain` and `flutter_gain`. A scene without a `WindSway` raises an error that names the call |
| `weather.set_sway{amplitude=, frequency=, gust=, flutter=}` | writes those gains of the first `WindSway`; every key is optional and each value is clamped to the field's range (amplitude 0 to 50, frequency 0 to 10, gust -1 to 1, flutter 0 to 50). An unknown key or a value that is not a number is an error that names the call, and nothing is written |
| `weather.snow_cover()` | the cover of the first `SnowCover` (section 9.19), 0 to 1, written by the engine while it snows and melts; a script reads it and cannot set it. A scene without a `SnowCover` raises an error that names the call |
| `weather.snow_strength()`, `weather.snow_build()`, `weather.snow_melt()`, `weather.snow_coverage()`, `weather.snow_scale()`, `weather.snow_wind()` | read the authoring fields of the first `SnowCover`: `strength`, `build_seconds`, `melt_seconds`, `coverage`, `noise_scale_m` and `wind_bias` |
| `weather.set_snow{strength=, build=, melt=, coverage=, scale=, wind=}` | writes those fields; every key is optional and each value is clamped to the field's range (strength 0 to 4, build and melt 0 to 86400 seconds, coverage 0 to 1, scale 0.1 to 100 m, wind 0 to 1). An unknown key (`cover` included) or a value that is not a number is an error that names the call, and nothing is written |
| `weather.ripples_strength()`, `weather.ripples_density()`, `weather.ripples_speed()`, `weather.ripples_ring_width()` | read the fields of the first rain-ripples component (section 9.19a): strength, density, speed and ring_width. A scene without one raises an error that names the call |
| `weather.set_ripples{strength=, density=, speed=, ring_width=}` | writes those fields; every key is optional and each value is clamped to the field's range (strength 0 to 1, density 0 to 1, speed 0.1 to 10, ring_width 0 to 1). An unknown key or a value that is not a number is an error that names the call, and nothing is written |
| `weather.splash_rate()`, `weather.splash_height()`, `weather.splash_life()`, `weather.splash_radius()`, `weather.splash_seed()` | read the fields of the first rain-splash component (section 9.19c): rate, height_m, life_seconds, radius_m and seed. The burst counter is written by the engine and a script cannot read or set it |
| `weather.set_splash{rate=, height=, life=, radius=, seed=}` | writes those fields; every key is optional and each value is clamped to the field's range (rate 0 to 64, height 0.01 to 5 m, life 0.02 to 5 s, radius 0.5 to 500 m, seed 0 to 4294967295). An unknown key or a value that is not a number is an error that names the call, and nothing is written |
| `weather.thunder_speed()`, `weather.thunder_volume()` | read the speed of sound in metres per second and the linear volume of the first thunder-delay component (section 9.19b) |
| `weather.set_thunder{speed=, volume=}` | writes those; every key is optional and each value is clamped to the field's range (speed 1 to 1000000, volume 0 to 4). The thunder clip is set in the scene, not from a script. An unknown key or a value that is not a number is an error that names the call, and nothing is written |
| `weather.wind_degrees()`, `weather.wind_drift()`, `weather.wind_heading()` | read the first wind-direction component (section 9.19d): the authored heading in degrees (0 is +x, counter-clockwise seen from above), the authored drift in degrees per minute, and the current resolved heading in degrees (0 up to 360, written by the engine as it drifts). A scene without one raises an error that names the call |
| `weather.set_wind_direction{degrees=, drift=}` | writes the authored heading and drift; every key is optional and each value is clamped to the field's range (degrees 0 to 360, drift -3600 to 3600 degrees per minute). The resolved heading belongs to the engine: an unknown key (angle_q included) or a value that is not a number is an error that names the call, and nothing is written |
| `weather.cloud_strength()`, `weather.cloud_fair_coverage()`, `weather.cloud_overcast_coverage()`, `weather.cloud_scale()`, `weather.cloud_speed_scale()`, `weather.cloud_height()` | read the first cloud-layer component (section 9.19e): the cover scale, the sky cover in clear weather, the cover at full overcast, the size of one cloud cell in metres, the drift speed scale and the layer height above the camera in metres. A scene without one raises an error that names the call |
| `weather.set_clouds{strength=, fair_coverage=, overcast_coverage=, scale=, speed_scale=, height=}` | writes those fields; every key is optional and each value is clamped to the field's range (strength, fair_coverage and overcast_coverage 0 to 1, scale 50 to 20000 m, speed_scale 0 to 10, height 100 to 20000 m). An unknown key or a value that is not a number is an error that names the call, and nothing is written |
| `weather.rain_level()`, `weather.wind_level()` | read the rain and wind bed levels of the first `WeatherAudio` (section 9.19f), 0 to 1, written by the engine as the beds fade (read-only). A scene without one raises an error that names the call |
| `weather.audio_rain_db()`, `weather.audio_wind_db()`, `weather.audio_fade()`, `weather.audio_enabled()` | read the authored gain at full rain level and at full wind level in dB, the fade time in seconds and the on switch (1 or 0) of the first `WeatherAudio`. A scene without one raises an error that names the call |
| `weather.set_audio{rain_db=, wind_db=, fade=, enabled=}` | writes those fields; every key is optional and each value is clamped to the field's range (rain_db and wind_db -60 to 12 dB, fade 0 to 60 s; for enabled any non-zero number is 1). The two sources belong to the scene and the levels to the engine: an unknown key (rain_q included) or a value that is not a number is an error that names the call, and nothing is written |
| `weather.bolt_min_range()`, `weather.bolt_max_range()`, `weather.bolt_ground_y()`, `weather.bolt_height()`, `weather.bolt_width()`, `weather.bolt_segments()`, `weather.bolt_jitter()`, `weather.bolt_fork()`, `weather.bolt_enabled()`, `weather.bolt_light()`, `weather.bolt_light_radius()` | read the authoring of the first `LightningBolt` (section 9.19g): strike distance range in metres, ground height, bolt height, width, the segment exponent (2^n segments), the sideways wander, the fork and enabled switches (1 or 0), the point light intensity and its radius. A scene without one raises an error that names the call |
| `weather.bolt_distance()`, `weather.bolt_pos()`, `weather.bolt_active()` | read the last strike (read-only, written by the engine): its distance from the camera in metres, its ground position as two numbers `x, z`, and the flash ticks left (0 outside a flash). All 0 before the first strike |
| `weather.set_bolt{min_range=, max_range=, ground_y=, height=, width=, segments=, jitter=, fork=, light=, light_radius=, enabled=}` | writes those fields; every key is optional and each value is clamped to the field's range (min_range and max_range 10 to 5000 m, height 50 to 3000 m, width 0.05 to 20 m, segments 2 to 7 and rounded down, jitter 0 to 0.6, light 0 to 100000, light_radius 1 to 5000 m; for fork and enabled any non-zero number is 1). The last strike belongs to the engine: an unknown key (strike_x included) or a value that is not a number is an error that names the call, and nothing is written |
| `weather.gust_strength()`, `weather.gust_min_gap()`, `weather.gust_max_gap()`, `weather.gust_min_rise()`, `weather.gust_max_rise()`, `weather.gust_min_fall()`, `weather.gust_max_fall()`, `weather.gust_min_peak()`, `weather.gust_max_peak()`, `weather.gust_seed()`, `weather.gust_affects()`, `weather.gust_enabled()` | read the authoring of the first `WindGust` (section 9.19h): how much a full gust adds, the calm gap, rise and fall times in seconds, the gust height range, the seed, the consumer bit mask (1 sway, 2 rain, 4 clouds, 8 snow, 16 audio) and the on switch (1 or 0). A scene without one raises an error that names the call |
| `weather.gust_level()`, `weather.effective_wind()` | read-only: the current gust level (0 to 1, 0 in the calm gaps) and the gusted wind `clamp(wind * (1 + strength * level), 0, 1)` with every consumer bit applied (`weather.wind()` keeps returning the smooth value). A scene without a `WindGust` raises an error that names the call |
| `weather.set_gust{strength=, min_gap=, max_gap=, min_rise=, max_rise=, min_fall=, max_fall=, min_peak=, max_peak=, seed=, affects=, enabled=}` | writes those fields; every key is optional and each value is clamped to the field's range (strength 0 to 3, min_gap and max_gap 0 to 120 s, min_rise and max_rise 0.05 to 30 s, min_fall and max_fall 0.05 to 60 s, min_peak and max_peak 0 to 1, seed and affects 0 to 4294967295 and rounded down; for enabled any non-zero number is 1). It never adds the component: an unknown key (gust_q included), a value that is not a number or a scene without a `WindGust` is an error that names the call, and nothing is written |
| `weather.mist_strength()`, `weather.mist_peak_hour()`, `weather.mist_rise()`, `weather.mist_burn()`, `weather.mist_base_humidity()`, `weather.mist_wet_gain()`, `weather.mist_weather_damp()`, `weather.mist_density()`, `weather.mist_thickness()`, `weather.mist_base_height()`, `weather.mist_exp2_density()`, `weather.mist_color()`, `weather.mist_enabled()` | read the authoring of the first `GroundMist` (section 9.19i): the level scale, the hour of the thickest mist, the build-up and burn-off hours, the mist on a dry day, how much wetness adds, how far rain/storm/snow weather damps it, the volumetric density, the height scale in metres, the base height in metres, the exp2 density added when volumetric fog is off, the colour (three numbers `r, g, b`) and the on switch (1 or 0). A scene without one raises an error that names the call |
| `weather.mist_level()` | read-only: the mist level now (0 to 1), from `Environment3D.time_of_day` and the first `Weather` (no `Weather`: no damping and wetness 0). It is 0 when the scene has no clock (`time_of_day` -1). A scene without a `GroundMist` raises an error that names the call |
| `weather.set_mist{strength=, peak_hour=, rise_hours=, burn_hours=, base_humidity=, wet_gain=, weather_damp=, density=, thickness=, base_height=, exp2_density=, color=, enabled=}` | writes those fields; every key is optional and each value is clamped to the field's range (strength 0 to 2, peak_hour 0 to 24, rise_hours and burn_hours 0.1 to 12, base_humidity, wet_gain, weather_damp and density 0 to 1, thickness 0.5 to 200 m, base_height -1000 to 1000 m, exp2_density 0 to 0.2, `color` a table `{r, g, b}` with each lane 0 to 1; for enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a `color` that is not three numbers or a scene without a `GroundMist` is an error that names the call, and nothing is written. The mist needs a clock: without a `WorldClock` the level stays 0 |
| `weather.moon_phase_offset()`, `weather.moon_cycle_days()`, `weather.moon_freeze_phase()`, `weather.moon_inclination()`, `weather.moon_light_intensity()`, `weather.moon_light_color()`, `weather.moon_light_shadows()`, `weather.moon_disk_size()`, `weather.moon_disk_intensity()`, `weather.moon_star_density()`, `weather.moon_star_brightness()`, `weather.moon_star_twinkle()`, `weather.moon_enabled()` | read the authoring of the first `Moon` (section 9.19j): the phase at day 0 hour 0, the days per cycle, the pinned phase (negative follows the clock), the path's tilt off the sun's arc in degrees (`inclination_deg`), the moonlight intensity, colour (three numbers `r, g, b`) and shadows (1 or 0), the disk's cosine size and brightness, the star density, brightness and twinkle, and the on switch (1 or 0). A scene without one raises an error that names the call |
| `weather.moon_phase()`, `weather.moon_illumination()`, `weather.moon_direction()`, `weather.moon_level()` | read-only, from the kit helpers on `Environment3D.time_of_day`, the first `WorldClock`'s day (0 without one) and the first `Weather` (none: no damping): the phase (0 new, 0.5 full), the lit fraction (0 to 1), the unit direction the moonlight travels as three numbers `x, y, z` (negate it for the direction toward the moon) and the level (0 to 1; 0 with no clock or with the moon below the horizon). A scene without a `Moon` raises an error that names the call |
| `weather.set_moon{phase_offset=, cycle_days=, freeze_phase=, inclination=, light_intensity=, light_color=, light_shadows=, disk_size=, disk_intensity=, star_density=, star_brightness=, star_twinkle=, enabled=}` | writes those fields (`inclination` is `inclination_deg`); every key is optional and each value is clamped to the field's range (phase_offset 0 to 1, cycle_days 2 to 120, freeze_phase -1 to 1, inclination 0 to 30, light_intensity 0 to 2, `light_color` a table `{r, g, b}` with each lane 0 to 1, disk_size 0.99 to 1, disk_intensity 0 to 50, star_density 0 to 1, star_brightness 0 to 4, star_twinkle 0 to 1; for light_shadows and enabled any non-zero number is 1). It never adds the component: an unknown key (`inclination_deg` included), a value that is not a number, a `light_color` that is not three numbers or a scene without a `Moon` is an error that names the call, and nothing is written. Moon, moonlight and stars need a clock: without a `WorldClock` there is no moon |
| `weather.aurora_latitude()`, `weather.aurora_gate_low()`, `weather.aurora_gate_high()`, `weather.aurora_strength()`, `weather.aurora_curtain_count()`, `weather.aurora_base_height()`, `weather.aurora_height_span()`, `weather.aurora_azimuth()`, `weather.aurora_width()`, `weather.aurora_speed()`, `weather.aurora_low_color()`, `weather.aurora_high_color()`, `weather.aurora_night_only()`, `weather.aurora_enabled()` | read the authoring of the first `Aurora` (section 9.19n), named like the `weather.set_aurora` keys: the magnetic latitude in degrees (`latitude_deg`), the gate's two ends in degrees, the brightness gain, the ribbon count, the curtains' lower edge and reach (sines of the elevation), the arc's centre and width in degrees (`azimuth_deg`, `width_deg`), the rate scale, the two colours (three numbers `r, g, b` each), and the night-only and on switches (1 or 0). A scene without one raises an error that names the call |
| `weather.aurora_gate()`, `weather.aurora_level()` | read-only, from the kit helpers on the first `Aurora`: the latitude gate (0 closed to 1 open) and the brightness gain the renderer gets now (0 to 4), which also uses the night fade of the current hour (0 with no clock, so with no clock only `night_only` 0 shows) and the cloud coverage of the first `Weather` and `CloudLayer`. A scene without an `Aurora` raises an error that names the call |
| `weather.set_aurora{latitude=, gate_low=, gate_high=, strength=, curtain_count=, base_height=, height_span=, azimuth=, width=, speed=, low_color=, high_color=, night_only=, enabled=}` | writes those fields (`latitude` is `latitude_deg`, `azimuth` is `azimuth_deg`, `width` is `width_deg`); every key is optional and each value is clamped to the field's range (latitude -90 to 90, gate_low and gate_high 0 to 90, strength 0 to 4, curtain_count 1 to 6 rounded down, base_height 0.02 to 0.6, height_span 0.05 to 0.8, azimuth 0 to 360, width 10 to 180, speed 0 to 4, `low_color` and `high_color` tables `{r, g, b}` with each lane 0 to 1; for night_only and enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a colour that is not three numbers or a scene without an `Aurora` is an error that names the call, and nothing is written. The engine has no globe, so a script moves `latitude` to fade the aurora in and out |
| `weather.rainbow_strength()`, `weather.rainbow_band_width()`, `weather.rainbow_secondary()`, `weather.rainbow_min_wetness()`, `weather.rainbow_max_sun_elevation()`, `weather.rainbow_enabled()` | read the authoring of the first `Rainbow` (section 9.19o), named like the `weather.set_rainbow` keys: the brightness gain, the band width scale, the secondary bow's strength, the ground wetness below which no bow shows, the sun height in degrees above which the bow fades out (`max_sun_elevation_deg`), and the on switch (1 or 0). A scene without one raises an error that names the call |
| `weather.rainbow_level()` | read-only, from the kit helper on the first `Rainbow`: the brightness gain the renderer gets now (0 to 4), from the sun elevation of the current hour (`Environment3D.time_of_day`, else its `sun_direction`), the first `Weather`'s wetness and rain intensity (while it rains or storms) and the cloud coverage of the first `Weather` and `CloudLayer`; 0 without a `Weather`. A scene without a `Rainbow` raises an error that names the call |
| `weather.set_rainbow{strength=, band_width=, secondary=, min_wetness=, max_sun_elevation=, enabled=}` | writes those fields (`max_sun_elevation` is `max_sun_elevation_deg`); every key is optional and each value is clamped to the field's range (strength 0 to 4, band_width 0.5 to 2, secondary 0 to 1, min_wetness 0 to 1, max_sun_elevation 10 to 60; for enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number or a scene without a `Rainbow` is an error that names the call, and nothing is written |
| `weather.meteor_rate()`, `weather.meteor_brightness()`, `weather.meteor_length()`, `weather.meteor_speed()`, `weather.meteor_min_darkness()`, `weather.meteor_night_only()`, `weather.meteor_seed()`, `weather.meteor_enabled()` | read the authoring of the first `Meteor` (section 9.19p), named like the `weather.set_meteor` keys: meteors per minute, the brightness gain, the streak length and speed scales, the night fade below which none shows, the night switch (1 or 0), the seed and the on switch (1 or 0). A scene without one raises an error that names the call |
| `weather.meteor_level()` | read-only, from the kit helper on the first `Meteor`: the level the renderer gets now (0 to 1), from the night fade of the current hour (`Environment3D.time_of_day`; 0 with no clock, so then only `night_only` 0 shows), the cloud coverage of the first `Weather` and `CloudLayer` and the first `Weather`'s rain intensity (while it rains or storms). A scene without a `Meteor` raises an error that names the call |
| `weather.set_meteor{rate=, brightness=, length=, speed=, min_darkness=, night_only=, seed=, enabled=}` | writes those fields; every key is optional and each value is clamped to the field's range (rate 0 to 30, brightness 0 to 4, length 0.5 to 2, speed 0.5 to 2, min_darkness 0 to 1, seed 0 to 65535 rounded down; for night_only and enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number or a scene without a `Meteor` is an error that names the call, and nothing is written |
| `weather.season_enabled()`, `weather.season_year_days()`, `weather.season_start_phase()`, `weather.season_winter_density()`, `weather.season_snow_cover_bias()`, `weather.season_cold_bias()`, `weather.season_strength()` | read the authoring of the first `Season` (section 9.19q), named like the `weather.set_season` keys: the on switch (1 or 0), days in a year, the phase at day 0 hour 0, the share of copies left in deep winter, the extra snow cover and the lee shift at the centre of winter, and the strength scale. A scene without one raises an error that names the call |
| `weather.season_spring_tint()`, `weather.season_autumn_tint()`, `weather.season_winter_tint()` | the foliage tint multiplier at the centre of each season, returned as three numbers `r, g, b` (linear; summer is white). A scene without a `Season` raises an error that names the call |
| `weather.season_phase()` | read-only, from the kit helper on the first `Season`: the year phase now (0 to 1; spring 0 to 0.25, summer to 0.5, autumn to 0.75, winter to 1) at the first `WorldClock`'s `day` and `hour`; with no clock it is the fixed `start_phase`. A scene without a `Season` raises an error that names the call |
| `weather.set_season{enabled=, year_days=, start_phase=, spring_tint={r,g,b}, autumn_tint={r,g,b}, winter_tint={r,g,b}, winter_density=, snow_cover_bias=, cold_bias=, strength=}` | writes those fields; every key is optional and each value is clamped to the field's range (year_days 1 to 3650, start_phase, winter_density, snow_cover_bias, cold_bias and strength 0 to 1, each tint channel 0 to 4; for enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a tint that is not a table of three numbers or a scene without a `Season` is an error that names the call, and nothing is written |
| `weather.water_ice_enabled()`, `weather.water_ice_freeze_start()`, `weather.water_ice_freeze_full()`, `weather.water_ice_manual()`, `weather.water_ice_ice_roughness()`, `weather.water_ice_ice_reflectivity()`, `weather.water_ice_ice_depth_m()`, `weather.water_ice_calm()`, `weather.water_ice_strength()` | read the authoring of the first `WaterIce` (section 9.19r), named `weather.water_ice_` plus the `weather.set_water_ice` key: the on switch (1 or 0), the winter weight at which ice starts to form and at which the water is fully frozen, the manual floor, the roughness, the reflection share and the depth scale of frozen water, how far the waves are flattened, and the strength scale. A scene without one raises an error that names the call |
| `weather.water_ice_ice_color()` | the colour frozen water is mixed toward, returned as three numbers `r, g, b` (linear) |
| `weather.water_frozen()` | read-only, from the kit helper on the first `WaterIce`: the freeze amount now (0 to 1), the same one the renderer draws, from the first `Season`'s winter weight at the first `WorldClock`'s `day` and `hour` (with no clock the fixed `start_phase`; with no `Season` only the `manual` floor; 0 when `enabled` or `strength` is 0). It does not change `water.height_at`, `water.normal_at` or `water.submerged`, which keep the authored waves, so a script reads it to stop a boat or switch a rule. A scene without a `WaterIce` raises an error that names the call |
| `weather.set_water_ice{enabled=, freeze_start=, freeze_full=, manual=, ice_color={r,g,b}, ice_roughness=, ice_reflectivity=, ice_depth_m=, calm=, strength=}` | writes those fields; every key is optional and each value is clamped to the field's range (`ice_depth_m` 0.05 to 10, the others and each colour channel 0 to 1; for enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a colour that is not a table of three numbers or a scene without a `WaterIce` is an error that names the call, and nothing is written |
| `weather.leaf_fall_enabled()`, `weather.leaf_fall_autumn_start()`, `weather.leaf_fall_autumn_full()`, `weather.leaf_fall_autumn_end_full()`, `weather.leaf_fall_autumn_end()`, `weather.leaf_fall_manual()`, `weather.leaf_fall_rate()`, `weather.leaf_fall_box_xz_m()`, `weather.leaf_fall_height_m()`, `weather.leaf_fall_size_m()`, `weather.leaf_fall_lifetime_s()`, `weather.leaf_fall_fall_speed()`, `weather.leaf_fall_wind_follow()`, `weather.leaf_fall_tint_follow()`, `weather.leaf_fall_max_particles()`, `weather.leaf_fall_seed()`, `weather.leaf_fall_strength()` | read the authoring of the first `LeafFall` (section 9.19s), named `weather.leaf_fall_` plus the `weather.set_leaf_fall` key: the on switch (1 or 0), the four year phases of the autumn window (leaves start to fall, reach full strength, start to thin, are gone), the manual floor, the leaves per second at full strength, the half extent of the spawn box and the spawn height in metres, the leaf size, the lifetime, the terminal fall speed, the wind-follow weight, how far the colour follows the season foliage tint, the particle budget, the seed and the strength scale. A scene without one raises an error that names the call |
| `weather.leaf_fall_leaf_color()` | the leaf colour, returned as three numbers `r, g, b` (linear) |
| `weather.leaf_falling()` | read-only, from the kit helper on the first `LeafFall`: the leaf amount now (0 to 1), the same one the renderer draws, from the first `Season`'s autumn window at the first `WorldClock`'s `day` and `hour` (with no clock the fixed `start_phase`; with no `Season` only the `manual` floor; 0 when `enabled` or `strength` is 0). A scene without a `LeafFall` raises an error that names the call |
| `weather.set_leaf_fall{enabled=, autumn_start=, autumn_full=, autumn_end_full=, autumn_end=, manual=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, fall_speed=, wind_follow=, leaf_color={r,g,b}, tint_follow=, max_particles=, seed=, strength=}` | writes those fields; every key is optional and each value is clamped to the field's range (`rate` 0 to 500, `box_xz_m` 1 to 100, `height_m` 1 to 50, `size_m` 0.02 to 1, `lifetime_s` 1 to 30, `fall_speed` 0 to 5, `wind_follow` 0 to 2, `max_particles` 64 to 8192 rounded down, `seed` 0 to 4294967295, the other keys and each colour channel 0 to 1; for enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a colour that is not a table of three numbers or a scene without a `LeafFall` is an error that names the call, and nothing is written |
| `weather.firefly_enabled()`, `weather.firefly_summer_start()`, `weather.firefly_summer_full()`, `weather.firefly_summer_end_full()`, `weather.firefly_summer_end()`, `weather.firefly_manual()`, `weather.firefly_night_only()`, `weather.firefly_min_darkness()`, `weather.firefly_rate()`, `weather.firefly_box_xz_m()`, `weather.firefly_height_m()`, `weather.firefly_size_m()`, `weather.firefly_lifetime_s()`, `weather.firefly_drift_speed()`, `weather.firefly_max_particles()`, `weather.firefly_seed()`, `weather.firefly_strength()` | read the authoring of the first `Firefly` (section 9.19u), named `weather.firefly_` plus the `weather.set_firefly` key: the on switch (1 or 0), the four year phases of the summer window (fireflies start to show, reach full strength, start to thin, are gone), the manual floor, the night-only switch and the night fade below which none show, the fireflies per second at full strength, the half extent of the spawn box and the spawn height in metres, the sprite size, the lifetime, the drift speed, the particle budget, the seed and the strength scale. A scene without one raises an error that names the call |
| `weather.firefly_color()` | the glow colour, returned as three numbers `r, g, b` (linear) |
| `weather.fireflies_glowing()` | read-only, from the kit helper on the first `Firefly`: the firefly amount now (0 to 1), the same one the renderer draws, from the first `Season`'s summer window at the first `WorldClock`'s `day` and `hour` (with no clock the fixed `start_phase`), floored by `manual`, times the night gate (the `Environment3D` hour; with no clock the night fade is 0, so only `night_only` 0 shows) and `1 - rain`; with no `Season` only the `manual` floor; 0 when `enabled` or `strength` is 0. A scene without a `Firefly` raises an error that names the call |
| `weather.set_firefly{enabled=, summer_start=, summer_full=, summer_end_full=, summer_end=, manual=, night_only=, min_darkness=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, drift_speed=, color={r,g,b}, max_particles=, seed=, strength=}` | writes those fields; every key is optional and each value is clamped to the field's range (`rate` 0 to 200, `box_xz_m` 1 to 100, `height_m` 1 to 20, `size_m` 0.02 to 0.5, `lifetime_s` 1 to 30, `drift_speed` 0 to 2, `max_particles` 16 to 4096 rounded down, `seed` 0 to 4294967295, the other keys and each colour channel 0 to 1; for enabled and night_only any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a colour that is not a table of three numbers or a scene without a `Firefly` is an error that names the call, and nothing is written |
| `water.level()`, `water.half_extent()`, `water.wave_count()`, `water.amplitude()`, `water.wavelength()`, `water.steepness()`, `water.wind_dir()`, `water.wind_follow()`, `water.speed_scale()`, `water.shallow_color()`, `water.deep_color()`, `water.depth_scale()`, `water.reflectivity()`, `water.roughness()`, `water.foam_amount()`, `water.foam_width()`, `water.enabled()` | read the authoring of the first `Water` (section 9.19k): the calm level, the plane's half size, the wave count, the largest wave's height, the longest wavelength, the steepness, the main direction in degrees (`wind_dir_deg`), the wind-follow weight, the speed scale, the two tint colours (three numbers `r, g, b` each), the depth scale, the reflectivity, the roughness, the foam amount and band width, and the on switch (1 or 0). A scene without one raises an error that names the call |
| `water.height_at(x, z)`, `water.normal_at(x, z)`, `water.submerged(x, y, z)` | read-only, the drawn surface (section 9.19m) at the scene time `mce.tick() * mce.dt`: the height of the water at that world position, its unit normal (three numbers) and whether `y` is below the height. A non-finite argument or a scene without a `Water` raises an error that names the call |
| `water.set{level=, half_extent=, wave_count=, amplitude=, wavelength=, steepness=, wind_dir=, wind_follow=, speed_scale=, shallow_color=, deep_color=, depth_scale=, reflectivity=, roughness=, foam_amount=, foam_width=, enabled=}` | writes those fields (`wind_dir` is `wind_dir_deg`); every key is optional and each value is clamped to the field's range (level -1000 to 1000, half_extent 1 to 20000, wave_count 1 to 8 rounded down, amplitude 0 to 10, wavelength 1 to 500, steepness 0 to 1, wind_dir 0 to 360, wind_follow 0 to 1, speed_scale 0 to 4, `shallow_color` and `deep_color` tables `{r, g, b}` with each lane 0 to 1, depth_scale 0.1 to 200, reflectivity 0 to 1, roughness 0 to 1, foam_amount 0 to 1, foam_width 0.05 to 20; for enabled any non-zero number is 1). It never adds the component: an unknown key, a value that is not a number, a colour that is not three numbers or a scene without a `Water` is an error that names the call, and nothing is written |

```lua
-- A storm rolls in when the player enters the harbour.
function update()
    if mce.pressed("Interact") and weather.target() ~= "storm" then
        weather.set("storm", 20)
    end
end
```

## 27.4 The rules

- **No state between ticks.** Everything a script remembers lives in components, so the state hash,
  saves, replays and a reload cover it. The cook enforces it: inside a function, assigning a top-level
  local, writing into a table one holds (`t.x = ...`, `t[k] = ...`) or changing one through
  `table.insert`/`remove`/`clear`/`sort`/`move`, `rawset`, `setmetatable` or `buffer.write*` is an error
  at `file:line:col`:

  ```
  assets/scripts/score.luau:3:5: error: a function assigns top-level local 'count', which would keep state between ticks; keep it in a component [cook.script]
  ```

  The top level itself may build constant tables and field handles (it runs once, when the script
  loads), and the locals of `update()` and the closures inside it are fine, since they live for one call.
  Globals are read-only after the top level runs.

  The cook also follows a local that points into a top-level table (`local row = grid[i]; row.x = 1`,
  `local a = t; a.n += 1`) and writes into a global table (`STATE.n += 1`, `table.insert(STATE, 1)`).

  What the cook cannot see is caught when the game runs. Once the top level has run, every table that
  `update()` and the script's globals reach is frozen, so writing into it through a helper's parameter
  (`fill(t)`) or a `newproxy` metatable fails at that line with `attempt to modify a readonly table (tables reachable
  from update() are frozen after the top level; keep state in a component)`. Upvalues (a closure made
  by a factory, `local inc = counter()`) and buffers it reaches are compared after each `update()`; a
  change disables the script on that tick with `update() kept state between ticks: upvalue 'n' of the
  function defined at line 3 changed (reached from upvalue 'inc' of update()); keep it in a component`.
  A top-level scratch table or a cache filled lazily is therefore an error too: build it at the top
  level, or make it per call.

  The check runs in the editor, `mce run`, `run_headless` and `mce_player -headless`, where a script
  that failed for this or any other reason also makes the player exit with status 3 (after printing
  the state hash), so a CI run notices. A shipped game's window leaves it off (the pack found beside
  the executable): a player cannot fix a script and the check costs a walk of the script's data once
  and a compare per tick. `-script.state_check 1` or `0` overrides either default.
- **Keys with a stable order.** Iterating a table keyed by tables or functions follows their addresses,
  which differ between runs. The cook rejects such keys where the source shows them (`{[{}] = 1}`,
  `seen[f] = true` with `local function f`), and `pairs` and `next` raise when they meet one at run time.
  Key by entity, number or string. One gap remains: a plain `for k, v in t do` loop over such a table
  is not caught (Luau runs that loop inside the VM, and the type check does not track key types), and
  the run-time state check above does not see it either, since iteration order is not state.
- **A sandbox.** Only the `table`, `string`, `math`, `utf8`, `bit32`, `buffer` and `vector` libraries
  are there; `os`, `io`, `debug`, `coroutine`, `print` and `math.random` are not, and the cook says what
  to use instead (`'print' is not available in scripts; use mce.log`).
- **Budgets.** Each script gets a step budget per tick and the VM a memory cap. A script that errors or
  runs over is disabled, its error logged as `script <name>: <file>:<line>: message`, and the other
  scripts keep running. A step-budget failure names the line that spent it (`dungeon:41: step budget exceeded
  (200000 per tick)`); `mce profile` (27.7b) shows how close each script runs to the limit.
- **Deterministic.** The same scene and input give the same state hash on every run, in Debug and
  Release.

**Limits at a glance.** No state between ticks (use a component); no quaternion type (rotations are Euler degrees);
`Perceiver.sees` and `.heard` read as numbers (`0` or `1`), not booleans; no `os`, `io`, `print` or `math.random`.
The full table, with what to do instead, is in the [engine reference](../engine/script.md#limits); the typed list of
every `mce` function is `mce.d.luau` ([where it comes from](../engine/script.md#mcedluau-the-typed-reference)).

## 27.5 Checks before the game runs

The cook checks every script before it compiles it, and reports every problem it finds, each at
`file:line:col`:

- **Names.** A component, event or field a script spells out (`mce.field("Mover", "velocity")`,
  `mce.each("Mover", ...)`, `mce.has`, `mce.add`, `mce.remove`, `mce.spawn`, `mce.events`, `mce.emit`, and
  the keys of the tables passed to them) must exist in the engine's registry. A typo comes with the closest
  name:

  ```
  assets/scripts/mover.luau:1:21: error: unknown component 'Movr' (did you mean 'Mover'?) [cook.script]
  assets/scripts/mover.luau:2:30: error: 'Mover' has no field 'velocty' (did you mean 'velocity'?) [cook.script]
  ```

  A name built at run time (`mce.get(e, kind, "hp")`) is checked when that line runs instead.
- **Types.** Luau's type checker runs against the `mce` API: a misspelt function (`mce.spwan`), a missing
  argument or an unknown global is an error. Scripts are checked in Luau's nonstrict mode; a first line
  `--!strict` asks for strict typing of that script.

The API the scripts are checked against is written to `cooked/mce.d.luau` whenever the project has
scripts: every `mce` function with its signature, the `ComponentName` and `EventName` unions, and each
component's and event's fields. Point an editor's Luau language server (luau-lsp) at it as a definitions
file to get completion and the same type errors while you type.

An agent checks a script without cooking with the MCP tool `script_check` (`path` of a `.luau` under
`assets/`, or the `source` text; `definitions: true` also returns `mce.d.luau`). It returns
`{ok, problems: [{line, col, message}]}`. When a script fails while the game runs, `run_headless` and
`test_run` return it under `script_errors` (`{script, error}` with `file:line`), and a test with a failed
script does not pass.

## 27.6 While you work

- **Hot reload.** Edit a `.luau` file while the game plays in the editor: the editor recooks it and the
  next tick runs the new code. Scripts keep no state between ticks, so nothing is lost, and a script
  that had failed runs again once the fix cooks.
- **Errors in the editor.** A script error in the Console or the Cook panel names the script and line.
  Double-click the Console line to open the script in the system's editor (the `scripts/x.luau:3` text
  is copied for its "go to line"); the Cook panel's context menu has "Open script".
- **Profiler.** In a Tracy build each script's `update()` has its own zone, named after the script.
- **Crash reports.** When the game crashes while a script runs, the `.crash` report has a `context:` section
  with the script's name and its Luau call stack (`at scripts/spin.luau:2 in spin`).

## 27.7 A game with no C++

`mce new MyGame --template script` makes a project of scripts and data: `MyGame.mceproj` (with an empty
`"games"` list, which is what marks it), `assets/scenes/main.mcscene`, a first script and a texture.
There is no `CMakeLists.txt` and no `games/` folder, and nothing needs a C++ compiler.

    mce build MyGame                 # cook assets/ (checks every script) into MyGame/cooked and MyGame/assets.mce
    mce play MyGame                  # the stock mce_player plays that pack and the project's start scene
    mce headless MyGame --ticks 300  # no window; prints the state hash
    mce editor MyGame                # the editor, with no game module to load
    mce mcp MyGame                   # the MCP server for an agent; script_check checks one script
    mce ship MyGame --version 1.0    # MyGame/dist/MyGame-1.0-<os>.zip

`mce play` and `mce headless` cook first (incremental), so a saved script is what runs. The zip holds
`mce_player`, the engine library, `assets.mce` and a launcher, `MyGame.sh` (`MyGame.bat` on Windows), that
starts the player on the pack and the start scene; arguments after the launcher's name go to the player.
Keep the files together.

**Phones and the browser.** The phone and web presets build the Luau runtime too (`MCE_WITH_SCRIPT` is on),
and the same project builds from the engine checkout: copy it to `projects/MyGame/` and run
`python3 scripts/ship.py --project projects/MyGame --target android|ios|web` (or `mce ship MyGame --target web`
from there). The engine's CMake gives such a project a host that is the stock player with the project's pack
(`mce_add_script_project`), and the host build's `mce_cook` cooks the pack. The phone and web toolchains (the
Android NDK, Xcode, the Emscripten SDK) are the ones the other projects use.


### By hand, without `mce new`

The smallest project is three files, and the commands below are the raw tools `mce` wraps (a build of the engine has
`mce_cook` and `mce_player`).

```
Blind/Blind.mceproj
Blind/assets/scripts/game.luau
Blind/assets/scenes/main.mcscene
```

```json
{ "format": "mce-project", "version": 1, "name": "Blind", "title": "Blind", "engine_version": "0.9.0",
  "games": [], "start_scene": "11111111-2222-4333-8444-555555555555", "plugins": [] }
```

`start_scene` is the `guid` of the scene file (choose any GUID; the scene carries the same one). A scene needs its
script's GUID in a `ScriptSystem` entity, and that GUID exists only once the script has a sidecar, so adopt first:

    mce_cook Blind --adopt                      # writes the sidecars; copy the script's guid into the scene
    mce_cook Blind --pack                       # cook and write Blind/assets.mce
    mce_player -headless 1 -ticks 100 -pack Blind/assets.mce -input tests/run.input   # one "from-to Action=value" line per range, chapter 11.5

A scene entity that holds script memory lists all 16 numbers of `ScriptVars.v` (the kind slot included); a coloured
square is a `Sprite` with no texture and a `tint`; `CameraFollow2D.target` is `"$<index of the entity in the scene>"`.
In 2D, `mce.axis2("Move")` is +y up (W and the up arrow), like the world. A pickup without physics is `mce.near(position, radius, "Coin")`
(or `vector.magnitude(a - b) < r` over `LocalTransform.position`); `mce.destroy` is safe inside `mce.each`. The headless run
prints `mce.log` lines and the state hash; to see component values after a run use `mce inspect MyGame --component Coin` (every live
entity with that component, at most 64) or `--entity <id or name>`, add `--show ScriptVars,LocalTransform` to pick the components and
`--ticks N` for the run length: it prints the live values as JSON, `ScriptVars.v` included. To see how a value moved over the run, `--trace Hero:ScriptVars.v` (or
`--trace 7:LocalTransform.position.x`, repeatable, `--every N` ticks, default 10) prints one row per sampled tick. `.mctest` scenarios run with `mce test MyGame`
(chapter 14.3.1), not through `mce_player`.


## 27.7b Seeing what a script costs: `mce profile`

`mce profile MyGame [--ticks N]` plays the scene headless and prints, for each scene script, its updates, the average and peak **steps**
per update and the share of the per-update budget the peak used (200000 steps by default). A step is a call or a loop pass, counted
the same on every machine, so the table is a fixed number you can compare between changes, not a timing. A script close to its budget
will be disabled by one more loop; exit code 3 if a script failed. `run_headless` returns the same list as `scripts`.

## 27.8 Remembering things: `ScriptVars`

A script keeps nothing between ticks, and a game with no C++ cannot define its own components, so the kit
has one made for this: `ScriptVars`, 16 numbers (`v`) on any entity, saved and hashed like every other
component. A script names the slots however it likes; write the layout in a comment at the top.

```lua
local VARS = mce.field("ScriptVars", "v")

function update()
    mce.each("ScriptVars", function(e)
        local v = mce.get(e, VARS)      -- a table of 16 numbers (v[1] .. v[16])
        v[1] = v[1] + 1                 -- a counter
        mce.set(e, VARS, v)             -- write the table back
    end)
end
```

**Named slots.** A handle can name one slot, so the layout lives in the names and not in `v[3]` comments:

```lua
local COINS = mce.field("ScriptVars", "v", 1)
local KIND  = mce.field("ScriptVars", "v", 16)

function update()
    mce.each("ScriptVars", function(e)
        if mce.get(e, KIND) == 1 then mce.set(e, COINS, mce.get(e, COINS) + 1) end
    end)
end
```

`mce.get(e, COINS, KIND)` reads several, `mce.set(e, COINS, 5, KIND, 1)` writes several, and a slot handle mixes with
the whole-array handle in one call. A number outside 1..16, or an `element` on a field that is not an array, is an error.

The scene gives the first values (`"ScriptVars": { "v": [0, 1, 38, 3, 4, 0, ...] }`). A convention that scales
to several kinds of entity: keep the kind in the last slot (`v[16]`: 1 = player, 2 = enemy, 3 = goal) and let
each script skip the entities that are not its own.

**Tuning that belongs to the scene: `ScriptParams`.** `ScriptVars` is memory a script writes; constants an author sets per
entity (a walker's speed, a door's delay) live in `ScriptParams`, which a script only reads. `names` lists the name of
each number (space or comma separated) and `v` holds up to eight values:

```json
"ScriptParams": { "names": "patrol_speed chase_speed", "v": [1.6, 3.4, 0, 0, 0, 0, 0, 0] }
```

```lua
local speed = mce.param(e, "patrol_speed", 1.6)   -- the scene's value, or 1.6 on an entity with none
```

One script then serves many entities with different tuning, and the values are plain scene data an agent or the editor changes
(`entity_set_component`) without touching the script. A name that is not in the list is an error unless you gave a default.

**Example: `projects/EmberScript`.** Ember Step's controller and an enemy state machine in Luau, with no
C++ at all: `player.luau` (run, coyote time, jump buffer, a shorter hop when Jump is let go, respawn),
`enemy.luau` (patrol / chase / return, stomp or hurt) and `goal.luau`. Scripts that move bodies run in the
`Post` phase, so the physics bodies exist on the first tick. Its `.mctest` scenarios (`tests/`) are the ones
`test_run` plays: a bot reaches the goal, the walker sends the player back, a jump lands on the walker.

    cmake --build build/linux-gcc-debug --target ember_script
    build/linux-gcc-debug/projects/EmberScript/ember_script -headless 1 -ticks 1800 -input projects/EmberScript/tests/run_right.input

It is a smaller game than the C++ sample `samples/03_platformer` (one level, one enemy, no wall jump or dash),
not a line-by-line port; the Luau-versus-C++ twin the determinism stage compares is the bouncing-box sample
(`samples/script_headless`).

**Gotchas** (found by an agent that built a script-only game blind):

- The `walk_to` bot steers by `x` only in a side view (`plane: "x"`): `through` points and the target never make
  it climb. Build the level so that walking right gets there (a hill of steps), or put a jump in an `input` script.
- `mce.quit()` ends the whole run, tests included. A pickup that quits when the last coin is taken ends every test
  that passes it; log "level complete" and destroy the goal instead, as `goal.luau` does.
- Two kinematic bodies block each other, so a touch check must be wider than the sum of their half-widths
  (a 0.4 and a 0.45 half-width: test `|dx| < 1.0`), or the layers' masks must let them overlap (`projects/EmberScript`).
- `script_check` takes `path` relative to `assets/` (`scripts/player.luau`); `assets/scripts/...` works too.
- Every MCP call in a `mce_mcp` session shares one scene; a one-shot client that starts a server per call should pass
  `"scene": "scenes/main.mcscene"` to `run_headless`.

## 27.9 Recipes

Each is a whole script; the state sits in `ScriptVars` or another component, never in a local.

**A countdown that fires once** (`v[1]` = ticks left, set in the scene; `v[16]` = 4 marks a timer entity):

```lua
local VARS = mce.field("ScriptVars", "v")
function update()
    mce.each("ScriptVars", function(e)
        local v = mce.get(e, VARS)
        if v[16] ~= 4 or v[1] <= 0 then return end
        v[1] = v[1] - 1
        mce.set(e, VARS, v)
        if v[1] == 0 then mce.signal("timer_done", e) end   -- a sequence or timeline can wait on it
    end)
end
```

**A state machine** is a number in a slot and an if-chain, as `projects/EmberScript/assets/scripts/enemy.luau`
does (Patrol, Chase, Return); keep the transitions in one place and write the state back once at the end.

**Spawning on a beat** (a prefab every 90 ticks):

```lua
function update()
    if mce.tick() % 90 == 0 then
        mce.instantiate("guid:<prefab>", vector.create(mce.random() * 10, 6, 0))
    end
end
```

**Reacting to physics events** (a sensor under a pickup; see `mce.events`):

```lua
function update()
    for _, hit in ipairs(mce.events("TriggerEnter")) do
        if mce.has(hit.other, "ScriptVars") then mce.destroy(hit.sensor) end
    end
end
```

**Playing a sound on a press:** `if mce.pressed("Jump") then mce.play_sound("guid:<sfx>", -6) end`.

**A chaser that goes around walls** (2D; `projects/Dungeon/assets/scripts/dungeon.luau` has the whole thing). Plan now and then, not every
tick, and walk the first corner of the plan:

```lua
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")
-- v[1], v[2] the waypoint; v[3] ticks to the next plan
local function chase(e, target)
    local v = mce.get(e, VARS)
    local pos = mce.get(e, POS)
    v[3] = v[3] - 1
    if v[3] <= 0 then
        v[3] = 20
        local path = mce.find_path(pos, target)          -- nil when the target is walled in
        if path then v[1], v[2] = path[1].x, path[1].y end
    end
    local d = vector.create(v[1] - pos.x, v[2] - pos.y, 0)
    if vector.magnitude(d) > 0.05 then
        mce.set(e, POS, pos + vector.normalize(d) * math.min(3.5 * mce.dt, vector.magnitude(d)))
    end
    mce.set(e, VARS, v)
end
```

A body-less chaser like this can move by writing `LocalTransform.position`; one with a 2D body moves with `mce.move_and_collide`
or `mce.set_velocity2d`.

**Hit-stop and a shake** (the moment a hit lands): `mce.shake(0.4)`, and slow the clock for a beat with `mce.set_time_scale(0.2)`;
set it back to 1 a few ticks later (count them in a `ScriptVars` slot).

**A save slot** (press F5 to save, F9 to load; only entities with a `SaveKey` and fields marked `Save` are written):

```lua
function update()
    if mce.pressed("QuickSave") then mce.save("quick") end
    if mce.pressed("QuickLoad") and mce.has_save("quick") then mce.load("quick") end   -- applied right after this tick
end
```

**3D pickups** (a coin is collected when the player's sphere touches it):

```lua
function update()
    mce.each("ScriptVars", function(p)
        if mce.get(p, "ScriptVars", "v")[16] ~= 1 then return end
        for _, thing in ipairs(mce.overlap_sphere3d(mce.get(p, "LocalTransform", "position"), 0.6, p)) do
            if mce.has(thing, "Pickup") then mce.destroy(thing) end
        end
    end)
end
```
