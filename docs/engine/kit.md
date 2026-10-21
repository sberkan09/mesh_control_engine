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

`seats3d.h` (M-OPENWORLD C1): `VehicleSeat3D` (on a `Vehicle3D`: `seat`, `exit`, `enter_radius`,
`max_exit_speed`, `locked`, `driver`) and `Rider3D` (on a character: `use_input` for the Interact action, `enter`,
`seated`, `vehicle`, `camera`), `VehicleEntered3D` / `VehicleExited3D`. Boarding takes the rider's
`CharacterController3D` off (the settings are held in a resource and restored on exit), sets
`Vehicle3D.use_input`, and switches a `VehicleDriver3D` off; `"kit3d_seats"` (`FixedUpdate_Pre`, after
`"kit3d_vehicles"`) decides, `"kit3d_seat_pose"` (`FixedUpdate_Physics`, after `"kit3d_vehicle_state"`)
carries the rider. Tests: `tests/kit/test_seats3d.cpp`.

`combat3d.h` (M-OPENWORLD C2): `Health3D`, `Weapon3D` (hitscan rays or projectiles; Fire / Reload actions,
rate, spread from a component seed, magazine and reload), `Projectile3D` (stepped by the engine as a ray per
tick with gravity and splash), `apply_damage` for any other source; events `WeaponFired3D`, `WeaponHit3D`,
`Damage3D`, `Died3D` (producer `FixedUpdate_Pre`). A death sets `Ragdoll3D.active` with the hit's impulse
(`ragdoll_on_death`), a big hit staggers (`stagger_damage`). Systems `"kit3d_weapons"`, `"kit3d_projectiles"`,
`"kit3d_health"` after `"kit3d_ragdolls"`. Tests: `tests/kit/test_combat3d.cpp`.

`wanted3d.h` (M-OPENWORLD C3): `Wanted3D` (on the offender: `heat`, `level`, per-crime heat, witness rule,
cool-down) and `Police3D` (on a unit: `min_level`, `attack_level`, `PoliceState` Idle / Pursue / Attack / Search,
sight, `drive_nav` for its `NavAgent3D`, `aim` for its `Weapon3D`; blackboard keys `wanted`, `police_state`,
`target`, `last_known` on an `AiBrain`). Crimes come from `Damage3D` / `Died3D` against `civilian_team` /
`police_team` victims, or from `report_crime`; events `Crime3D`, `WantedChanged3D`. System `"kit3d_wanted"`
(`FixedUpdate_Pre`, after `"kit3d_health"`). Tests: `tests/kit/test_wanted3d.cpp`.

`roads.h` (M-OPENWORLD B1, ADR-0046): `RoadNetwork3D` places a cooked `.mcroads` (`asset::RoadView`) at its
entity (translation only) and runs its traffic lights on `time` (saved; `"roads_clock"`, FixedUpdate_Pre,
paused by `lights` 0). `RoadRegistry` (a resource) copies each network out of its chunk once (re-read on hot
reload; `define` registers one from a payload) with a 32 m grid of lane segments. Queries on a placed
`RoadNet` (`road_net`, `road_net_first`): `road_nearest` (flat distance, optional heading filter, connectors
opt-in), `road_lane_point`, `road_light` (Green, Amber, Red and the seconds left), `road_route` (A* over
lanes by travel time) and `road_debug_lines`. `install_stock` installs it before the vehicles. Tests:
`tests/kit/test_roads.cpp`; the cook side is `tests/asset/test_cook_roads.cpp`.

`traffic.h` (M-OPENWORLD B2, ADR-0046): `TrafficDriver3D` drives a `Vehicle3D` along a placed network's lanes
(`"traffic_drivers"`, FixedUpdate_Pre ahead of `"kit3d_vehicles"`): projection onto its lane, hashed turn
choice two lanes ahead, IDM speed against the car ahead, stop lines (red, amber, stop signs, conflicting
connectors by rank and arrival) and bends, pure pursuit steering. `TrafficSpawner3D` (`"traffic_spawners"`,
before the drivers) spawns cars from a prefab or the game's `TrafficSpawnHook` in a ring around a focus and
removes far, lost or flipped ones. All driver state is in the component (saved, hashed). A `Pedestrian3D` in `Cross` mode
within a car's lanes ahead (lane half width + 1.2 m, checked every 1.5 m up to its stopping distance + 15 m) stops it
3 m short.

