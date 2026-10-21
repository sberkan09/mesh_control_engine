# 6. Game code

Gameplay is C++ in `games/<game>/module/`. It is built as a **game module** that the host loads
next to the engine: a set of plain components, systems that run over them on a fixed 60 Hz tick,
and one `install()` that registers them. This chapter is the programming model; the following
chapters cover each subsystem's components and calls.

## 6.1 The shape of a module

```cpp
// components.h: data only (core + refl), so the cook can compile it without your gameplay
#include "mce/refl/macros.h"
#include "mce/types.h"

namespace my_game {
using mce::f32;

struct Spin {
    f32 degrees_per_second = 90.0f;
};
MCE_COMPONENT(Spin, degrees_per_second)
}
```

```cpp
// my_game.cpp: systems and install
#include "my_game.h"   // includes components.h and mce/gameplay.h

namespace my_game {
using namespace mce;

namespace {
void spin(Frame& f) {
    f.query<const Spin&, LocalTransform&>().each([&](const Spin& s, LocalTransform& t) {
        const f32 radians = s.degrees_per_second * (math::kPi / 180.0f) * f.dt;
        t.rotation = math::quat_from_axis_angle({0, 0, 1}, radians) * t.rotation;
    });
}
}  // namespace

ResultV install(World& world) {
    world.register_components<Spin>();
    world.add_system(Phase::FixedUpdate_Pre, "my_game_spin", &spin);
    return ok();
}
}  // namespace my_game
```

```cpp
// module.cpp: the entry point the host looks for
#include "my_game.h"
MCE_GAME_MODULE("my_game", 1, &my_game::install)
```

`mce/gameplay.h` is the one include a module needs: core, the ECS, transforms, the input snapshot,
the 2D kit components, 2D physics and audio components, and the module contract. The renderer, the
RHI, the platform layer and the cook are deliberately not part of it.

The second argument of `MCE_GAME_MODULE` is your **module version**, the save-format version of
your game (section 13.3). Bump it when you change what `Save` fields mean.

## 6.2 Components

A component is a plain struct declared with `MCE_COMPONENT(Type, fields...)`. The declaration is the
single source of truth for the inspector, the scene format, the cook, saves and the MCP server.

```cpp
struct Health {
    f32 hp = 100;
    f32 max = 100;
    u32 hits = 0;      // runtime only
    u32 pad_ = 0;
};
MCE_COMPONENT(Health,
              (hp, Range{0, 1000}, Help{"current hit points"}, Save),
              max,
              (hits, ReadOnly, Transient))
```

Field attributes:

| Attribute | Effect |
|---|---|
| `Range{min, max}` | the inspector's slider range (not enforced at run time) |
| `Help{"text"}` | tooltip in the inspector, description in `refl_describe_component` |
| `ReadOnly` | shown in the inspector, never edited there |
| `Transient` | never written to a scene file; runtime state |
| `Save` | included in save games (section 13.3) |

Supported field types: `bool`, fixed-width integers, `f32`/`f64`, `FixedString<N>`, `vec2/3/4`,
`quat`, `Color`, enums declared with `MCE_ENUM(E, values...)`, nested structs declared with
`MCE_REFL_STRUCT`, `Entity` (an entity reference), `asset::AssetRef<T>` (an asset reference) and
one-dimensional fixed arrays of those.

Rules:

- **Plain data only.** Trivially copyable, no pointers to heap, no destructors, no containers.
  Rows move with `memcpy`.
- **No padding surprises.** The determinism hash covers component bytes, so keep layouts
  padding-free: put 8-byte and asset-reference fields first, then 4-byte fields, and add explicit
  `pad_` members to round the size up. Kindle's `components.h` follows this throughout.
- **An empty struct is a tag**: it marks an entity and costs nothing.
- **Keep headers data-only.** `components.h` is compiled into the cook and the MCP server through
  `types.cpp`; include only `mce/refl/macros.h`, `mce/types.h`, `mce/math/math.h` and
  `mce/asset/handle.h` there.
- Names are unqualified inside your namespace; two components may not share a name across
  modules.

## 6.3 Systems and phases

A system is a free function `void (*)(Frame&)`. `install()` registers it in a phase with a name;
registration order within a phase is execution order, and `after` names a system already registered
in that phase when you want to be explicit. Systems run one after another on the main thread (an
automatic parallel scheduler was measured and not merged: `docs/research/auto-parallel-scheduling.md`):

