# kit — starter-kit gameplay components, particles, sequences, FSM helper and the 2D extract

`mce_gameplay_kit` is an L3 library in `src/engine/kit/` (headers under `include/mce/kit/`). Its
public dependencies are `mce_ecs`, `mce_refl`, `mce_asset`, `mce_render`, `mce_ecs_scene`,
`mce_audio` and `mce_physics2d` (`src/engine/kit/CMakeLists.txt`). It holds the reflected
components an editor-authored 2D scene is made of (FR-ENT-080) and their render extract, which is
the "extract lives with the gameplay kit" half of `mce_render`'s contract (`renderer.h`). Because
the components use `MCE_COMPONENT`, the cook, the scene loader, the editor inspector and the MCP
server see them with no per-type code.

It is linked by `mce_editor_core`, the `mce_engine` umbrella, `mce_player_runtime` (directly when
game modules are unsupported), `tools/cook` (which registers the engine components), the samples
`03_platformer` and `05_ashen_court`, and `test_kit` / `test_scene`. Game modules see
`mce/kit/components.h` through the `mce/gameplay.h` umbrella header.

## Public surface

`components.h` (namespace `mce::kit`): plain reflected structs. Units are metres, Y up; the entity's
`LocalTransform` (or `WorldTransform` when the hierarchy wrote one) places them.

- `Sprite`: textured or solid quad, optional `normal_map` for lit 2D, tint, size, pivot, `layer`
  (authoritative stacking) and `order`, flips.
- `TextLabel`: world-space text from a cooked font (`FixedString<64>`), height, alignment.
- `Camera2DComponent`: zoom, `pixels_per_meter`, clear colour, `primary`, `pixel_snap`.
- `Post2DComponent`: vignette, colour-grade LUT and chromatic aberration for the camera on the same
  entity (FR-R2D-100).
- `TilemapLayer`: one baked `.mctilemap` layer drawn from the entity's position (FR-R2D-115).
- `ParticleEmitter`: preset, seed, layer and order, `playing`, `destroy_when_done` (FR-R2D-070).
- `Light2D`: point or ambient light for sprites with a normal map (FR-R2D-090).
- `Parallax`: per-axis scroll factor against the camera and `repeat_x` (FR-R2D-050).
- `SequencePlayer`: plays a cooked `.mcseq` or a code-defined sequence by name; its progress fields
  carry the `Save` flag (FR-ENT-100).
- `Sprite.region`: the texture rect drawn (`[u0, v0, u1, v1]`, v0 top); `SpriteExtract` adds each
  distinct rect to the texture's sprite set once.
- `AnimatedSprite`: plays a `.mcsheet` clip onto the `Sprite` beside it (`sprite_anim.h`:
  `install_sprite_animation`, `"kit_sprite_animation"` in `FixedUpdate_Post`, `SpriteAnimEvent`
  for frame events) over `render/sprite_animation.h` (M-DATA-REACH 5).
- `Skeleton2D` and `Bone2DFollow` (`skeleton2d.h`, M-ANIM2D 1): a `.mcskel2d` skeleton
  (`ChunkType::Skeleton2D`, `Skeleton2DView`) posed from its rest pose plus the component's per-bone
  `pose` offsets (64 bones, `Save`), written to `world` by `"kit_skeleton2d"` in `FixedUpdate_Post`
  (`install_skeleton2d`, in `install_stock`). `SpriteExtract` draws the slots in slot order from the
  pure `skeleton2d_quads`; `Skeleton2DRegistry` copies skeletons out of their chunks (and takes
  `define`d payloads for tests and tools). `Bone2DFollow` writes a follower's `LocalTransform` from a
  named bone.
- `Skeleton2DAnimator` (`skeleton2d.h`, M-ANIM2D 2): plays the skeleton's animations (chunk minor 1:
  per-bone rotate / translate / scale timelines with linear, stepped or bezier keys, and events;
  `asset::skeleton2d_sample`) and cross-fades them. `"kit_skeleton2d_animators"` runs just before
  `"kit_skeleton2d"` in `FixedUpdate_Post` and writes, through `Skeleton2DBlend`, only the `pose`
  channels the sampled clips key; other channels keep gameplay's writes. An `.mcanim` graph runs
  through the same state machine step as the 3D `Animator` (`src/anim_graph_step.h`, templated on
  the component; layers, root motion and IK are 3D only). Clip and state events raise
  `AnimEvent3D`, which `install_skeleton2d` also registers. Every field is `Save`.