`crowd.h` (M-OPENWORLD B3, ADR-0048): `CrowdSpawner3D` keeps up to `max_count` `Pedestrian3D` walking the pavements of
a `RoadNetwork3D` between `spawn_min` and `spawn_max` of a focus (an entity, else the first camera; with none the
crowd fills the network) and removes the ones past `despawn_radius`. A pedestrian is a few numbers (edge, distance
along, direction, hop target, own random stream), no behaviour tree: edges are the road centreline offset by half the
carriageway plus half `sidewalk`, trimmed at the junction; at a junction it hops to another edge end there, round
the corner or across a road, and a crossing waits until every road it crosses has no straight-on traffic with green or
amber and enough red left (`road_light`). Level of detail: step every tick within `near_radius`, every 4th within
`far_radius`, else every 16th, staggered by entity index. Spawn candidates are a function of the focus's 32 m cell and
`CrowdSpawner3D::serial` counts attempts, so replays and saves resume exactly. `"kit_crowd"` runs in FixedUpdate_Pre
after `"roads_clock"`; `crowd_stats` is the per-tick budget counters. Tests: `tests/kit/test_crowd.cpp`.

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

`cinematic.h` (M-CINEMA 4): `CameraRail3D` (Catmull-Rom dolly through up to 8 points (a scene writes all 8 entries); `rail_point`),
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
- ChunkLod3D (M-OPENWORLD A4, `mce/kit/chunk_lod3d.h`): `Kit3DExtract` draws the `proxy` of a `SceneChunk` entity
  whose `SceneChunkStatus` is not `Loaded` (the `impostor` beyond `impostor_distance`, nothing beyond
  `max_distance`), at the chunk entity's transform, counted in `Kit3DStats::chunk_proxies` / `chunk_impostors`.
  The distance is from the camera to the chunk box: a render decision, while loading stays focus-based.
  `bake_chunk_hlod` (`chunk_lod3d.cpp`) loads an area scene into a scratch world and merges its visible rigid
  `MeshRef`s with opaque or masked materials through `render::build_merged_mesh` (one submesh per material, each
  source's LOD levels kept), positions relative to the chunk entity; hidden, unloadable, skinned and non-opaque
  meshes count as `skipped`, those under `min_size` as `small`. `EditorSession::bake_hlod` writes the result as
  `.mchlod` (`cook::write_hlod_file`) and sets `ChunkLod3D.proxy` as one undo step. Its new sidecars carry
  `"lod_levels": 2`: the `hlod` importer then adds meshopt levels (`generate_lods`, as the model importers) to the
  submeshes that have none, leaving the sources' own levels and level 0 as baked. `collect_chunk_sources` is the
  gathering half (one callback with every source; a child's world matrix composes its `LocalTransform`s up the
  `ChildOf` chain when the scratch world has no `WorldTransform`), and gives each source a `group`: its root entity's
  ordinal. The impostor bake (`cook::bake_impostor`, `mce/cook/impostor.h`, M-OPENWORLD A3) takes those groups: a
  box (four sides and the top) or, for a group taller than `card_ratio` x its width and narrower than `card_size`, two
  crossed cards, each quad an orthographic albedo capture rasterised on the CPU (cooked BC/RGBA8 textures decoded,
  sRGB through one table, alpha-masked materials cut, uncovered texels dilated) packed into one atlas by shelves.
  `EditorSession::bake_hlod(settings, only, &impostor)` writes `chunk_<id>_far.png` (a `texture` sidecar) and
  `chunk_<id>_far.mchlod` and sets `ChunkLod3D.impostor` in the same undo step as the proxy.
- StaticMerge3D per frame (M-OPENWORLD A3): an unchanged group costs a walk over its children and a word-wise hash of
  their raw inputs (entity, mesh, material, position, rotation, scale, tints, shadow flag, mesh generation); the mesh
  views and material checks run only when that hash changes, counted in `Kit3DStats::merged_rebuilt`. The change
  plans the cells (children by cell, kept as entity and placement on the group); `r.merge_budget` /
  `set_merge_budget` then merges at most that many cells per frame, re-reading each child's MeshRef, mesh view and
  material when its cell merges, while the unmerged children draw on their own (`merge_pending`). A rebase while
  cells wait plans the group again. Per-entity
  arrays of the extract (`world_stamp_`, `worlds_`, `lod_states_`, `tracked_`, `merged_stamp_`) grow by half their
  capacity, not to `e.index + 1`: 24k entities arriving in one frame cost 16 ms instead of 2.7 s.
- Floating origin (M-OPENWORLD A2, `mce/ecs/origin.h`): `install_kit3d`, `install_bodies3d`,
  `install_nav3d` and `install_cinematic` register `ecs::on_origin_shift` listeners for the physics world,
  static instances, cached body poses, world-anchored joint ends, the navmesh offset and crowd targets,
  and root camera rails. Kit fields holding world points (`Locomotion3D::last_position`,
  `SequencePlayer::anchor`, `CameraFollow3D::pivot` and `last_goal`, `NavAgent3D::target` and
  `next_corner`, `Perceiver::last_seen` and `heard_at`, `Ragdoll3D::root_position`, `Raycast3D::point`)
  are flagged `WorldPoint`; `Blackboard` vectors are not. An absolute sequence `MoveTo` is a fixed-world
  target. `Kit3DExtract` notices a rebase through `WorldOrigin::serial`: it shifts its interpolation
  history, foliage and merged-mesh caches, re-uploads terrains, starts a camera cut and passes the shift
  to `Scene3D::set_origin_shift`.
- The renderer has no particle code; particles are sprite instances in the one sprite batch.
- Engine container ban applies (`scripts/ci/check_walls.py`).

## How it is tested

ctest `test_kit` (no GPU), `tests/kit/`:

- `test_chunk_lod3d.cpp`: "chunk lod: the proxy near, the impostor far, nothing past max_distance, hidden while
  loaded" (and kept while loading) and "chunk lod: a world without ChunkLod3D draws exactly as before".
  The bake is tested in `tests/editor/test_hlod_bake.cpp` (`test_editor`, "editor hlod bake*": counts, undo,
  byte-identical re-bake, `min_size`; the impostor's files, GUIDs and byte-identical re-bake). The capture itself is
  `tests/asset/test_cook_impostor.cpp` (`test_cook`: boxes and cards, colours in the atlas, every quad's window
  covered, blended sources left out, same bytes twice).
- `test_static_merge3d.cpp` also gates the city path by counts: "static merge: a 4096-piece city block reaches the
  scene as its cells and merges again only on a change" and "static merge: a merge budget spreads a block over
  frames, the waiting cells' children drawing on their own".
- `test_origin3d.cpp`: "origin3d: a world rebased mid-run matches the same run without the rebase" (a box
  stack, a character, a hinged door and a nav agent, rebased by cells {-4, 0, 3} after 31 ticks, then 600
  more ticks compared in absolute coordinates within 5 cm) and "origin3d: two rebased runs hash alike".

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

## Inventory, equipment and pickups (M-INVENTORY 2)

`mce/kit/inventory.h`, installed by `install_stock`. The items are a project's `.mcitems` catalog (`ItemCatalog`).

- `Inventory`: 24 slots of (`item`, `qty`), the catalog it indexes, an optional `max_weight_milli` (thousandths of a kilogram, 0: none).
  `item` is the catalog index plus one, 0 meaning empty. All arithmetic is whole numbers, so the same inputs give the same state hash.
- `Equipment`: one catalog index per `EquipSlot` (Head, Body, Hands, Feet, Weapon, Offhand, Accessory1, Accessory2). An item fits a
  slot when it carries the tag `equip.<slot>` (`equip.weapon`, `equip.head`, ...). Equipping moves one unit out of an inventory slot;
  a unit already in the slot goes back first, and nothing changes if it has nowhere to go.
- `ItemPickup3D` on an entity with a `Trigger3D`: whatever with an `Inventory` enters takes it; it is destroyed when all of it fits,
  otherwise `qty` shrinks to what is left.
- Rules as plain functions (`inventory_add`, `inventory_remove`, `inventory_count`, `inventory_move`, `inventory_split`,
  `equipment_equip`, `equipment_unequip`): add fills existing stacks and then empty slots and stops at the weight limit (an item that
  weighs nothing is never limited by weight); move onto an empty slot moves, onto the same item merges up to the stack, onto another
  item swaps; split moves part of a stack to the first empty slot.
- Events, processed in `FixedUpdate_Pre` (push them one tick before you need the result; they are swapped at the sync point):
  `GiveItem`, `TakeItem`, `MoveItem`, `SplitItem`, `EquipItem`, `UnequipItem`, `UseItem` in; `ItemAdded` (with the remainder that did
  not fit), `ItemRemoved`, `ItemEquipped`, `ItemUnequipped`, `ItemUsed` out. `UseItem` on an item tagged `consumable` takes one unit; if the item has a `heal` stat it also heals the user's `Health3D` (capped at `max`, `ItemHealed{owner, item, amount}` out), and a heal at full health or on a dead body is refused: nothing is consumed and `ItemUsed.consumed` is 0 (M-ITEMFX 1). Without a `Health3D` the item is consumed as before.
- Worn items change the wearer's `Health3D` (M-ITEMFX 2): the `armor` stats of the equipped items add to `Health3D.armor` (kept within 0..1) and their `max_health` stats to `Health3D.max` (never below 1; `current` is clamped down when `max` falls). Only the change is applied each tick and `Equipment.armor_applied` / `max_applied` remember what was added (not saved: `Health3D` is not in a save game either, so both load at the authored values and the bonus is applied again; a `Health3D` overwritten at run time loses the bonus until the equipment changes), so the authored base is never overwritten, a level-up that raises `max` stays on top, and unequipping gives back what was added (within float rounding). An entity with `Equipment` but no `Health3D` is left alone.
- Loot tables and chests: see the next section. Not yet: Luau and the `ItemGrid` widget (step 4), a sample (step 5).

## Loot tables, drops and chests (M-INVENTORY 3)

`mce/kit/loot.h`, installed by `install_stock` (it installs the inventory kit first). The tables are in the project's `.mcitems`
catalog: weighted entries, nested tables, guaranteed entries, quantity ranges and roll ranges.

- `roll_loot(catalog, table, Rng&, out)`: a pure function of the catalog, the table and the stream. A table does its guaranteed
  entries first, in entry order, then rolls `rolls_min..rolls_max` times, each roll picking one weighted entry (integer cumulative
  weights, a 64-bit draw; no float). An entry that names a table rolls that table (depth-limited). Equal items merge in first-seen
  order; a quantity saturates instead of wrapping. The same table and the same stream give the same list on every machine.
- `LootDrop` (on an entity with `Health3D`): when `Died3D` fires, rolls `table` once and spawns each result as an `ItemPickup3D` with
  a sphere `Trigger3D` (`radius`) on a fixed ring (`scatter` metres; a second ring beyond eight). `dropped` and `rolls` are saved.
- `Container` (a chest; give the entity an `Inventory` on the same catalog): the first `OpenContainer{container, opener}` rolls
  `table` into the chest's own `Inventory` (slots you pre-filled stay), then the opener takes as much as fits (stacks and weight
  cap apply). The rest stays; a later open takes more. `ContainerOpened{taken, left}` and an `ItemAdded` per move are sent. An opener
  on a different catalog than the chest moves nothing.
- Seeds: `seed` 0 derives from the table id and the entity index; a non-zero `seed` is used as given (set it for loot that must be
  the same on every run). The stream is `Rng::child(seed, "loot", rolls)`, so loading a save carries on with the next draw.
- Luau: the `inventory` global (`docs/user-guide/27-scripting.md`). UI: Text widgets named `slot_N`, `equip_<where>` and `inventory_weight` are filled by the stock player (`docs/user-guide/12-ui-and-console.md`).
- Saves: `Inventory`, `Equipment`, `Container` and `LootDrop` fields are `Save` fields of the ordinary save, which is versioned as a whole; a save without them loads with empty defaults, and an unknown field from a newer module is skipped. A save stores catalog item *numbers*, so a catalog may only grow at the end; renumbering items changes what an old save holds.
- Not yet: a sample (step 5).

## Crafting rules, events and stations (M-CRAFT 2)

`mce/kit/craft.h`, installed by `install_stock` (after the loot kit). The recipes are in the project's `.mcitems` catalog (see above).

- `craft_check(inv, catalog, recipe)` / `craft_apply(inv, catalog, recipe)`: pure functions of an `Inventory`. A craft is atomic: every
  input comes out and every output goes in, or nothing changes. The room for the outputs is checked *after* the inputs are taken out,
  against stacks, slots and the weight limit exactly as `inventory_add` does, so a full inventory can still make a recipe whose
  inputs free the space. Results (`CraftResult`, `craft_result_name`): `Ok`, `NoRecipe`, `MissingInput` (reported before room),
  `NoRoom`, `NoStation`, `StationBusy`, `NoInventory`.
- `CraftingStation` (on any entity): `station` is the tag it offers (`workbench`). A recipe with a `station` needs a `CraftingStation` of
  that tag; a recipe without one is made by hand.
- Events: `CraftItem{owner, recipe, station}` in (`station` null: by hand); `ItemCrafted{owner, recipe, station}` and
  `CraftFailed{owner, recipe, station, reason}` out. A recipe with time 0, or one made by hand, is applied in the tick the event is read.
  A timed recipe at a station is checked when it starts and queued (`recipe`, `left_us` on the station, one at a time); the inputs
  are taken and the outputs added when the time is up, as one atomic step, so the inventory stays usable meanwhile and a craft that
  no longer fits fails with its reason. `recipe`, `left_us` and `crafter` are saved, so a craft in progress survives a save. Destroying a station drops its queued craft without
  an event (nothing was taken); the station tag is checked again when the time is up. A recipe without a station tag is always made by hand, even when a station is named.
- System `kit_craft` (FixedUpdate_Pre, after `kit_loot`). Luau: `inventory.can_craft(e, recipe, station?)` and `inventory.craft(e, recipe, station?)` (`docs/user-guide/27-scripting.md`); `samples/12_loot` crafts a tonic by hand and a pack at a workbench.

## Timed status effects (M-STATUS 1)

`mce/kit/status.h`, installed by `install_stock` (after the crafting kit). A `StatusEffects` component holds up to 8 effects, each an id, a
kind, an amount and a time left in whole microseconds (the fixed step rounded to microseconds, so a run is the same every time).

- Kinds: `Regen` (hp/s, capped at `Health3D.max`, a dead entity is not healed), `Poison` (hp/s taken in half-second pulses through
  `apply_damage`, so armor, `Died3D`, ragdolls and `LootDrop` work as for any damage; the instigator is the applier), `Armor` (adds to
  `Health3D.armor`, kept within 0..1) and `MaxHealth` (adds to `Health3D.max`, never below 1, `current` clamped down when it falls).
  A poison pulse that has not completed when the effect ends is not dealt.
- Bonuses are applied as a remembered change (`applied`, not saved), like `Equipment`: the authored base is never overwritten, a
  level-up that raises `max` stays on top and the end of the effect gives back exactly what it added. `Health3D` is not in a save game
  either, so the bonus applies again on the first tick after a load.
- Events: `ApplyStatus{target, id, kind, amount, seconds, source}` and `ClearStatus{target, id}` (empty id: all) in; `StatusApplied`
  (`refreshed` when the id was active), `StatusEnded` (`expired` 1 or 0 when cleared) and `StatusRefused` (all 8 slots busy, or bad
  input: seconds outside 0.01 to 3600, a non-finite amount, an unknown kind) out. Applying an id that is active refreshes it (a refresh that changes the kind first gives back what the old kind added); nothing stacks. Regen and Poison need a positive amount, amounts are limited to ±1e6, clears are handled before applies in a tick (a cleanse then a re-apply ends with the effect on), and each effect remembers who applied it, so a poison kill credits the poisoner. A corrupt saved slot (non-finite amount, unknown kind) is dropped. A `Health3D` overwritten at run time loses the bonus kinds until they end: clear the effects when respawning.
- System `kit_status` (FixedUpdate_Pre, after `kit_craft`). Items and Luau: the next step.

### Items and Luau start effects (M-STATUS 2)

- A consumable with `regen` (hp/s) and `regen_seconds` starts a `Regen` effect named after the item; `armor_boost` and `boost_seconds` start an
  `Armor` effect named `<item>:armor`. The user needs `Health3D` (alive) and `StatusEffects`; without them an item that only carries effects is
  refused and not consumed, as a heal at full health is. An item that also has `heal` follows the heal rule. The effect starts a tick after the
  use (`ApplyStatus` is queued by `kit_inventory` and read by `kit_status`).
- Luau: `status.add(e, id, kind, amount, seconds, source?)`, `status.has(e, id)`, `status.clear(e, id?)` (`src/api_status.cpp`, typed in
  `definitions.cpp`, call table in `docs/user-guide/27-scripting.md`). Bad arguments raise an error that names the call.