```cpp
world.add_system(Phase::FixedUpdate_Pre, "my_game_movers", &movers);
world.add_system(Phase::FixedUpdate_Pre, "my_game_player", &player, "my_game_movers");
world.add_system(Phase::FixedUpdate_Post, "my_game_triggers", &triggers);
```

The phases, in tick order:

| Phase | Runs | Use it for |
|---|---|---|
| `Startup` | once, after the scene loads | one-time setup that must precede the first tick |
| `FixedUpdate_Pre` | every tick | input handling, timers, AI, moving kinematic bodies, applying forces |
| `FixedUpdate_Physics` | every tick | owned by the engine (the 2D physics step) |
| `FixedUpdate_Post` | every tick | reacting to contacts and triggers, cameras, HUD text, anything that reads `WorldTransform` |
| `Render_Pre`, `Render_Submit` | every rendered frame (also headless), after the first tick | presentation only: frame meters, interpolation, debug draws; never sim state |
| `Editor` | editor only | not for games |

The fixed tick is 1/60 s and `f.dt` is always exactly that. Time scale, pause and catch-up change
how many ticks run per real second, never `dt`, so the simulation is a pure function of the scene,
the tick count and the input (section 6.9).

What a system gets from `Frame`:

| Member | What it is |
|---|---|
| `f.tick`, `f.dt`, `f.time_scale` | the tick index, the fixed step, the current scale |
| `f.world()` | the `World`: `try_get<T>(e)`, `try_get_mut<T>(e)`, `has<T>(e)`, `alive(e)` |
| `f.query<Cs...>()` | iterate entities having all of `Cs` (next section) |
| `f.commands()` | deferred structural changes (section 6.5) |
| `f.events()` | push and read events (section 6.6) |
| `f.resource<T>()`, `f.try_resource<T>()` | world-wide singletons (section 6.7) |
| `f.is_added<T>(e)`, `f.is_changed<T>(e)` | change detection since this system last ran |

## 6.4 Queries

```cpp
f.query<const Mover&, LocalTransform&>().each([&](Entity e, const Mover& m, LocalTransform& t) {
    // ...
});
```

- Request `const T&` to read and `T&` to write. A mutable item marks the component changed for
  change detection, even if you do not write it.
- The lambda may take the `Entity` as its first parameter or omit it.
- Filters: `.with<Tag>()`, `.without<Dead>()`, `.added<T>()`, `.changed<T>()`, `.since(tick)`.
  `.count()` counts without iterating.
- Iteration order is deterministic: tables in creation order, rows in row order. "First in ECS
  order" is a stable notion you can rely on (Kindle picks the first `ColorSwitch` as the one whose
  `start_color` applies).
- Do not create, destroy or add components to entities inside `each`; use `Commands`.

## 6.5 Structural changes: Commands

Inside a phase, table structure is frozen. Spawn, destroy, add and remove go through
`f.commands()`, and they apply at the sync point that ends the phase (spawns first, then
add/remove/set in order, then destroys). A system never sees its own structural changes.

```cpp
Entity bullet = f.commands().spawn(LocalTransform{...}, Sprite{...}, Bullet{...});  // handle valid now
f.commands().add<Dead>(e);
f.commands().set(e, Health{.hp = 0});
f.commands().destroy(e);
```

`World::spawn_with`, `add`, `remove` and `destroy` are the immediate versions for use **outside**
a phase (in `install()`, in a test). Calling them inside a system asserts.

Hold only `Entity` handles across ticks, never pointers into components. Handles are
generational: a destroyed entity's handle fails `alive()` and never aliases a new one.

## 6.6 Events

Events are per-tick lists, double-buffered: what one phase pushes, later phases (and the same
phase next tick) read. Register your own in `install()` with the phase that produces them:

```cpp
struct Scored { Entity who; u32 points; };
world.register_event<Scored>(Phase::FixedUpdate_Post);

// push
f.events().push(Scored{e, 10});
// read
for (const Scored& ev : f.events().read<Scored>()) { ... }
```

Engine events you will read: `physics::TriggerEnter` / `TriggerExit` / `ContactBegin2D` /
`ContactEnd2D` (produced by the physics step, read in `FixedUpdate_Post`), `kit::SeqSignal` from
sequences. Engine events you push: `audio::PlaySfx` to play a one-shot (register it in your
`install()` with `world.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre)`; the audio bridge
reads it).

