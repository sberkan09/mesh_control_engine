# M-OPENWORLD gaps — what `samples/09_openworld` still lacks (play review)

**Status:** review record, 2026-10-29 (Sami: "the game is far too incomplete, play it in the cloud, find the gaps, document them").
**Source of evidence:** the sample built on Linux (`linux-gcc-release`, Mesa llvmpipe, Xvfb), played through `mce_player` with
`-input` scripts and `-capture_ticks` screenshots (1300 ticks, 0 GL errors), plus the scene and `gen_assets.py`
(`samples/09_openworld`), and the kit headers. On the owner's Mac (M4, macOS 26.6.2) the same build opens on Metal
(120 frames, no shader or Metal error). Frame rate on a real GPU is not measured here. Interactive feel (mouse look,
shooting) was not hands-on tested in the cloud: scripted input only.

**Short verdict.** `09_openworld` is an *engine-feature integration slice*, not yet a game. It wires roads, traffic, a crowd,
one drivable car, one weapon and an on-foot police force. It wires **none** of the other open-world pieces the kit already
ships and that have their own demo samples. Most gaps below are therefore "wire what exists", not "build new systems".

## A. Kit features that exist but are not in the 09 scene

The scene holds only: Environment3D, RoadNetwork3D, box colliders, `MeshRef`s, 5 `Health3D`/`Weapon3D`/`CharacterController3D`,
4 `Police3D` + `NavAgent3D`, 3 `Vehicle3D` + `VehicleSeat3D`, `Wanted3D`, `TrafficSpawner3D`, `CrowdSpawner3D`, a camera.
No UI document, no audio source, no clock, no weather, no mission, no marker, no respawn point.

| # | Missing in the sample | Exists as | Demo elsewhere |
|---|---|---|---|
| A1 | **HUD** (E2 done in the HUD PR: health, wanted level, cash, ammo, objective hint; wanted stars done (Panels bound to `Wanted3D/level`, visible from `range` lo up); dynamic mission HUD done too (one `mission_<id>.mcui` per Mission entity: name, stops, time bar, passed/failed banners)): no health bar, ammo, wanted stars or any on-screen text. The player cannot tell the wanted level, health or that a crime was counted | `.mcui` documents, `Wanted3D`/`Health3D` events | `samples/10_missions/assets/ui/hud.mcui` |
| A2 | **Minimap and map markers** (E2: minimap with car and police markers wired; mission and shop markers wired in E7/E8) | `MapMarker`, `Minimap` widget (M-MISSION step 2) | `10_missions` |
| A3 | ~~**Missions**~~ **done (E7, 2026-10-29)**: three jobs (Errand, Courier, Long haul) started from marker pads, reward, fail fee and retry, wanted-level fail; minimap labels via `MapMarker`. Still open: kill/interact objectives, Luau chaining | M-MISSION, M-MISSIONSTART/-PAY/-RETRY | `10_missions` |
| A4 | **Day/night cycle**: ~~the sun is fixed~~ **done (E6, 2026-10-29)**, see [M-OPENWORLD-DAYNIGHT](M-OPENWORLD-DAYNIGHT.md) | `WorldClock`, day cycle, moon (M-DAYNIGHT) | `11_daynight` |
| A5 | **Weather**: ~~rain, wet roads, fog, lightning, wind~~ **done (E6)** except snow (no snow cover in the city) | M-WEATHER, M-WEATHERAUDIO | `12_weather` |
| A6 | **Audio** (done 2026-10-29, E3: engine by speed, shots, footsteps, sirens, city bed; shots land and cars crash with sounds; still missing: radio, music) | `mce_audio`, M-AUDIO2, vehicle/weapon audio events | other samples |
| A7 | **Busted and wasted**: ~~dying or being arrested ends the game for good~~ **done (E4, 2026-10-29)**: hospital and station respawn | `Respawn3D`, `RespawnPoint3D` (M-WASTED) | none |
| A8 | **Arrest and reinforcements** | `Police3D` arrest, dispatch (M-ARREST 1-2) | none |
| A9 | **Police cars and roadblocks**: ~~all four police are on foot~~ **done (E4)**: 4 pursuit cars, 2 roadblocks | `drive_car` on `Police3D` (M-ARREST 3) | none |
| A10 | **Carjacking** done (E8, parked cars 2 and 3 are owned, witnessed theft adds heat); vehicle damage, wrecks and ramming pedestrians done ([M-CARWRECK](M-CARWRECK.md) step 2); still open: fire/smoke on a wreck, repair | M-CARJACK, M-CARWRECK, `vehicle_ram.h`, M-VEHICLEHIT | none |
| A11 | **Shops and money** done (E8): 12 cash pads, a medic, a scrapyard that buys a stolen car; the ammo shop came with the gun-on-player change; loot and a bag done ([M-LOOTVIS](M-LOOTVIS.md): walkers drop bandages and medkits, H uses one) | `loot.h`, `inventory.h`, M-AMMOSHOP/-HEALSHOP/-CHOPSHOP | `12_loot` |
| A12 | **Pause menu, options, save/load** ~~in the sample~~ **done (2026-10-31)**, see [M-OWPAUSE](M-OWPAUSE.md) | `ui_flow`, save system | `ui_screens` |
| A13 | **HLOD / `ChunkLod3D` skyline and scene streaming**: the whole city is one scene (714 entities) | A3/A4 steps, `samples/hlod_city` | `hlod_city` |
| A14 | **Floating origin**: not set (1 km² never reaches the threshold); matters if the map grows | `FloatingOrigin3D` | — |

