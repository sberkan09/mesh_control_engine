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
| `mce.log(msg)` | an info line in the engine log, `script: <msg>` |

**Missions and map markers.** Two more global tables, `mission` and `marker`, are thin over the mission kit (section 9.13) and the map markers (9.15). They keep nothing: the state is the `Mission` and `MapMarker` components and the `MissionSignal` event, so saves, replays and the editor see it. A world without the kit pieces gets an error that says so; a bad argument is a Luau error that names the call, and it disables only that script.

| Call | Does |
|---|---|
| `mission.start(e)` | an idle mission starts on the next tick (sets `Mission.start`); `true`, or `false` when it is already active, complete or failed |
| `mission.fail(e)` | fails an active mission now and sends `MissionFailed` with `reason` 3 (a script; 0 is the time limit, 1 `fail_on_death`, 2 a vanished objective); `true`, or `false` in any other state |
| `mission.status(e)` | `state, objective, elapsed, done`: `"idle"`, `"active"`, `"complete"` or `"failed"`; the active objective entity (`nil` unless active); seconds since it started; how many objectives are done |
| `mission.signal(name, amount?)` | a `MissionSignal` for the active `signal` objectives (`amount` a whole number from 1, default 1; `name` up to 31 characters). The script runs after the kit's mission system in a tick, so the signal counts two ticks later |
| `marker.add(e, {icon?, color?, layer?, label?, always_visible?, follow?})` | puts a `MapMarker` on the entity at the next sync point. `color` is `{r, g, b}`, `{r, g, b, a}` or a vector (`colour` works too); `layer` 0 to 31; `label` up to 31 characters; `follow` an `Objective` or `Mission` entity. Fields left out keep the current marker's, or the defaults; an unknown field is an error |
| `marker.remove(e)` | takes the `MapMarker` off at the next sync point (nothing happens without one) |

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

  What the cook cannot see is caught when the game runs. Once the top level has run, every table that
  `update()` and the script's globals reach is frozen, so writing into it through an alias
  (`local a = t; a.n += 1`), a helper's parameter (`fill(t)`), a global table (`STATE.n += 1`) or a
  `newproxy` metatable fails at that line with `attempt to modify a readonly table (tables reachable
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
  scripts keep running.
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
In 2D, `mce.axis2("Move")` is +y up (W and the up arrow), like the world. A pickup without physics is a distance test
(`vector.magnitude(a - b) < r` over `LocalTransform.position`); `mce.destroy` is safe inside `mce.each`. The headless run
prints `mce.log` lines and the state hash; it does not dump components, and `.mctest` scenarios run with `mce test MyGame`
(chapter 14.3.1), not through `mce_player`.


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
