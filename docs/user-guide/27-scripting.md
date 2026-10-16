# 27. Scripting in Luau

Gameplay rules can be written in Luau, a sandboxed Lua dialect, instead of C++. A script is an asset:
the cook compiles it to bytecode, a scene names it, and the stock player runs it with no game module
and no C++ compiler. The track is being built (ROADMAP M-SCRIPT, ADR-0041); this chapter describes what
works today and grows with it.

Scripting needs a build with `MCE_WITH_SCRIPT` (on in the desktop presets). Phones and the web turn it
on in a later M-SCRIPT step.

## 27.1 A script

Put a `.luau` file anywhere under `assets/`. The cook writes its sidecar (`importer: "script"`) the
first time it sees it and compiles it into a `Script` asset (a `CHNK_SCRIPT` chunk of Luau bytecode).
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
| `phase` | `Pre` | `Pre`: before physics (FixedUpdate_Pre); `Post`: after it (FixedUpdate_Post) |
| `order` | 0 | scripts of a phase run lowest first; ties run in ECS order |
| `enabled` | true | off: the script is not loaded |

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
| `mce.field("Comp", "field")` | a handle to one field, looked up once; call it at the script's top level |
| `mce.get(e, h, ...)` | the values of one or more fields (handles, or `"Comp", "field"` pairs) in one call |
| `mce.set(e, h, v, ...)` | writes one or more fields (`h, v, h2, v2, ...`) and marks the components changed |
| `mce.each("A", "B", ..., fn)` | calls `fn(e)` for every entity with all the components, in the order a Query visits |

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
| `mce.instantiate("guid:<prefab>", pos?)` | spawns a prefab, its root at `pos`; the root entity, or `nil` (logged) |

**Events and signals.** Events are readable from the tick after they were pushed.

| Call | Does |
|---|---|
| `mce.events("TriggerEnter")` | this tick's events as an array of tables: `TriggerEnter`/`TriggerExit` (`sensor`, `other`), `ContactBegin2D` (`a`, `b`, `point`, `normal`, `approach_speed`), `ContactEnd2D`, `TriggerEnter3D`/`TriggerExit3D` (`trigger`, `other`), `JointBreak3D` |
| `mce.emit("Type", {...})` | pushes an event of any reflected event type the world has |
| `mce.signal("name", e?)` | a named signal that sequences' and behavior trees' wait nodes hear |
| `mce.signals("name")` | the emitters of this tick's signals of that name (sequences, timelines, behavior trees) |
| `mce.play_sound("guid:<sfx>", gain_db?)` | a one-shot sound |

**Input.** `mce.pressed(action)`, `mce.held(action)`, `mce.released(action)`, `mce.axis(action)` (a
number) and `mce.axis2(action)` (a vector) read the input map's actions by name (chapter 11). An action the
map does not define reads as not pressed and zero.

**Physics.**

| Call | Does |
|---|---|
| `mce.raycast2d(origin, dir, max, skip?)` | the first hit, `{entity, point, normal, distance}`, or `nil`; `skip` is an entity to ignore |
| `mce.overlap2d(min, max, skip?)` | the entities whose shapes' boxes overlap the box |
| `mce.move_and_collide(e, delta)` | moves a character body; `{position, grounded, ground, ceiling, wall_left, wall_right}` |
| `mce.velocity2d(e)`, `mce.set_velocity2d(e, v)`, `mce.impulse2d(e, v)` | a 2D body's velocity, and pushes |
| `mce.raycast3d(origin, dir, max, skip?)` | the same for the 3D physics world |

**The rest.**

| Call | Does |
|---|---|
| `mce.dt`, `mce.tick()` | the fixed step in seconds and the current tick (from 0) |
| `mce.random()` | a number in [0, 1) that depends only on the seed, the tick, the script and the call |
| `mce.load_scene(target)` | switches scene after this tick (a source path or `"guid:<uuid>"`; section 4.5) |
| `mce.quit()` | ends the run after this tick, like the window's close button |
| `mce.log(msg)` | an info line in the engine log, `script: <msg>` |

## 27.4 The rules

- **No state between ticks.** Everything a script remembers lives in components, so the state hash,
  saves and replays cover it. Globals are read-only after the script's top level runs.
- **A sandbox.** Only the `table`, `string`, `math`, `utf8`, `bit32`, `buffer` and `vector` libraries
  are there; `os`, `io`, `debug`, `coroutine`, `print` and `math.random` are not.
- **Budgets.** Each script gets a step budget per tick and the VM a memory cap. A script that errors or
  runs over is disabled, its error logged as `script <name>: <file>:<line>: message`, and the other
  scripts keep running.
- **Deterministic.** The same scene and input give the same state hash on every run, in Debug and
  Release.
