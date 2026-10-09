# M7 — Ashen Court (milestone record)

**Scope:** ROADMAP M7. This covers the Ashen Court level with a stated triangle budget and a per-mesh budget table checked at cook. It also covers:

- a third-person character with a `camera_sweep` boom
- three FSM enemy types
- melee combat and health pickups, built on events rather than callbacks
- spatialized combat audio
- point lights plus a sun
- a HUD with scene states

The game is `samples/05_ashen_court`. Gameplay is `game/court.{h,cpp}`: ECS, `World3D`, `fsm.h`, input actions and audio events, with no host knowledge. The host is `main.cpp`. Content is procedural, from `tools/gen_assets.py`, which is deterministic; it is cooked at build time.

**Measured on:** the Linux CI container (4 cores, Mesa llvmpipe software GL, Xvfb), Debug build, 1280×720.

| ROADMAP M7 item | Where | Evidence |
|---|---|---|
| The level, with a stated triangle budget (≈ 300k) | `court::build_layout` places the court: a 44 m walled square (44 wall segments), a 14-column colonnade with fallen columns, two arches, a three-step dais, 6 braziers and 8 rubble piles. That is 76 static pieces. `kLevelTriangleBudget = 300000`. | At load the host sums every placed static plus the full cast against the budget and refuses to start if it is over. Today the level is **64,256 triangles of 300,000**, printed by every run. |
| Per-mesh budget table checked at cook | A new glTF sidecar key, `"max_triangles"`: a mesh over its budget fails the cook with `N triangles, over the mesh's budget of M`. `gen_assets.py` writes every model's budget into its sidecar. | `test_cook` "gltf: a sidecar triangle budget fails the cook…" covers over, at and exactly-at the budget, plus a non-integer value. The table is below. |
| Third-person character + camera with `camera_sweep` | `ac_player`: camera-relative run (5 m/s), jump and turn on `World3D::move_character`, which steps 0.45 m so the dais steps climb. `ac_physics`: after `World3D::step`, a 0.25 m sphere is swept from the head to the wanted eye 5.5 m back, and a hit pulls the eye in front of the wall. | `test_ashen_court` "the camera boom is pulled in front of a wall, never through it": the eye stays at least the probe radius inside the wall face, and turning to the open court restores the full length. The headless bot run counts boom pull-ins: 94 over 3000 ticks, closest 3.25 m. |
| Three FSM enemy types | `fsm.h` tables, validated at install:<br>• **husk** (melee): Idle → Chase → WindUp → Recover, plus Stagger. The blow lands on WindUp's exit action.<br>• **wisp** (caster): Idle → Hover → Cast → Cooldown, plus Stagger. It strafes at 6–9 m and fires a slow orb on Cast's exit.<br>• **brute** (charger): Idle → Chase → WindUp → Charge → Stunned or Recover. The entry action locks the charge direction; a wall stops the charge and stuns it, and a stunned brute takes double damage.<br>Guards read a shared `Senses` component that `ac_senses` refreshes each tick (distance, line of sight by raycast, reach). | `test_ashen_court`:<br>• "a husk notices the player, closes in and strikes"<br>• "a wisp keeps its distance and its orb hits"<br>• "a brute charge hits once, slams the wall and leaves it stunned for double damage" |
| Melee combat + health pickups (events, not callbacks) | Attackers (the sword at swing tick 7, a husk's blow, an orb, a charge) push an `AttackEvent` with a cone or sphere. `ac_combat` resolves it against `Health`: damage, invulnerability frames, stagger and knock-back. It pushes `Damaged` / `Died` for feedback. Ember hearts heal 35 on touch, only when hurt. | "two sword hits kill a husk; the first staggers it"; "an ember heart heals a hurt player and is used up; a healthy one leaves it"; "death waits for confirm, then the court restarts with a fresh cast". The restart goes through `Commands` and a settle tick, so it is phase-safe. |
| Spatialized audio for combat | Every combat sound is a `PlaySfx` with the actor's position and attenuation curve 1 (inverse, 2–40 m). The listener rides the camera, facing the pivot. The 7 clips are synthesized in the host. | The audio command hash is folded into the headless hash (the determinism stage below); 3D one-shots use the M3 path covered by `test_audio`. |
| 4–6 point lights + sun | 6 brazier point lights with value-noise flicker and a low dusk sun with 3 shadow cascades. Each wisp core, orb and heart adds its own light (11 lights in the screenshot frame). Fog and bloom are on. | Screenshots below; `sample_05_ashen_court_gpu` (GL debug layer, 0 errors) |
| HUD via text sprites + scene states | `Mode`: Title → Playing ⇄ Paused → Dead or Victory → restart. The HUD shows an HP bar, foes left, kills, time and render stats, plus a banner per state. | the restart test above; screenshots |
| Determinism | The bot (`court::autopilot`) is a pure function of the world. It hunts the nearest enemy, sidesteps a brute's wind-up and fetches a heart when hurt. | `test_ashen_court` "the bot clears the court, the same way every run": 9/9 kills, no deaths, equal hashes. The determinism stage runs `-headless 1 -ticks 3000` (a cleared court, then a restart) 3 times in Debug and Release: one hash. |
| Editor perf dashboards | the editor's Profiler panel | `docs/milestones/M7-editor-perf.md` |

## Triangle budgets (cook-checked)

| Mesh | Triangles | Budget |
|---|---:|---:|
| court_floor | 4,608 | 12,000 |
| wall_segment | 60 | 1,500 |
| column | 1,548 | 6,000 |
| broken_column | 1,344 | 4,000 |
| arch | 336 | 4,000 |
| dais | 36 | 1,000 |
| brazier | 708 | 3,000 |
| rubble | 1,920 | 4,000 |
| hero | 2,136 | 6,000 |
| sword | 36 | 500 |
| husk | 1,864 | 5,000 |
| wisp | 848 | 5,000 |
| brute | 1,672 | 6,000 |
| orb | 360 | 1,000 |
| ember_heart | 80 | 500 |
| **level** (76 statics + the cast) | **64,256** | **300,000** |

## Frame budget

600 frames, bot playing, `-frames 600 -autoplay 1 -r.vsync 0`, with the CI stages running alongside:

| Stage | CPU ms | GPU ms (llvmpipe) |
|---|---:|---:|
| gameplay tick (ECS + World3D + 3 FSMs + combat) | 0.22 | — |
| scene build (cull, sort, cascades, clusters) | 0.90 | — |
| command recording (3D + HUD) | 1.11 | — |
| shadow cascades | — | 59 |
| main pass (Forward+ PBR + sky) | — | 436 |
| post (resolve, bloom, tonemap, FXAA) | — | 103 |

The engine's CPU share is about 2.2 ms per frame, which leaves wide headroom against the 16.6 ms frame. As in M5a, every GPU number is llvmpipe on a loaded 4-core container. That is orders of magnitude slower than any target GPU, so the "60 fps at 1080p with ≥ 2 ms headroom" gate can only be judged on hardware.

## Honest gaps

- **Content is procedural, not CC0 kits.** The ROADMAP plans the level kit-assembled from Kenney and Poly Haven CC0 kits with Blender kitbashing. The build container cannot reach those sites (kenney.nl returns HTTP 403, and Blender is not available), so every mesh and texture comes from `gen_assets.py`. The level is therefore far under its budget (64k of 300k), and the budget column is an allowance rather than a measured kitbash.
- **Audio is synthesized,** not the freesound/JSFX pass the ROADMAP plans. There is no music.
- **Characters are static proxies** (ROADMAP Open Question 3 default): node transforms, no skinning. The sword swing is a host-side transform from the swing tick.
- **Real-GPU numbers, RenderDoc captures and golden images** need hardware; this container has only llvmpipe.
- **Mouse look** reads raw pointer deltas; there is no relative-mouse or pointer-lock mode in `mce_platform` yet. Q/E and the right stick turn the camera too.

## Screenshots

`/mnt/project-files/m7-3d/ashen-court-*.png` (title, combat).