## 6.7 Resources

A resource is one value per World. The engine provides `input::InputState` (chapter 11),
`physics::World2D` (chapter 9) and the particle and sequence systems; your game adds its own run
state the same way:

```cpp
struct GameState { u32 coins = 0; u32 won = 0; Entity player; };
world.set_resource(GameState{});          // in install()
GameState& gs = f.resource<GameState>();  // in a system
```

Keep all mutable game state in components and resources. Modules may not keep mutable statics;
the World is the state, which is what makes saves, replays and the determinism hash cover
everything. A module may declare boot-time switches with `MCE_CVAR` (an `-autoplay` bot, a debug
view): the player applies their command-line values once the module has loaded, before the
scene. Read such a switch once (in a `Startup` system) and copy what the sim needs into a
resource, so the World still holds the state.

## 6.8 Install order and what the host gives you

The host installs the engine systems before loading your module: input, 2D physics, the audio
bridge, particles and sequences. Your `install()` then:

1. checks what it needs is there (`world.try_resource<physics::World2D>()`),
2. registers events and components (`register_event<T>`, `register_components<Ts...>()`),
3. registers systems in an explicit order,
4. sets resources,
5. returns `ok()` or an `Error`, which the host reports as "module install failed".

The engine's own components (`LocalTransform`, `Sprite`, `RigidBody2D`, ...) are registered by
the host; a scene that uses a type nobody registered fails to load with `NotFound` naming the
column, never silently. Where they are declared, for code that names them:

| Types | Namespace | Header |
|---|---|---|
| `LocalTransform` | `mce` | `mce/ecs/transform.h` (in `mce/gameplay.h`) |
| `WorldTransform`, `ChildOf` | `mce` | `mce/ecs/hierarchy.h` |
| `Sprite`, `AnimatedSprite`, `TextLabel`, `Camera2DComponent`, `TilemapLayer`, `ParticleEmitter`, `ViewInfo` | `mce::kit` | `mce/kit/components.h` (in `mce/gameplay.h`) |
| `MeshRef`, `Camera3DComponent`, `Environment3D`, `PointLight3D`, `BoxCollider3D` | `mce::kit` | `mce/kit/kit3d.h` (in `mce/gameplay.h`) |
| `RigidBody3D`, `CharacterController3D`, `Trigger3D` and its events | `mce::kit` | `mce/kit/bodies3d.h` |
| `SfxTrigger`, `CameraFollow2D`, `CameraFollow3D`, `CameraShake2D` | `mce::kit` | `mce/kit/effects.h` |
| `RigidBody2D`, `Collider2D`, `CharacterController2D` | `mce::physics` | `mce/physics/components.h` (in `mce/gameplay.h`) |
| `AudioSource`, `AudioListener`, `AudioListenerMix`, `AudioBusEffects`, `AudioReverb`, `AudioLimiter`, `AudioDuck`, `AudioSnapshot`, `AudioOcclusion`, `AudioDoppler` | `mce::audio` | `mce/audio/components.h` (in `mce/gameplay.h`) |
| `InputState` | `mce::input` | `mce/input/ecs.h` (in `mce/gameplay.h`) |

A respawned `RigidBody3D` keeps its spin unless you clear it: set `velocity` to zero with
`set_velocity` and `stop_spin` to `true` together with the new position.

Every host installs `InputState` (the player and play-in-editor). If your module can also run
where nobody does (MCP `run_headless`), install an idle snapshot yourself so your systems can
still read it:

```cpp
if (!world.try_resource<input::InputState>()) input::install(world);
```

## 6.9 Determinism, and why it matters to you

Same scene, same tick count, same input, same binary: same `World::state_hash`. The headless run
prints it, the editor's Play panel shows it, MCP's `run_headless` returns it, and the engine's own
tests compare it across runs and across Debug and Release. It is how you (or an agent) verify a
change without looking at pixels, and how replays work.

Keep it that way in game code:

- never read wall-clock time, random devices or anything outside the World in a system (use a
  PCG or seed stored on a component, as the particle system does),
- never let an unordered container's iteration order affect gameplay (the engine's `HashMap` is
  banned from sim-visible order; `FlatMap` is ordered),
- float math is fine as long as it is the same code on the same machine; cross-machine identical
  hashes are not claimed.

