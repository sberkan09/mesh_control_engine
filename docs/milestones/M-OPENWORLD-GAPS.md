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
| A1 | **HUD** (E2 done in the HUD PR: health, wanted level, cash, ammo, objective hint; wanted stars done (Panels bound to `Wanted3D/level`, visible from `range` lo up); open: the objective text is static until E7 missions): no health bar, ammo, wanted stars or any on-screen text. The player cannot tell the wanted level, health or that a crime was counted | `.mcui` documents, `Wanted3D`/`Health3D` events | `samples/10_missions/assets/ui/hud.mcui` |
| A2 | **Minimap and map markers** (E2: minimap with car and police markers wired; mission markers wait for E7) | `MapMarker`, `Minimap` widget (M-MISSION step 2) | `10_missions` |
| A3 | **Missions** (reach, kill, deliver, timers, Luau chaining, retry, pay) | M-MISSION, M-MISSIONSTART/-PAY/-RETRY | `10_missions` |
| A4 | **Day/night cycle**: ~~the sun is fixed~~ **done (E6, 2026-10-29)**, see [M-OPENWORLD-DAYNIGHT](M-OPENWORLD-DAYNIGHT.md) | `WorldClock`, day cycle, moon (M-DAYNIGHT) | `11_daynight` |
| A5 | **Weather**: ~~rain, wet roads, fog, lightning, wind~~ **done (E6)** except snow (no snow cover in the city) | M-WEATHER, M-WEATHERAUDIO | `12_weather` |
| A6 | **Audio** (done 2026-10-29, E3: engine by speed, shots, footsteps, sirens, city bed; shots land and cars crash with sounds; still missing: radio, music) | `mce_audio`, M-AUDIO2, vehicle/weapon audio events | other samples |
| A7 | **Busted and wasted**: ~~dying or being arrested ends the game for good~~ **done (E4, 2026-10-29)**: hospital and station respawn | `Respawn3D`, `RespawnPoint3D` (M-WASTED) | none |
| A8 | **Arrest and reinforcements** | `Police3D` arrest, dispatch (M-ARREST 1-2) | none |
| A9 | **Police cars and roadblocks**: ~~all four police are on foot~~ **done (E4)**: 4 pursuit cars, 2 roadblocks | `drive_car` on `Police3D` (M-ARREST 3) | none |
| A10 | **Carjacking, vehicle damage and wrecks, ramming pedestrians** | M-CARJACK, M-CARWRECK, `vehicle_ram.h`, M-VEHICLEHIT | none |
| A11 | **Loot, shops, inventory** (ammo, health, chop shop) | `loot.h`, `inventory.h`, M-AMMOSHOP/-HEALSHOP/-CHOPSHOP | `12_loot` |
| A12 | **Pause menu, options, save/load** in the sample (the stock player has Esc pause; no game screens) | `ui_flow`, save system | `ui_screens` |
| A13 | **HLOD / `ChunkLod3D` skyline and scene streaming**: the whole city is one scene (714 entities) | A3/A4 steps, `samples/hlod_city` | `hlod_city` |
| A14 | **Floating origin**: not set (1 km² never reaches the threshold); matters if the map grows | `FloatingOrigin3D` | — |

## B. Content and look (observed in the captures)

- **Roads, ground, kerbs and trees are tinted boxes** (one `box.glb`). No road markings, lane lines, crossings, junction
  meshes or sidewalk detail; the road network is data only, so the visual road is a flat slab.
- **Washed-out colours.** The grass tint (0.20, 0.34, 0.14) shows as a pastel green (about 186, 214, 164) and the kerb or
  sidewalk strip reads near white. Suspect the fixed sun (intensity 3.0, exposure 1.0) and no grading; cause not confirmed.
  Check it on Metal as well (same shaders translated to MSL) before tuning.
- **Characters and cars are blocky/toy Kenney models with no animation.** Characters slide: the node clips of the blocky
  rig are not played; wheels do not spin; no door, steering or lights animation.
- **No sky**: no HDRI, no clouds, no stars; no night look; traffic lights do not change colour with the signal state.
- **No third-person feel**: the player does not face its movement direction (the engine has no facing component for
  `use_input` characters), the camera stays behind a fixed orbit, the gun is on the camera.
- **No visible feedback on shooting**: no muzzle flash, tracer, impact decal or hit marker (`WeaponFired3D` carries the data).
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
  fall-off or out-of-bounds handling (the ground is a 1120 m slab; the world edge has no wall).
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
4. **E5 look pass**: roads with markings and crossings, a sky HDRI, tint/exposure check on GL and Metal, shot effects.
5. **E6 day/night + weather** (A4, A5), **E7 missions + minimap** (A2, A3), **E8 shops, carjack, wrecks** (A10, A11).
6. **E9 animation**: play the node clips, wheel spin, door and seat transitions.
7. **E10 gates**: screenshot gate, budget counters, scripted playthrough test, Mac soak.
8. **E11 scale**: HLOD skyline, scene streaming, floating origin when the map outgrows 1 km² (A13, A14).

Steps 1 to 4 are the "make it playable" bundle; 5 to 8 make it a game; 9 to 11 are polish and scale. Each is a data-only or
single-module change, so they can run in parallel threads on disjoint files (`samples/09_openworld` plus one module each).