- Attachments, meshes and `Skeleton2DSlots` (`skeleton2d.h`, M-ANIM2D 3): chunk minor 2 adds
  per-slot attachment lists (quads, or meshes whose vertices sit in the slot bone's space or carry
  up to 4 bone weights), slot color and attachment timelines and mesh deform timelines. The pure
  `skeleton2d_draws` skins meshes on the CPU (weighted sums in listed order, no FMA) and returns
  quads and triangles in slot order; `SpriteExtract` sends the triangles through
  `render::SpriteBatch::add_triangle` (a degenerate quad in the sprite stream, so no backend work).
  The optional `Skeleton2DSlots` component (4.3 KB, `Save`) holds each slot's shown attachment and
  color; `"kit_skeleton2d"` sets up a fresh one, the animator adds one when the clips key slots and
  writes only keyed slots (`Skeleton2DBlend::write_slots`). Deform offsets are not stored: the
  extract samples them with `skeleton2d_animator_sample`, a pure function of the saved animator
  fields.
- 2D IK, `Skeleton2DIK` and `IKTarget2D` (`skeleton2d.h`, `src/skeleton2d_ik.cpp`, M-ANIM2D 4):
  chunk minor 3 adds up to 16 constraints (two-bone or CCD; a chain is its last bone and that
  bone's nearest ancestors) sorted by `order`, and `IKMix` timelines that key a constraint's mix.
  The pure `skeleton2d_solve_pose` poses the bones, then solves each constraint in the skeleton's
  space: it turns bones by aiming in their parent's space, writes local rotations back and
  recomputes the bones after them. Two-bone is analytic (law of cosines, a straight chain out of
  reach, no stretch); CCD runs a fixed pass count with no tolerance exit. `"kit_skeleton2d"` and
  `SpriteExtract` both call it, with the targets of `skeleton2d_ik_targets` (`IKTarget2D`
  entities taken into the skeleton's space) and the mixes of `skeleton2d_ik_state` (the optional
  `Skeleton2DIK`: keyed mixes plus a gameplay `override_mix`; before the animator's `Commands` add
  one, the keyed mixes sampled from the animator). Followers move after the solve.
- `Animator` graphs and `AttachToBone` (`anim3d.h`, M-DATA-REACH 7): an `.mcanim` AnimGraph
  (`ChunkType::AnimGraph`, `AnimGraphView`) drives the Animator's clip, 1D blends (`blend_clip`,
  `blend_weight`), transitions and `AnimEvent3D` events; one-shots finish (`loop`, `finished`,
  `restart`). Clip lengths and bone poses come from a device-less `render::MeshStore` copy of the
  mesh loaded through `install_kit3d`'s asset system. `kit3d_extract` poses up to three weighted
  samples (the playing clip, the blend's second clip, the clip fading out). A version 2 graph
  (M-GAMEPLAY3D 5) adds 2D blend spaces, layers (bone masks, additive clips; playheads in
  `layer_time`), root motion (`root_motion`, `root_velocity`, `root_turn`) and IK chains (two-bone,
  look-at, foot); `extended_pose` builds that pose in local TRS (`render::sample_local`,
  `compose_local`), and `"kit3d_ik"` gathers the chains' goals (`IKTarget3D`, rays down for feet)
  into the `AnimIK3D` resource every pose of the tick reads. `Locomotion3D` (M-MOVE3D 5, `"kit3d_locomotion"` before
  `"kit3d_animators"`) writes a mover's motion (controller, nav agent or position change) into its
  Animator's graph parameters by name (`speed`, `move_x`, `move_y`, `vertical_speed`, `grounded`,
  `airborne`, `crouch`) and sets the play rate from `stride_speed`.
