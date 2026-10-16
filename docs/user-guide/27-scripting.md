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
- **Keys with a stable order.** Iterating a table keyed by tables or functions follows their addresses,
  which differ between runs. The cook rejects such keys where the source shows them (`{[{}] = 1}`,
  `seen[f] = true` with `local function f`), and `pairs` and `next` raise when they meet one at run time.
  Key by entity, number or string. One gap remains: a plain `for k, v in t do` loop over such a table
  is not caught (Luau runs that loop inside the VM, and the type check does not track key types).
- **A sandbox.** Only the `table`, `string`, `math`, `utf8`, `bit32`, `buffer` and `vector` libraries
  are there; `os`, `io`, `debug`, `coroutine`, `print` and `math.random` are not, and the cook says what
  to use instead (`'print' is not available in scripts; use mce.log`).
- **Budgets.** Each script gets a step budget per tick and the VM a memory cap. A script that errors or
  runs over is disabled, its error logged as `script <name>: <file>:<line>: message`, and the other
  scripts keep running.
- **Deterministic.** The same scene and input give the same state hash on every run, in Debug and
  Release.

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
