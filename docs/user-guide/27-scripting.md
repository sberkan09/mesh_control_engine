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

| Call | Does |
|---|---|
| `mce.each("A", "B", ..., fn)` | calls `fn(e)` for every entity with all the components, in the order a Query visits |
| `mce.get(e, "Comp", "field")` | a number, boolean or vector (`Vec2`/`Vec3`); `Vec4`, `Quat` and `Color` come back as four numbers |
| `mce.set(e, "Comp", "field", v...)` | writes the field and marks the component changed |
| `mce.dt`, `mce.tick()` | the fixed step in seconds and the current tick (from 0) |
| `mce.random()` | a number in [0, 1) that depends only on the seed, the tick, the script and the call |
| `mce.log(msg)` | an info line in the engine log, `script: <msg>` |

Spawning, events, input and physics queries arrive in the next M-SCRIPT step.

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