## B. Content and look (observed in the captures)

- ~~**Roads, ground, kerbs and trees are tinted boxes** (one `box.glb`).~~ **Done (E7, 2026-10-29), see [M-OPENWORLD-LOOK](M-OPENWORLD-LOOK.md).** Was: No road markings, lane lines, crossings, junction
  meshes or sidewalk detail; the road network is data only, so the visual road is a flat slab.
- **Washed-out colours.** The grass tint (0.20, 0.34, 0.14) shows as a pastel green (about 186, 214, 164) and the kerb or
  sidewalk strip reads near white. Suspect the fixed sun (intensity 3.0, exposure 1.0) and no grading; cause not confirmed.
  Check it on Metal as well (same shaders translated to MSL) before tuning.
- **Characters and cars are blocky/toy Kenney models with no animation.** Characters slide: the node clips of the blocky
  rig are not played; wheels do not spin; no door, steering or lights animation.
- **No sky**: no HDRI, no clouds, no stars; no night look; traffic lights do not change colour with the signal state.
- **No third-person feel**: the player does not face its movement direction (the engine has no facing component for
  `use_input` characters), the camera stays behind a fixed orbit (the gun is the player's since step 2a and aims along the camera).
- **Visible feedback on shooting**: ~~no muzzle flash~~ **muzzle flash and a small view kick done ([M-SHOTFX](M-SHOTFX.md))**; still no tracer, impact decal or hit marker (`WeaponFired3D`/`WeaponHit3D` carry the data).
- **Sparse city**: 121 box lots, no interiors, no props (lamps, hydrants, bins, benches, signs), no vegetation beyond boxes.
- ~~**Traffic and crowd were not seen in any of the 11 captures**~~ **Fixed (SBA-213, PR #859).** The cars did spawn and
  drive (24 alive within 10 s); two things hid them and the crowd. (1) The orbit camera started at yaw 0, hung over the
  avenue and looked north into a block, so no road was ever in frame; it now starts at yaw π/2, looking west along the
  avenue. (2) The walkers carry a kinematic capsule, and `kit3d_bodies` syncs bodies before `kit_crowd` steps, so the
  physics write-back put each walker back on its spawn point every tick (its `s` ran on, its transform did not), half
  under the pavement. The crowd now runs explicitly after `kit3d_bodies`, stands a walker with a body on its capsule and
  drives the body by velocity (far walkers glide between steps); a walker turns round at a parked car instead of shoving
  it. Also: `mce_player`'s headless line "714 entities" counts the scene's own entities, not the spawned ones, so it never
  showed the traffic either (not changed). Verified on captures at ticks 300 and 700 from the start position and during
  the drive script (cars on both carriageways, walkers moving on both sidewalks).
- **The camera does not turn with the car** (done, M-OPENWORLD-POLICE 4: `CameraFollow3D::chase`, on in the sample; found in SBA-213). `CameraFollow3D` keeps the yaw the player gave it;
  while driving, the car turns under a fixed camera, so a turn off the avenue leaves the road out of frame. Needs a chase
  option on `CameraFollow3D` (ease the yaw behind the target's heading while it moves, Look input overriding it for a
  moment), used by the sample while `Rider3D` is seated. A separate step.
- ~~**Walkers with a body: two open edges**~~ **Fixed (SBA-214).** (1) A walker mid-hop at a junction that meets a dynamic
  body walks the hop back to the end it left, as one on a pavement turns round, instead of waiting there for good. (2) The
  blocked test sweeps the step (sphere casts low, middle and high on the capsule) as well as testing its end, so a far
  walker's 16-tick step no longer passes through a thin body.

## C. Gameplay depth

- **Player stuck behaviour.** Sprinting into a building face stops the player with no slide or feedback; the sample has no
  fall-off handling; ~~the world edge has no wall~~ **four invisible edge walls added (M-WORLDEDGE)** on the 1120 m slab's rim.
- **No cover, melee, grenades or weapon switching.** One hitscan gun (`Weapon3D`), reload, no ammo pickups.
- **Pedestrians did not visibly react** to gunfire or cars: the kit has M-PEDFLEE (hurt, shot nearby, panic spreads) and
  M-PEDCRASH (crash, car bearing down) and the walkers did switch to `Flee`, but they were frozen in place (B, SBA-213), so
  nobody ran. With the fix they run. Still missing: no reaction to the player walking into them, no phone calls to police,
  crowds do not avoid each other.
- **Traffic**: no lane changes, no overtaking, no horns, no reaction to a car crash; a car stopped for a still pedestrian waits.
- **Police**: the navmesh covers only the middle blocks, so units stay there; no police cars (A9), no helicopters, no
  roadblocks; wanted level has no on-screen indicator (A1) and no flashing sirens.
- **Driving**: one car type for the player, no radio, no gears display, no lights toggle, no horn, no damage model on the
  drive path in the sample.
- **No progression or goal**: nothing to do after the 3 car and police interactions; no economy, no story.
- **No gamepad or touch walk-through** of the openworld input map was tried (the map has gamepad bindings).

## D. Verification and tooling gaps

- No **budget counters** for the sample in `local_ci.sh` (entities, draws, frame time); only the headless replay test and the
  MCP behaviour test. Frame time on a real GPU is open (A1 in M-OPENWORLD, GPU thread).
- No **screenshot gate** (golden or perceptual) for the sample, so a lighting or tint regression is invisible to CI.
- No **hands-on play log**. A scripted playthrough that sprints, enters the car, drives, shoots and gets arrested, with
  per-step assertions, would catch most of section C.
- `M-OPENWORLD.md` "Open items" still lists "a dead pedestrian vanishes after 0.2 s": `crowd.cpp` already removes a dead
  pedestrian after `kPedestrianCorpseSeconds`, so that line is stale (left unedited here).
- Not verified on the Mac: sound, gamepad, frame rate at 2560 x 1440 and the 10-minute soak for this sample (M-MAC's soak
  covers Ember Step).

## E. Suggested order (smallest steps that make it feel like a game)

1. **E2 HUD + wanted stars + health + ammo** (A1): copy `10_missions` `hud.mcui`; one `.mcui`, no code. Biggest feel gain.
2. **E3 audio pass** (A6): done, see [M-OPENWORLD-AUDIO](M-OPENWORLD-AUDIO.md). Engine loop, shot, footsteps, siren, ambience; hit sounds and radio remain.
3. **E4 busted and wasted + police cars + arrest** (A7-A9; respawn, cars and roadblocks done, see [M-OPENWORLD-POLICE](M-OPENWORLD-POLICE.md)): data only, uses M-WASTED and M-ARREST components.
4. **E5 look pass**: roads with markings and crossings (done, E7: [M-OPENWORLD-LOOK](M-OPENWORLD-LOOK.md)), a sky HDRI, tint/exposure check on GL and Metal, shot effects.
5. **E6 day/night + weather** (A4, A5), **E7 missions + minimap** (A2, A3), **E8 shops, carjack, wrecks** (A10, A11).
6. **E9 animation**: play the node clips, wheel spin, door and seat transitions.
7. **E10 gates**: screenshot gate, budget counters, scripted playthrough test, Mac soak.
8. **E11 scale**: HLOD skyline, scene streaming, floating origin when the map outgrows 1 km² (A13, A14).

Steps 1 to 4 are the "make it playable" bundle; 5 to 8 make it a game; 9 to 11 are polish and scale. Each is a data-only or
single-module change, so they can run in parallel threads on disjoint files (`samples/09_openworld` plus one module each).

## F. Weapons and combat (step 2)

Step 2 closes "no cover, melee, grenades or weapon switching" and moves the gun to where the shop and confiscation components look.

| Step | What | Status |
|---|---|---|
| 2a | `Weapon3D::aim_from` (an Entity): the shot leaves from that entity's pose (plus `muzzle`) and points along its forward; null or a gone entity falls back to the weapon's own pose. The 09_openworld gun moved from the camera to the player (`aim_from` = the camera, `WeaponAudio` and the ammo readout in `hud.mcui` with it), so `Confiscate3D` (an arrest keeps half the spare rounds) and an `AmmoShop3D` on the avenue (2 cash a round, up to 150) now work. `Weapon3D` 168 to 176 bytes. | done |
| 2b | `Loadout3D` weapon switching | open |
| 2c | `Melee3D` | open |
| 2d | `Cover3D` / take cover (ray from the shooter to the target against the cover) | open |

## G. Mission objectives: kill and interact in 09_openworld

Closes the A3 remainder ("kill/interact objectives"). Today `Kill` counts every `Died3D` when `target` and `tag` are empty
(a run-over walker or a wrecked traffic car counts), counts only the target's death once `target`/`tag` resolves, and never
completes when the named target died or was destroyed before the objective went active (the event is gone, and `find_entity`
skips dead entities). `Interact` already works: the `Interact` action (E, F, pad East) pressed within `radius` of the target.

**Kit change (one step, `missions.h`/`missions.cpp`, no new component).** `Objective.pad_` (u16) becomes two u8 fields, so
`sizeof(Objective)` is unchanged (add `static_assert(sizeof(Objective) == <current>)` next to the struct; no padding added):
- `u8 team = 0`: kill only; 0 = any. A `Died3D` counts only when the dead entity's `Health3D.team` equals it (the dead entity
  still has its `Health3D` that tick: `kit_missions` runs after `kit3d_health`, before `destroy_on_death`'s `linger`).
- `u8 by_actor = 0`: kill only; 1 = the `Died3D.killer` must be `Mission.actor` (the gun's instigator is its owner, a ram's is
  the driver, so shooting and running down both count).
- Rule: a `Died3D` counts when (no target and no tag, or `d.entity == target`) and the team and killer filters pass. A tag
  still names *one* entity (scene names are not kinds; a "kind" is a team). Fix: a `Kill` with a resolved `target` that is
  already dead or gone (`dead(w, target)`) is done, so a target killed early or destroyed still completes the step; an
  unresolved tag keeps waiting as today (documented).
- Both fields reflected with `Save` and `Help`; defaults 0 keep every existing scene and its state hash byte-identical. No new
  component, so no sample_coverage or component-count change; `capabilities.md` (new field rows) and any golden that lists
  `Objective` fields are regenerated by the integration runner. Saves are versioned as a whole and fields load by name: an
  old save reads 0, i.e. today's behaviour, so no save version bump.

**Data only (`gen_assets.py`, `economy_and_missions`).** `mission()` takes stops as `(kind, x, z, extra)` so a step can be a
pad (ReachZone, Interact) or an entity (Kill); markers and stops stay on the avenue sidewalks (z = +-9), MapMarker per stop.
- *Hit* (marker (20, -9)): Kill the named NPC "Hit target" (new scene entity at (165, 9): `CharacterController3D`,
  `Health3D{max 60, team 4, destroy_on_death 0}`, MapMarker), then ReachZone a pad at (-150, 9). 180 s, 900 cash, fee 150,
  no wanted fail (the shot raises it by design), retry 8 s.
- *Cop killer* (marker (-80, 9)): Kill `count 3, team 2, by_actor 1`, then ReachZone (200, -9). 240 s, 1200, fee 200.
- *Pickup* (marker (-20, -9)): Interact a "Package" pad at (-130, -9) (radius 2.5), then ReachZone (100, 9). 150 s, 400,
  fee 50, `MissionWantedFail{3}`. The package pad stays > 6 m from a parked car (E also enters a car within 4 m).
- `fail_on_death` = the player for all three, as today; the HUD document per mission names the steps ("Kill", "Pick up").

**Tests.** `tests/kit/test_missions.cpp`: team filter (a team-3 death does not count, two team-2 deaths complete count 2);
`by_actor` (a kill by another entity does not count); a target dead before its step is done on the first tick of the step;
the same run twice has the same state hash. `tests/mcp/test_mcp_openworld.cpp`: Mission count 3 -> 6; Hit: start it, set
"Hit target" `Health3D.current` 0, stand on the drop pad -> Complete; Pickup: stand at the package, press Interact
(`-input` script or `InputState` via MCP) -> the second step active.

**Known limits.** Team 2 includes police cars (a destroyed car counts as an officer). Kill progress ("2/3") is on the
Objective entity, not shown on the mission HUD until it can bind another entity. No "lose the cops" objective (a `Signal`
from a future wanted-cleared event would do it). Interact shares E with entering cars. One named target per Kill step.

Interface: `Objective { ...; u8 team = 0; u8 by_actor = 0; ... }` (replaces `u16 pad_`, size unchanged) plus "a resolved
Kill target that is already dead completes the step".

## H. Fire and smoke on a wrecked car

Closes the M-CARWRECK remainder ("fire and smoke on a wreck"). It is not content-only today: `FireOn` has no death condition,
and `ParticleBurst` calls `ParticleSystem::burst`, which only feeds 2D `ParticleEmitter`s, so it does nothing on a car's
`ParticleEmitter3D`. A one-time burst alone would also leave no lasting smoke.

**Kit change (one PR, `effects.h` + `effects.cpp`; fan-out of `effects.h` is 20 TUs, so no seam PR).**
- Append `Died` to `FireOn` and to its `MCE_ENUM` list (value 7; earlier values unchanged). `record_physics_events` adds
  `if (w.event_queue<Died3D>()) for (d : read<Died3D>()) add_unique(n.died, d.entity)` (`combat3d.h` included by the .cpp
  only); `should_fire` gets `case FireOn::Died`. `Died3D` is produced in FixedUpdate_Pre and read in Post, as `Landed3D` is.
- `ParticleBurst` also drives a `ParticleEmitter3D` on the entity: `burst = count`, `++burst_version` (Kit3DExtract's
  one-shot path). Its `pad_[2]` becomes `u8 keep_emitting = 0; u8 pad_ = 0` (size stays 12, `static_assert` unchanged):
  1 = the fire also sets that emitter's `emitting = 1`. That is the continuous smoke: authored `emitting 0`, switched on at
  death, saved with the scene, so a loaded wreck still smokes without `Died` firing again.
- `SfxTrigger` on an entity with `AudioSpatial` fills `PlaySfx.attenuation_id` and `position` from it, so a wreck is heard
  where it burns (cars already carry `AudioSpatial{attenuation_id 1}`). `Help` strings of the three `on` fields name `Died`.

**Data (`gen_assets.py`).** A helper `wreck_effects()` merged into the root by `car_entities` (traffic prefab and the 3 parked
cars) and `police_car`:
- `ParticleEmitter3D`: color_start [2.5, 1.0, 0.25, 0.9], color_end [0.06, 0.06, 0.06, 0], offset [0, 0.5, -1.1] (the hood;
  cars drive toward -z), box [0.4, 0.1, 0.4], direction [0, 1, 0], gravity [0, 1.2, 0], spread_deg 18, rate 14, speed 1..2.2,
  lifetime 2.5..3.5, drag 0.6, size 0.5 -> 2.2, max_particles 128, seed 9, collide 0, additive 0, emitting 0.
- `ParticleBurst {count 60, on Died, keep_emitting 1}`; `SfxTrigger {clip guid:G_CRASHES (crashes.mcsound, a set PlaySfx
  resolves), gain_db -2, gain_jitter_db 2, on Died}`; `ShakeTrigger {trauma 0.35, on Died}`.
- The "Camera" entity gets `CameraShake3D {}` (defaults: 0.15 m, 0.03 rad, decay 1.5/s); without it the shake is a no-op.

**Tests.** `tests/kit/test_effects.cpp`: an entity with `Health3D`, the emitter, burst, Sfx and shake triggers, killed by
`apply_damage`: next tick `burst_version` 1, `burst` 60, `emitting` 1, each `fired` 1, one `PlaySfx` with the entity's
position, camera trauma 0.35; a living neighbour unchanged; no second fire on later ticks; `FireOn` "Died" round-trips by name;
same run twice gives the same state hash. `tests/mcp/test_mcp_openworld.cpp`: `ParticleBurst` count >= 3 at tick 1 (parked
cars; police and traffic add more); set the first parked car's `Health3D.current` 0, run 2 ticks: its `ParticleEmitter3D.emitting`
1 and `ParticleBurst.fired` 1. `sample_coverage.allow` and `capabilities.md` are left to the integration runner.

**Known limits.** One emitter per entity, so fire and smoke share one colour ramp (no flame light, no separate fireball
sprite); the burst uses the smoke's speeds. The shake has no distance falloff (traffic spawns near the player, so it is rare
from far away). No blast damage or impulse to neighbours. A traffic wreck's smoke ends with the car 10 s after death. Nothing
puts the fire out; a future repair must set `emitting 0`.

Interface: `FireOn::Died` (appended) plus `ParticleBurst { u32 count; FireOn on; bool fire; u8 keep_emitting; u8 pad_; u32 fired; }`
driving the entity's `ParticleEmitter3D` (burst and, with `keep_emitting`, `emitting = 1`).

## I. Repair shop (pay and spray)

A wreck stays a wreck: `kit_wreck` (`impact_damage.cpp`, FixedUpdate_Pre) re-applies `Vehicle3D.use_input 0`, throttle/steer 0,
handbrake 1 (and AI `drive 0`) every tick while `Health3D.dead`, and section H's smoke (`ParticleEmitter3D.emitting 1`) never
stops. Nothing restores a damaged car either. `PaySpray3D` (M-PAYSPRAY) exists but is not in the 09 scene and only clears the
wanted level. Cash (`Wallet3D.cash`) and `Wanted3D` (`heat`, `level` = ceil(heat / heat_per_level), `seen_by` = police units that
see it this tick, written by `kit3d_wanted`) sit on the player, who keeps them while seated (`kit3d_seat_pose` moves its
`LocalTransform` with the car). The shops use a distance test, not `Trigger3D`; the HUD binds `Wallet3D/cash` and the stars already.

**Kit change (one PR, gameplay kit only; no ECS, render or physics change).** `wanted3d.h` (fan-out 13 TUs) and `impact_damage.h`
(8 TUs), so no seam PR.
- `void repair_vehicle(World&, Entity car)` in `impact_damage.h/.cpp`, the undo of `kit_wreck`: `Health3D` current = max, dead 0,
  dead_time 0, stagger_left 0 (as `kit3d_respawn` revives); `Vehicle3D` handbrake 0, `use_input` = 1 when `VehicleSeat3D.driver`
  is set (the boarding state; `seat.saved` stays valid for the exit); `ParticleEmitter3D.emitting` 0 (live particles fade out).
  `kit_wreck` no longer sees `dead`, so the order of the two systems does not matter. A car destroyed by `destroy_on_death` is gone.
- `RepairShop3D` next to `PaySpray3D` (it needs the file-local `settle`), `static_assert(sizeof(RepairShop3D) == 24)`:
  `{ f32 radius = 5; f32 max_speed = 2; u16 price = 150; u16 price_per_level = 100; u8 wanted_drop = 0; u8 need_unseen = 1;
  u8 caption = 1; u8 enabled = 1; Entity served; }` (`served` ReadOnly, Transient).
- System `"kit3d_repairshop"`, FixedUpdate_Pre, after `"kit3d_chopshop"` (so after `kit3d_wanted` and this tick's `seen_by`).
  Per shop, per car with `VehicleSeat3D` + `Health3D` within `radius`, `|Vehicle3D.speed| <= max_speed`, and a driver holding
  `Wallet3D`: once per visit (`served` = that car, cleared when it leaves `radius`; stops `wanted_drop` N from dropping a level
  every tick). Repair part: `price` when `current < max` or dead. Wanted part: when the driver's level > 0 and (`need_unseen` 0
  or `seen_by` 0): `wanted_drop` 0 clears it (`clear_wanted`), N lowers it by N (`heat = min(heat, (level - N) * heat_per_level)`,
  `settle`), at `price_per_level` x levels dropped. All or nothing: too poor, nothing happens (as `PaySpray3D`). Pays with
  `take_cash` (pushes `CashChanged3D`), pushes `Repaired3D { driver, shop, car, paid, level }` and with `caption` a `ShowCaption`
  "REPAIRED -$N" when `Captions` is installed (the `kit3d_respawn` pattern). Seen by police: the car is repaired, the record kept.
- `MCE_COMPONENT` with `Help` and `Save` on the four settings; registered in `install_wanted3d`, event `Repaired3D`.

**Data (`gen_assets.py`).** `pad()` 8 m, appended at the end of `scene()` so the older entity order (and its hashes) stays:
"Repair shop 1" at (-55, 9) (across the avenue from the start, 25 m from parked car 1) and "Repair shop 2" at (-175, -9) (west end, inside the
roadblock), each `RepairShop3D {radius 5, price 150, price_per_level 100}` and `MapMarker {label "Pay 'n' Spray", always_visible 1}`.

**Tests.** `tests/kit/test_wanted3d.cpp` (M-REPAIRSHOP): a car with a seated driver (`Wallet3D 1000`, `Wanted3D` level 2) killed by
`apply_damage`; one tick: `use_input` 0, `handbrake` 1. Shop on the car: current = max, dead 0, `use_input` 1, `emitting` 0, cash
1000 - 150 - 200, level 0, one `Repaired3D`; later ticks charge nothing; out and back in charges again. Also: moving above
`max_speed`, no driver, too poor, `seen_by > 0` (repair only), `wanted_drop 1` (2 -> 1); same run twice, same state hash.
`tests/kit/test_impact_damage.cpp`: `repair_vehicle` on a wreck. `tests/mcp/test_mcp_openworld.cpp`: `RepairShop3D` count == 2.

**Docs.** `docs/engine/kit.md` (wanted paragraph), `docs/user-guide/09-physics.md` (beside pay-and-spray),
`docs/skills/mce-gamedev/references/kit-components.md`, `changelog.d/M-REPAIRSHOP.md`. `capabilities.md` and
`sample_coverage.allow` are the integration runner's.

**Known limits.** A wreck cannot drive in (it must die there or be pushed in). No door, spray animation or colour change; no
police-memory rule (a unit that saw the car does not lose it). The police check is this tick's `seen_by`, not a ray of its own.
Traffic cars are locked, police cars have no seat, so only the player's car is served.

Interface: `RepairShop3D` (24 bytes), event `Repaired3D`, system `"kit3d_repairshop"`, `repair_vehicle(World&, Entity)`.

## J. Pause menu, options, save/load (A12)

Esc does nothing in 09 today, so the A12 note "the stock player has Esc pause" does not hold here. The scene has `UiScreen`s
(HUD, mission HUDs), so `mce_player` suppresses its own overlay (`screens_.active()`) and passes Esc on as the `Pause` action of
`openworld.mcinput`, which no system reads. That orphans the scene's "Pause mix" `AudioSnapshot{while_paused}` too (only the
overlay's `paused_` fades it in). Show, hide, hold and quit are already data (`UiFlow`). Volume and look sensitivity are data too
(a widget's `"convar"`: `audio.master_db` and `input.look_sensitivity` are Saved convars the player writes and applies; neither
touches the sim, so no hash changes). Save and Load cannot be data only: `UiFlowDo` has no save act, nothing in the kit writes a
save, and a Luau `mce.save` never sees the button, because `script.pre/post` do not run on held ticks (only `run_during_cutscenes`).

**Kit change (one PR, `ui_flow.h` + `ui_flow.cpp`; fan-out of `ui_flow.h` is 6 TUs, so no seam PR).** No new component.
- Append `SaveSlot, LoadSlot` to `UiFlowDo` and its `MCE_ENUM` (values 6, 7; earlier values unchanged). The slot name is the
  rule's `field` (unused by Action and Signal rules; Help says so; empty means "quick"), checked like Luau's `slot_arg`
  (1..64 of `[A-Za-z0-9_-]`). Add `static_assert(sizeof(UiFlowRule) == 128)`; no size or padding change.
- `SaveSlot`: `save::save_game` with `SceneChange.current_guid`, `f.tick` and the playtime, as `l_save` does. `kit_ui_flow`
  already `run_when_held`, so the save is taken while the menu is up. It only reads the world, so the state hash is unchanged.
- `LoadSlot`: when `save::exists(slot)`, it sets `load = scene` (when set) and calls `scene::request_load(slot)`. The host's
  `apply_change` restarts the scene, then applies the slot: a fresh city plus the saved deltas, with the screens back at their
  authored `visible`. A missing slot does nothing and logs at info (`openworld_test.cmake` fails on `[warn]`).

**Data (`gen_assets.py`).** `pause_menu()` and `options_menu()` write `ui/pause.mcui` and `ui/options.mcui` (modal, the
`ui_screens` layout). Both entities are appended at the end of `scene()`:
- "Pause menu": `UiScreen{pause.mcui, order 10, visible false}`, `UiFlow{show_on Pause, hide_on Resume, quit_on Quit, pause 1}`.
  `UiFlowRules`: Action "Save" -> `SaveSlot` (field "quick"), then Hide; Action "Load" -> `LoadSlot` (field "quick",
  scene `G_SCENE`). Buttons: RESUME, SAVE, LOAD, OPTIONS, QUIT TO DESKTOP. The veil panel's `"action": "Resume"` makes Esc
  (UI map Cancel) resume.
- "Options menu": `UiScreen{options.mcui, order 11, visible false}`, `UiFlow{show_on Options, hide_on Back, pause 1, keep_audio 1}`
  (the volume can be heard while it moves). Sliders `audio.master_db` [-40, 0, 2] and `input.look_sensitivity` [0.25, 3, 0.05],
  plus a BACK button; the panel's Cancel presses Back, and the pause menu stays underneath.
- `"SaveKey": {"key": 999}` on "Player" (missions keep 1001+): position, cash, wanted heat and bag are saved. The input map
  adds `gamepad Start` to UI `Cancel`.

**Tests.** `tests/kit/test_ui_flow.cpp` (`save::set_root` to a temporary folder): `SaveSlot` on a held tick writes the slot and
leaves the state hash as it was. `LoadSlot` restores a changed `Wallet3D.cash` after `apply_change` and queues the scene. A missing
slot does nothing. A bad slot name is refused. The enum round-trips by name. `tests/mcp/test_mcp_openworld.cpp`: `UiFlow` == 2,
`UiFlowRules` == 1; with input "10 Pause\n12-60 Move=0,1", "Pause menu" is visible and the Player has not moved at tick 60.
"61 Resume" lets it walk. A second headless test (`openworld_save.input`, `-save_dir <build>/saves`): Pause, Save, walk, Pause,
Load ends with `quick.mcesave` present, the Player back at the start position, and the same hash on two plays.

**Docs.** `docs/user-guide/12-ui-and-console.md` (the two new acts beside `UiFlowRules`), `13-sequences-fsm-saves.md`,
`docs/skills/mce-gamedev/references/kit-components.md`, `changelog.d/M-OWPAUSE.md`. `capabilities.md` and
`sample_coverage.allow` are left to the integration runner. Gameplay kit and sample only: no change to ECS, render, physics,
the player or `.mcui`.

**Known limits.** There is one slot and no confirmation, slot list or "saved" toast: SAVE closes the menu as feedback. Fields
without `Save` (`Health3D.current`, `Weapon3D` ammo, seat state, traffic, police) come back as authored. A save taken while
driving loads the player standing where the car was, and the car at its spawn. The pause itself is silent (no `keep_audio`):
the "Pause mix" snapshot stays unused and could be removed. Invert-look is left out: there is no convar for it, so it would
need an input change.

Interface: `UiFlowDo::SaveSlot`, `UiFlowDo::LoadSlot` (appended; slot in `UiFlowRule.field`, scene to restart in `scene`).

## K. Horn and headlights

**Goal.** The player's car honks on a button and its headlights can be forced on or off. Today every car carries a nose
`SpotLight3D` driven by `TimedLight3D` (dusk to dawn, `headlights()` in `gen_assets.py`) and the player has no say.

**Where.** A new kit header `mce/kit/vehicle_extras.h` and `src/engine/kit/src/vehicle_extras.cpp`, installed from
`install_stock` after `install_seats3d` and `install_timed_light`. `vehicles3d.*`, `seats3d.*`, `traffic.*` and
`timed_light.*` (routine 3 and the day cycle) are read, never edited. Nothing in combat, melee or cover.

**Components** (both padding-free, `static_assert(sizeof(...) == N)`, `MCE_COMPONENT`, enum `MCE_ENUM`, append-only):
- `VehicleHorn3D` (32 bytes): `AssetRef<Sfx> clip` (16), `f32 gain_db = -4`, `f32 min_gap_s = 0.35`, `u16 attenuation_id = 1`,
  `u8 honk` (set to 1 to honk once, the engine clears it: Luau, AI and tests use the same path), `u8 pad_`, `u32 honks`
  (engine-written, Transient). A honk pushes one `audio::PlaySfx` at the car's position (3D with `attenuation_id`), at most
  once per `min_gap_s`. A one-shot event is used because the root's `AudioSource` is already the engine loop (one per entity)
  and a child's source would not follow the car (the bridge reads the entity's own `LocalTransform`, see the siren note).