## 6.10 Changing scenes

```cpp
#include "mce/scene/scene.h"

void on_goal(Frame& f) {
    if (reached) scene::request_change(f.world(), "scenes/level2.mcscene");
}
```

`request_change` only records the target (the last request in a tick wins), so it is safe in any
phase. Between ticks the host applies it: every entity goes, the target scene is loaded, and your
Startup systems run again for the new scene. Resources are kept, so keep what must survive a
change (score, lives, the level reached) in a resource, not on an entity. A target the pack lacks
is an error: `mce_player` logs it and exits with code 1, and a test run fails with `scene.change`.
`world.try_resource<scene::SceneChange>()->current` is the source path of the scene that is
loaded. Store the next scene's path in a string field, or its GUID in an `AssetRef<asset::Scene>`
field (`scene::request_change(world, ref.guid)`); either can point back at the title.

## 6.11 Spawning prefabs from code

To spawn entities you did not place in the scene, instantiate a cooked prefab:

```cpp
asset::AssetSystem& assets = ...;   // not in Frame: hosts expose it through a resource of your own
auto h = assets.load_sync<asset::Prefab>(assets.find(prefab_guid));
auto view = asset::SceneView::read(assets.resolve(h.value()));
ecs::SpawnParams p;
p.count = 3;
p.parent = spawner;
const ecs::PrefabOverride ov[] = { ecs::PrefabOverride::of("Health.hp", 25.0f) };
p.overrides = ov;
MCE_TRY(ecs::instantiate(f.world(), view.value(), p));   // inside a phase: recorded as Commands
```

The template host does not hand the asset system to the module today; sequences (`spawn` step)
are the authored way to spawn prefabs from a project, and the samples that spawn from code own
their host. For many games `Commands::spawn` with components built in code is simpler.

## 6.12 Reading other components safely

```cpp
if (const Coin* c = f.world().try_get<Coin>(other)) { ... }        // null when absent
if (Health* h = f.world().try_get_mut<Health>(e)) h->hp -= 1;      // marks changed
if (f.world().has<Hazard>(other)) { ... }
```

`try_get_mut` is allowed inside a system (it changes a value, not structure). Pointers are valid
until the next structural change, so do not keep them across ticks.

## 6.13 Logging and errors

`MCE_LOG_INFO("...")`, `MCE_LOG_WARN`, `MCE_LOG_ERROR` write to the engine log, which the editor's
Console panel and the in-game console show. The format is fmt's, with `{}` placeholders:
`MCE_LOG_INFO("gem {} of {}, {:.1f} s left", n, total, t)`. A printf format (`%u`, `%s`) does not
compile, since it would log the codes as they are. Do not log inside per-entity loops; log a summary per
tick if you must. Expected failures return `Result`/`ResultV` (`Error(Errc::InvalidArgument,
"stage", "detail")`); programmer errors assert with `MCE_ASSERT`. There are no exceptions.

## 6.14 Platform notes

- **Linux and macOS** load the game as a shared module (`libmy_game.so` on both; only
  `libmce_engine` is a `.dylib` on macOS). The host copies the file before mapping it, so
  rebuilding the module while the game runs is safe. In a read-only folder (a shipped `.app`, a
  read-only install) the copy fails and the module is mapped in place.
- **Windows** has no game modules yet: `mce_add_project` links the game sources into the host and
  the host uses `PlayerDesc::static_module`. Your code does not change; only the build does.
- The module and the host must come from the same build: the engine id stamped into the module
  (git describe, build type, compiler, sanitizer) must match, or loading fails with
  `EngineIdMismatch`. Rebuild both after pulling engine changes.
- Modules see every engine include directory, so nothing stops you including the renderer. Do not:
  `mce/gameplay.h` is the supported surface, and a module that draws cannot run headless or in
  the editor.

## 6.15 Testing gameplay

Three levels, cheapest first:

1. `my_game -headless 1 -ticks 600` prints a state hash and entity count. Compare before and after
   a change, or against a known-good value.
2. MCP `run_headless` with `inspect` reports the final values of chosen components (chapter 14).
3. A ctest in the project, like Kindle's `kindle_level`: link the game sources statically with
   `mce_add_test`, mount the cooked pack, install what the host installs, play the scene with
   scripted `InputState`s and assert on components. Read `projects/Kindle/tests/test_kindle.cpp`.