- `PathAgent`: walks to `target` over the world's `NavGrid` at `speed`, re-planning every
  `repath_interval` ticks; `status` and the current smoothed path (16 waypoints) are runtime
  fields (#152).

`sprite_extract.h`:

- `SpriteExtract`: `init(renderer, assets)`, `extract(world, batch)` fills a `render::SpriteBatch`
  from the kit components and live particles in ECS iteration order; `sprite_set`, `font`,
  `post_2d`, `reset` (after remounting a pack). Textures load on first sight, one reference per GUID.
- `primary_camera(world, cam, ppm, clear)`: the first primary `Camera2DComponent` in ECS order.

`particles.h`:

- `ParticleSystem`: the CPU simulation resource. `advance(world, dt)`, `burst(entity, n)`,
  `draw(batch, resolve, user)`, `hash()`, `stats()`, `set_preset` for code-built presets.
- `install_particles(world, assets)`: registers `ParticleEmitter`, the resource and
  `"particles_advance"` in `FixedUpdate_Post` (after `"transform_propagate"` when installed).

`sequence.h`:

- `SequenceRegistry`: code-defined sequences (`define`), the named call table (`register_call`,
  `validate_calls`), and step factories (`wait`, `wait_event`, `move_to`, `anim`, `sound`,
  `stop_sound`, `emit`, `call_step`, `spawn`, `destroy`).
- Events `SeqSignal` (emit / wait_event) and `SeqAnimRequest` (for the game's animator).
- `install_sequences(world, assets)` and the system `sequence_runner` in `FixedUpdate_Post`.

`pathfinding.h` (#152):

- `NavGrid`: width x height cells of `cell_size` metres from `origin`, a `u8` cost per cell (0 =
  blocked); `cell_at`, `cell_center`, `mark_aabb`, `hash`.
- `nav_grid_from_tilemap(view, origin, solid_tiles)` and `mark_static_colliders(grid, world, inflate)`.
- `Pathfinder`: integer A* (`find_cells`, `find_path`), 4/8-neighbour without corner cutting,
  octile heuristic, total-order tie-break; `smooth` (string pulling) and `line_of_sight`.
- `install_path_agents(world[, grid])`: `PathScratch` resource and `"path_agents"` in
  `FixedUpdate_Pre`; moves a physics2d body by velocity or the `LocalTransform` directly.
- `debug_draw_grid`, `debug_draw_agents` into a `physics::DebugDrawSink`.

`procgen.h` (M-PROCGEN 1): pure, integer-only level generators, a function of (parameters, seed) alone.

- `Rng`: a splitmix64 stream; `Rng::child(seed, "name", index)` gives each step of a generator its own
  stream, so adding a step never shifts another's numbers.
- `generate_rooms` (BSP rooms and L-shaped corridors, 1 or 2 wide), `generate_caves` (cellular automata;
  needs about 16 x 16 or more), `generate_walk` (a drunkard's walk), `generate_noise` (thresholded value
  noise, fixed point). Each returns a `ProcGrid` (`kWall` / `kFloor`, row 0 on top) with a wall border, one
  4-connected floor region, a `start` and the farthest `exit`; a draw that misses the floor target is
  retried on the next attempt's stream (at most 16), then the call fails with a stage naming the generator.
- `WfcRules` + `generate_wfc` (M-PROCGEN 2): wave function collapse on a 64-bit option mask per cell.
  Rules by `add_pair(a, dir, b)` or learned from an example (`from_example`: neighbouring pairs allowed,
  weights = how often a tile occurs; at most 64 tiles, ids kept in ascending order). The fewest-options cell
  is collapsed by weight from the `"wfc"` stream and the neighbours are narrowed; a contradiction restarts on
  the next attempt's stream (`max_restarts`, default 32), then the call fails with a named stage. `WfcPin`
  cells are set first; pins that disagree with each other or the rules fail at once.
- `ProcGrid::hash`, `floor_count`, `to_ascii`. Tests pin one hash per generator: a generator change
  changes every saved seed, so those lines move only on purpose.

`stock.h` (M-DATA-REACH): `install_stock(world, assets)` installs everything below that the stock
player runs (particles, sequences, kit3d, `WorldSettings`, effects), one call shared by `mce_player`,
play-in-editor and MCP `run_headless`; `install_world_settings` applies the first `WorldSettings`
(`gravity_2d`, `gravity_3d`, in `components.h`) at Startup.

`bodies3d.h` (M-DATA-REACH 6): `RigidBody3D` (dynamic/kinematic; box, sphere, capsule, convex hull
of a mesh), `CharacterController3D` (capsule walk by `move` or the Move/Jump input; M-MOVE3D 1 feel:
acceleration, coyote time, jump buffer, jump cut, air jumps, sprint, crouch via
`World3D::set_character_height`, `Landed3D` events, `FireOn::Landed`), `Trigger3D`
(box/sphere overlap with `TriggerEnter3D` / `TriggerExit3D`); `install_bodies3d`: `"kit3d_bodies"`
(`FixedUpdate_Pre`: create, teleport on outside moves, velocities, characters, runtime and moved
static colliders), `"kit3d_writeback"` and `"kit3d_triggers"` after `"kit3d_physics"`; remove hooks
destroy the bodies.

`vehicles3d.h`: `Vehicle3D` (Jolt wheeled car on a `RigidBody3D`; M-MOVE3D 3 feel: `anti_roll` bars,
`downforce`, `gears` / `first_gear` / `top_gear` and shift rpm as data, `skid_slip` with `Skid3D`
events and `FireOn::Skid`, `self_right` after `right_after` seconds flipped); `Waypoint3D` and
`VehicleDriver3D` (M-MOVE3D 4: pure-pursuit AI driver with corner braking and laps, `LapCompleted3D`,
`FireOn::Lap`), run inside `"kit3d_vehicles"` before the car takes its inputs.

`sensors3d.h` (M-GAMEPLAY3D 6): `Raycast3D` (a ray, or a sphere cast with `radius`, from the entity:
`hit`, `point`, `normal`, `hit_distance`, `hits`) and `Overlap3D` (a sphere or a turned box at the
entity: `count`, `enters`, `last_other`), both pushing `TriggerEnter3D` / `TriggerExit3D` with the
sensor as `trigger`, so `PhysicsEvents` and `FireOn::TriggerEnter` see them. A sensor skips its own
entity and its `ChildOf` ancestors; switching it off, removing it or destroying it lets everything
out. `install_sensors3d` (from `install_stock`, after `install_bodies3d`): `"kit3d_sensors"` after
`"kit3d_triggers"`. Tests: `tests/kit/test_sensors3d.cpp`.

`effects.h` (M-DATA-REACH 4): `FireOn` (`Manual` via the `fire` flag, `Start`, `TriggerEnter`,
`ContactBegin`); `SfxTrigger` (a `PlaySfx` with dB jitter, `audio::AudioBus`), `ParticleBurst`,
`ShakeTrigger`, `CameraShake2D` (added to the view by `primary_camera`), `CameraFollow2D` (offset,
dead zone, smoothing, look-ahead, bounds), `CameraFollow3D` (orbit with yaw/pitch, Look-input
orbit; M-MOVE3D 2: a spring arm over `World3D::camera_sweep`, look-ahead, first person, lock-on), `PhysicsEvents` (engine-written last 2D trigger/contact pairs and counts), `NavGridSource`
(the `NavGrid` from a tilemap layer and/or static colliders; automatic from colliders when absent).
`install_effects(world, assets)`: `"kit_nav_grid"` (`FixedUpdate_Pre`, before `"path_agents"`,
which it installs), `"kit_effects"` and `"kit_camera_follow"` (`FixedUpdate_Post`).

`cinematic.h` (M-CINEMA 4): `CameraRail3D` (Catmull-Rom dolly through up to 8 points; `rail_point`),
`CameraLookAt3D`, `CameraShake3D` (fed by `ShakeTrigger`, advanced in `"kit_effects"`), `CameraBlend3D`
(the extract's `view_of` blends from `from`'s view; a timeline camera cut with a blend writes it) and
`Cutscene` (next to a `TimelinePlayer`: the world's cutscene hold, letterbox, hold-to-skip through
`timeline_skip`; `cutscene_view` for the player's overlay). `install_cinematic(world)`: `"kit_cutscene"`
(`FixedUpdate_Pre`, last in install_stock) and `"kit_cinematic"` (`FixedUpdate_Post`, after the
timelines and follow cameras), both `run_during_cutscenes`. A cutscene hold (`World::set_cutscene_held`)
runs only the systems so marked: timelines, sequences, animators, attach, sprite animation, particles,
effects, captions, follow cameras, transforms and the audio bridge.

`video.h` (M-CINEMA 5, ADR-0043): `VideoPlayer` (an `AssetRef<Video>`, the playhead `time`, `speed`,
`playing`, `loop`, `restart`, `finished`, `fullscreen`, `sound`, `gain_db`) and `VideoRegistry` (the cooked
videos' lengths and sound clips, the sound voices). `install_video(world, assets)`: `"kit_video"`
(`FixedUpdate_Post`, before `"timeline_player"` so a video track's playhead wins the tick,
`run_during_cutscenes`) advances the playhead and keeps one mixer voice per playing player in step with it.
Render side, `VideoTextures`: one `render::VideoDecoder` and one RGBA8 texture per shown player, stepped to
its `time` each frame; `SpriteExtract::set_videos` draws a `VideoPlayer`'s `Sprite` with its film, and
`fullscreen()` gives the stock player the splash to draw.

`fsm.h` (header-only, namespace `mce::fsm`):

- `Trans<S>` rows (`from`, `to`, guard `when` or timed `after`, `entry`, `exit` actions),
  `validate(table, initial)`, `step(...)` for one entity, `run<C>(frame, table)` over every entity
  with component `C`, optional `StateChanged<S>` event.

## Building a 2D game on the kit (what M9 needed)

The kit draws; it has no gameplay. A scene-authored platformer (UC-D's `projects/Kindle`, the SDK's
`share/mce/examples/Kindle`) puts these
together:

- **Screen-space HUD.** There is no HUD component: a `TextLabel` with `Parallax {factor: [0, 0]}`
  stays fixed to the camera. Author it where it should sit relative to the camera centre (for a
  1280×720 window at 48 px/m the view is ±13.3 m by ±7.5 m) and give it a high `layer`. The game
  writes `TextLabel::text` (a `FixedString<64>`) to show live values.
- **Backdrops.** `Sprite` + `Parallax` with `repeat_x`; a `factor.y` of 0 pins the layer vertically
  to the camera, which suits a camera that only scrolls sideways.
- **Frame animation.** `Sprite` shows a whole texture, so animate by assigning another texture:
  `sprite.texture = asset::AssetRef<asset::Texture>{guid, {}}`. The extract resolves a GUID on first
  sight and keeps it, so swapping between a few frames costs nothing after the first draw. Keep the
  frame GUIDs as `AssetRef<Texture>` fields on a game component so the editor sets them.
- **Tile art.** A sprite stretches its texture to `size`, so a long platform drawn as one sprite
  smears a 16×16 tile. Use one `Sprite` per tile (separate from the collider entity) or a
  `TilemapLayer`; a `.mctilemap` is painted in the editor's Tilemap panel, which the MCP server does
  not expose.
- **Fading.** `Sprite::tint.a` fades a sprite (Kindle's inactive blocks draw at alpha 0.22).
- **Not in the kit:** a player controller, pickups, hazards, a goal, a follow camera, crumbling or
  moving platforms. Ember Step's versions live in `samples/03_platformer/game/`, are not reflected,
  and are spawned from code, so a new game ports them into its own module as `MCE_COMPONENT`s
  (Kindle's `components.h` is a worked example of each).

## Rules and invariants

- Determinism: `SpriteExtract` draws in ECS iteration order (FR-RUN-040). Particles run on the fixed
  tick, each emitter owns a PCG stream seeded from `seed` and the entity index, and
  `ParticleSystem::hash()` covers every live particle so replays can check it next to
  `World::state_hash`. Particles live in world space and die in swap-remove order.
- Sequence timing is tick math (ADR-0032 §3): a waiting step owns its tick and the next step starts
  on the next tick; a 64-step per-tick budget (`kStepBudget`) stops runaway loops. A loop with no
  waiting step is rejected by the cook and by `define`.
- Sequence steps reach other systems only through existing paths: `World2D::set_linear_velocity`
  or `LocalTransform` for `move_to`, `audio::PlaySfx` for `sound`, removing `AudioSource` for
  `stop_sound`, `ecs::instantiate` through Commands for `spawn`.
- FSM semantics: `state_time` advances first, then rows for the current state are checked in table
  order; at most one transition per entity per tick; exit then entry, exactly once each. Guards and
  actions are function pointers and never serialized; `state` and `state_time` are component
  fields. Guards run mid-query and must not change structure directly.
- Missing or unloadable textures draw as the magenta checker (FR-DX-050); a null texture GUID draws
  a solid quad.
- Component layouts carry explicit `pad_` fields so hashed component bytes stay defined.
- The renderer has no particle code; particles are sprite instances in the one sprite batch.
- Engine container ban applies (`scripts/ci/check_walls.py`).

## How it is tested

ctest `test_kit` (no GPU), `tests/kit/`:

- `test_fsm.cpp`: "fsm: a timed transition fires on its exact tick and resets state time", exit and
  entry order, `StateChanged`, validation, "fsm: state and state time survive a save mid-state".
- `test_particles.cpp`: burst, rate, duration, cap, integration, determinism per seed, one-shots,
  the hierarchy, MDI grouping, "FR-R2D-070: a 2,000-particle burst simulates and batches within
  budget", `.mcpart` cook and errors.
- `test_sequence.cpp`: wait timing, `move_to` on plain entities and kinematic bodies, emit and
  wait_event, the call table, loops, "sequence: save mid-step, load into a fresh world, finish on the
  same tick in the same state", `.mcseq` cook and errors.
- `test_pathfinding.cpp`: paths around walls, blocked and walled-in goals, no corner cutting,
  smoothing, "pathfinding: same inputs, same path; 100 agents hash identically after N ticks", a
  grid from a cooked `.mctilemap`, static colliders, agents on transforms and kinematic bodies.

ctest `test_kit_gl` (label `gpu`, registered only with `MCE_GPU_TESTS`; CI uses Xvfb and Mesa
llvmpipe): parallax factors and `repeat_x`, particles on screen, Light2D with a cooked normal map,
`Post2DComponent` to renderer settings.

`test_scene` also checks `Sprite` and `primary_camera` through a cooked scene. The `extras_2d`
sample adds `extras_2d_player_600` and the GPU run `extras_2d_player_gpu`.

## Design records

- [ECS and gameplay architecture](../architecture/ecs-gameplay.md) §2.9 starter kit.
- [Scripting architecture](../architecture/scripting.md) §2.5 FSM helper, §2.6 sequences.
- ADR-0024 The runtime scene path (`docs/adr/0024-runtime-scene-path.md`) decision 4.
- ADR-0031 M6 2D extras (`docs/adr/0031-m6-2d-extras.md`): particles, parallax, lit 2D, 2D post.
- ADR-0032 SequenceRunner (`docs/adr/0032-sequence-runner.md`).
- M6 2D extras (`docs/milestones/M6-2d.md`) and M6 gameplay (`docs/milestones/M6-gameplay.md`).

## Known gaps

- `Sprite` shows its whole texture; atlas regions and animation wait for the sprite-sheet importer
  (comment on `Sprite`, ADR-0024 decision 4).
- Not built (ADR-0032): the editor sequence live view. `TimelinePlayer` and `.mctimeline`
  (`kit/timeline.h`, FR-ENT-110) landed in M-CINEMA step 1, the editor timeline panel in step 2 and
  cutscenes (`kit/cinematic.h`) in step 4.
- The lit-2D GPU cost at 10k sprites and the 2D post cost at 1080p are open until measured on a
  hardware GPU (M6 2D budget table).
- Pathfinding is grid A* only: no navmesh, no jump-point search, no agent avoidance; the grid and
  path debug draw goes to a `physics::DebugDrawSink`, which nothing wires to the editor viewport
  yet (#152).
- No gameplay starter components (controller, pickups; the follow camera and effects are in `effects.h`); see "Building a 2D game on
  the kit" above (M9).