- `VehicleLights3D` (4 bytes): `HeadlightMode mode` (`enum class HeadlightMode : u8 { Auto, On, Off }`, Save), `u8 pad_[3]`.
  Auto leaves `kit_timed_light` in charge. On sets the same entity's `SpotLight3D.enabled = 1` and its intensity to
  `TimedLight3D.spot_scale` (the captured authored value; the light's own intensity when there is no `TimedLight3D`). Off sets
  `enabled = 0`. The toggle cycles Auto -> On -> Off -> Auto. No child entities: the spot already sits on the car root. An
  emissive lens on the Kenney model is left out (its atlas has no lamp region); `TimedLight3D.emissive_*` could drive one later.

**System.** One, `kit_vehicle_extras`, `FixedUpdate_Pre`, after `kit_timed_light` (it must overwrite that tick's light write).
It reads seat state written by `kit3d_seats` in the same phase; if the scheduler does not already order it after the seats, the
input reads one tick late, which is harmless. For each `VehicleSeat3D` whose `driver` is set and whose driver has `Rider3D`
with `use_input == 1` and `seated == 1` (the player, never a `TrafficDriver3D`/`Police3D` car), `Horn` pressed sets `honk`
and `Lights` pressed cycles `mode`. A car without a driver reads no input, so walking players never honk. Actions are looked
up with `in->has_action(...)` like `kInteract` in `seats3d.cpp`, so maps without them are untouched. The horn and the light
write are pure functions of sim state plus input: replays and the state hash stay deterministic.

**Input** (`input_map()` gameplay): `Horn`: `key("G")`, `gamepad_button LeftStick` (stick press, as in most driving games).
`Lights`: `key("L")`, `gamepad_button DpadRight`. H stays `UseItem`; G and L are free in the current map.

**Audio** (`audio_assets()`): add `"horn": ("wav", {})` to `AUDIO_FILES`, a CC0 car horn fetched and pinned by
`tools/fetch_audio.py` (trimmed to about 0.6 s mono 44.1 kHz, deterministic ffmpeg/numpy cut), with a row in
`assets/audio/SOURCES.md` and `CREDITS.md`. If no pinned CC0 horn passes review, synthesize it in `fetch_audio.py` (two square
tones at 400 and 500 Hz, 0.5 s, 10 ms fades). `car_entities()` adds `VehicleHorn3D{clip horn}` and `VehicleLights3D{}` to
the player-takeable (non-traffic) cars only; traffic and police cars stay as they are.

**Out of scope.** Pedestrians and traffic reacting to the horn (a later `HornHeard3D` event could feed `crowd`/`traffic`,
routine 3), brake and tail lights, high beams, a HUD icon.

**Tests.** `tests/kit/test_vehicle_extras.cpp`: seated player + `Horn` pressed pushes one `PlaySfx` with the car's position
and bumps `honks`; held across ticks inside `min_gap_s` pushes no second one; unseated player or an AI-driven car pushes
none; `Lights` cycles Auto -> On -> Off and On wins over `TimedLight3D` at noon, Off wins at midnight, Auto restores the timed
ramp; the enum round-trips by name; a two-run hash check. `tests/mcp/test_mcp_openworld.cpp`: with a `drive.input`-style script
("enter car, 40 Horn, 60 Lights") the player's car has `honks == 1` and its spot `enabled == 1` at noon. Both are short (no `slow`).

**Docs and goldens.** `docs/skills/mce-gamedev/references/kit-components.md`, the driving page of the user guide,
`changelog.d/M-OWHORN.md`. The registered component count rises by two: `tests/mcp/transcripts/demo_edit.golden.jsonl`
`"components": 178` -> 180 (regenerated by the integration runner, like `capabilities.md`; a PR may bump it by hand).

**Fan-out.** The new header is included only by `vehicle_extras.cpp`, `stock.cpp` and the test; it forward-declares `World`
and `AssetSystem` and includes `asset/handle.h`, `refl/macros.h` and `math.h` only, so `scripts/ci/include_fanout.py` shows
no new wide header. `kit3d.h`/`components.h` umbrellas are not grown; `seats3d.h`, `timed_light.h`, `audio/components.h`
and the light component header are included from the `.cpp` only.
