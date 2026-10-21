# Solstice Reach — design document

**Project:** `projects/SolsticeReach` · **Game id:** `solstice_reach` · **Kind:** third-person exploration, 3D ·
**Purpose:** the engine's *graphics showcase*: one island seen under five lighting regimes in a single
ten-minute sitting, built as a project (SDK workflow) with a custom 3D host ·
**Status:** design baseline (2026-10-09); the "Built" column of §9 is updated as the game lands.

**As built since M-KIT3D (2026-10-12):** the custom host of §7 is gone. The game is a module on the
engine's 3D kit played by the stock player (`docs/milestones/M-KIT3D.md`, user guide chapter 8):
the game's `MeshRef` / `Light3D` / `Animated` / `BoxCollider3D` became the kit's `MeshRef`,
`PointLight3D` + `Glow`, `Animator` and `BoxCollider3D`; the sky keys, beams, HUD and sounds moved
into the module's `present.cpp`. Photo mode, the frame-budget table and `-autoplay` went with the
host (an idle title starts the bot instead). The sections below are the design as written.

Related: [Undelivered](undelivered.md) (the light novel built in the same thread), `docs/engine/render.md`,
`docs/engine/physics3d.md`, `samples/05_ashen_court` (the host and procedural-kit pattern this game extends).

---

## 1. Pitch

> *The sun is setting on the longest day of the year. You are the last Keeper of the Reach, a cliff
> observatory whose seven heliostat mirrors once carried the solstice light into the Vault at its
> summit. Relight them before the dark, and the island will show you what it keeps.*

Solstice Reach is a quiet, non-violent, ten-minute game: run, climb and glide-less-walk across a small
island, find and align seven mirrors, and watch the island change from gold to night to dawn as you do.
There is no fail state. Every mirror aligned advances the time of day, adds a beam of light to the sky
and lights a new part of the island. The finale ignites the Vault and brings the dawn.

It exists to put every feature of the M5a/M7 forward renderer on screen in one place: PBR materials with
normal and ORM maps, four cascaded shadow maps under a moving sun, Forward+ with well over a hundred point
and spot lights at night, emissive geometry under bloom, layered fog, a procedural sky with a sun disk
and a star field at night, glTF node animation, thousands of culled and sorted instances, MSAA, ACES
tonemapping and FXAA, with the GPU stage timings on the HUD so the viewer can see what it costs.

## 2. Story and world

### 2.1 The island

The Reach is a tilted limestone island, two hundred metres across, rising from a shallow turquoise
shelf to a cliff on its north side. An observatory order built it to measure the solstice: on the
longest day, seven polished mirrors (heliostats) placed around the island each catch the low sun and
throw it, mirror to mirror, up to the Vault, a domed hall at the summit whose oculus only lights on that
one evening. The order is gone. Wind, goats and lanterns remain; the lamps are still lit each night by
someone, and that someone is you.

Districts, from the landing stage up:

| District | Where | What it shows |
|---|---|---|
| **The Landing** | south cove, sea level | water plane with sun glitter, a wooden pier, nets, boats, the first lanterns; start point |
| **Terraces** | south slope | olive terraces and dry-stone walls, grass tufts and wildflowers in the thousands, a windmill whose sails turn (node animation), the first mirror |
| **The Cistern Garden** | east hollow | a sunken garden with a reflecting pool, cypress rows, a mirror whose beam crosses the pool |
| **The Stair of Hours** | west cliff | a long stair cut into the cliff with 24 lantern niches; two mirrors at its landings; the longest shadows at dusk |
| **The Colonnade** | north ridge | a ruined colonnade (ashlar, fluted columns, arches) on the cliff edge, wind-torn banners animating, a mirror |
| **The Crystal Cut** | the quarry on the north-east face | emissive sunshard crystals in the rock, the island's bloom-heavy district; a mirror |
| **The Vault** | summit | the domed hall, oculus, the seventh mirror and the finale |

### 2.2 The Keeper

The player is the Keeper: a hooded figure with a lantern on a staff (the lantern is a point light that
follows the player and matters at night). No dialogue; the Keeper's journal entries (short HUD lines
at each mirror) carry what story there is.

### 2.3 Act structure (time of day as narrative)

The island's time of day is a scalar `hour` in [17.0, 30.0] (17:00 to 06:00 next morning). It never
advances on its own while the player stands still; each mirror aligned moves it to the next keyframe
over an eight-second transition. The order the player finds mirrors in is free; the lighting states are
tied to the *count* aligned, so the story reads the same whichever route they take.

| Mirrors aligned | Hour | Regime | What the viewer sees | Journal line |
|---|---|---|---|---|
| 0 | 17:30 | **Gold** | high warm sun, short shadows, saturated grass and sea, bloom only on the sunshards | *"The last long day. Seven mirrors, one evening. Begin at the water."* |
| 1 | 18:40 | **Amber** | sun at 12°, long shadows across the terraces, orange fog creeping over the sea | *"The windmill still turns. Someone oiled it. Someone keeps coming back."* |
| 2 | 19:30 | **Ember** | sun on the horizon, red disk, cascade shadows at full stretch, first lanterns light themselves | *"Light goes from mirror to mirror the way a word goes from mouth to mouth."* |
| 3 | 20:15 | **Blue hour** | sun gone, deep blue sky gradient, fog thick and cool, every lantern on, warm against blue | *"The order measured the year by what the sun touched last."* |
| 4 | 22:00 | **Night** | star-field sky, moon as a cold directional light with soft shadows, fireflies in the garden (moving point lights), the lantern on the staff matters | *"Count the lamps on the stair. Twenty-four. One for every hour they could not see the sun."* |
| 5 | 02:00 | **Deep night** | the sunshards are the brightest things on the island; the beams aligned so far hang in the fog as emissive shafts | *"I used to think they built this to see further. They built it to keep from forgetting."* |
| 6 | 04:30 | **False dawn** | horizon pales, the moon fades, the colonnade silhouettes against a green-gold band | *"One more. The Vault has been waiting a year. So have I."* |
| 7 | 05:50 → 06:30 | **Finale → Dawn** | the Vault oculus fires, all seven beams converge, the sun rises at the end of the cutscene; free roam afterwards in full morning light | *"It keeps the light. That's all it ever did. That is enough."* |

The finale is a 25-second scripted camera move around the Vault while the beams converge and the sun
rises; afterwards the player can keep exploring in the dawn regime and a *photo mode* unlocks every
regime for scrubbing.

## 3. Gameplay

### 3.1 Controls

| Action | Keyboard / mouse | Gamepad |
|---|---|---|
| Move (camera-relative) | WASD / arrows | left stick |
| Look | mouse, Q/E | right stick |
| Jump | Space | A |
| Interact (align mirror, read plaque) | E / F / left mouse | X |
| Sprint | Shift (hold) | left trigger |
| Lantern on/off (night) | L | Y |
| Pause | Esc / P | Start |
| Photo mode (after the finale, or always with `-photo 1`) | P in pause menu | Back |
| Confirm (menus) | Enter / Space | A |

The character is a Jolt character (`World3D::create_character`, capsule 0.35 × 1.7 m): walks 4.5 m/s,
sprints 7 m/s, jumps 1.1 m, climbs 0.4 m steps, slides on slopes over 50°. The camera is a third-person
boom swept against the island (`camera_sweep`), 3.5 m behind the shoulder, pitch −20°..+60°.

### 3.2 The mirror puzzle (the only mechanic)

Each heliostat is a bronze dish on a pivot. To align one the player stands on its plate and holds
Interact: the dish turns toward the current light source (sun, or the previous mirror's beam at
night) over 2.5 seconds with an audible ratchet, a flare blooms on the dish, and a beam (a long
emissive, blended, double-sided quad strip from this mirror toward the next in the chain) appears and
stays. Mirrors can be aligned in any order; the chain always draws from each aligned mirror to the Vault
through the nearest aligned neighbours, so the sky fills with light progressively.

Finding the mirrors is the exploration: each is visible from a distance by its unlit dish (a specular
highlight in the day, a cold reflection at night) and from the previous mirror by a sightline. The HUD
shows `MIRRORS n/7`. There is no map; the island is small and the Vault is visible from everywhere.

### 3.3 Secondary loop: sunshards

42 **sunshards** (small emissive crystals) are scattered off the paths: on ledges, in the cistern,
behind the colonnade. Collecting one plays a chime, adds to `SHARDS n/42`, and brightens the sunshard
veins in the Crystal Cut (the crystal emissive intensity scales with the count). At 42 the Vault's dome
gains an inner glow during the finale. They exist to make the player look at corners of the island
from angles the paths do not give.

### 3.4 Photo mode

Unlocked after the finale (or from the start with `-photo 1`): the camera detaches (fly with WASD, Q/Z
up/down, Shift fast), the HUD hides, and five keys scrub the regime (`1`..`8` select the eight hours,
`[` `]` nudge `hour` by five minutes), `B` toggles bloom, `F` toggles fog, `T` cycles MSAA/FXAA for
comparison, `C` captures a PNG to the save folder. This is the mode the screenshots in
`/mnt/project-files/showcase-games/` come from.

### 3.5 Attract / autoplay

`-autoplay 1` (and every headless run) plays a scripted route through all seven mirrors in
`tick`-indexed segments (the same `autopilot` pattern as Ashen Court's bot): the route is what
`solstice_reach_headless_300` and the determinism check exercise, and what `-frames 1800 -autoplay 1`
renders for the budget table.

### 3.6 Session shape

Title (sky and sea, slow camera drift) → New game / Continue (auto-save at every mirror) → play (≈ 8–12
min to the finale, more with shards) → finale cutscene → dawn free roam + photo mode → Esc → title.

## 4. Visual design

### 4.1 Style

Painterly-real: clean PBR surfaces with strong material contrast (polished bronze mirrors against chalky
limestone, dark cypress against pale grass) and lighting that does the storytelling. Colour script:
gold → amber → ember red → cobalt → ink with warm lamps → ink with cold crystals → green-gold → morning
white. Every regime is a `SkyKey` (table in §7.3) blended by `hour`.

### 4.2 Renderer features exercised, where

| Feature (render.md) | Where it is unmissable |
|---|---|
| PBR metallic-roughness with albedo + normal + ORM | limestone ashlar, flagstones, bronze dishes (metallic 1, roughness 0.15), olive bark, painted boat hulls |
| 4 cascaded shadow maps, 2048² | the Stair of Hours at Ember (shadows 60 m long), the colonnade at False dawn |
| Forward+ point and spot lights | 24 stair lanterns + 30 path lanterns + 12 garden fireflies + 7 mirror flares + the Keeper's lantern + 20 Vault sconces: ~110 lights at Night, 16 per cluster max visible in the HUD counter |
| Emissive + bloom | sunshards, mirror beams, the Vault oculus, lantern flames (a small emissive flame mesh inside each glass) |
| Sky gradient + sun disk, equirect texture | day gradient with the sun; at Night the equirect star field (procedurally generated 2048×1024) fades in through `texture_intensity` |
| Fog | distance fog whose colour follows the horizon colour; thick at Blue hour, thin at Gold |
| Alpha-mask materials | grass tufts, wildflowers, cypress fronds, banner edges |
| Alpha-blend materials | the sea plane (roughness 0.05, alpha 0.8, double-sided off), mirror beams, the Vault's light shafts |
| Node animation | windmill sails (`turn`), banners (`flutter`, three clips crossfaded with wind strength), mirror dish rotation (`align`), gulls (`glide` on a looping path) |
| Instancing throughput | `Scatter` volumes place 6–9 k grass tufts, 1.2 k wildflowers, 180 trees, 400 rocks; the HUD prints instances submitted / drawn after culling |
| MSAA 4× + FXAA | toggled in photo mode; default both on |
| ACES tonemap, exposure | exposure keys per regime so night is dark but readable |
| GPU timings | `shadow / main / post / total` on the HUD, from `Forward3D::gpu_times` |

### 4.3 Content (procedural, deterministic)

Every asset is generated by `projects/SolsticeReach/tools/gen_assets.py` (standard library + the
04_cubes glTF/PNG writers, and NumPy/Pillow where they are present for the big textures): re-running
reproduces the committed bytes. CC0 kits are still unreachable from the build container (403), so
the kit is procedural, and the generator is written so a real kit can replace any model by path.

| Group | Models | Triangles (budget) |
|---|---|---|
| Terrain | `island.glb` as 16 chunks, 4 submeshes per chunk by slope and altitude (grass, dry grass, rock, sand), heightmap from ridged fBm with hand-placed district plateaus | 120 k |
| Sea | `sea.glb` (a 600 m disc, 4 rings so fog bands shade smoothly) | 1 k |
| Architecture | `vault.glb`, `vault_dome.glb`, `colonnade_column.glb`, `arch.glb`, `wall_low.glb`, `stair_flight.glb`, `landing.glb`, `pier.glb`, `windmill_body.glb`, `windmill_sails.glb` (animated), `cistern.glb`, `niche.glb` | 60 k total placed |
| Heliostat | `mirror_base.glb`, `mirror_dish.glb` (animated `align`), `beam.glb` (unit strip) | 6 k |
| Props | `lantern.glb` (glass + flame submesh, emissive), `boat.glb`, `net_post.glb`, `banner.glb` (animated), `olive.glb`, `cypress.glb`, `pine.glb`, `rock_a/b/c.glb`, `grass_tuft.glb` (3 crossed quads), `flower.glb`, `sunshard.glb`, `gull.glb` | per model ≤ 2 k; tufts 6 |
| Keeper | `keeper.glb` (rigid proxy, four nodes: body, hood, staff, lantern) with `walk`, `idle` node clips | 4 k |
| Textures | limestone ashlar, flagstone, terrace stone, grass, dry grass, rock, sand, bark, bronze, canvas, glass: albedo/normal/ORM at 1024²; `stars_equirect.png` 2048×1024; grass/flower/frond alpha masks 256² | |

A level triangle budget (statics placed + scatter + cast) is checked at load like Ashen Court's;
target ≤ 1.6 M triangles submitted before culling, ≤ 600 k drawn in the worst view.

## 5. Audio

Synthesised at start-up like Ashen Court (no WAV assets to fetch): wind bed whose gain follows altitude
and `hour`, sea wash near the shore (3D source at the pier), lantern crackle (tiny 3D one-shots,
staggered), the mirror ratchet and flare, the shard chime (pentatonic, pitch by count), the Vault's low
organ chord in the finale, gull cries by day, crickets by night. Listener on the camera.

## 6. Flow and screens

```
boot ─► Title ──New──► Play(Gold) ──mirror──► Play(Amber) ─ … ─► Play(False dawn) ──7th──► Finale ─► Dawn roam
          │  └Continue─► Play(from save)                                                        │
          │                                                                                       └─► Photo mode ◄─┐
          └◄──────────────────── Esc from Pause ◄──────────────────────────────────────────────────────────────────┘
Pause: Resume · Photo mode (if unlocked) · Lantern · Quit to title
```

HUD (top-left, the sample HUD font at 1×): `MIRRORS 3/7   SHARDS 12/42   19:30`, the journal line for
four seconds after each mirror, and the budget line `TRIS 412k/1.3M  LIGHTS 94  INST 7.8k/2.1k  GPU 9.4 ms
(sh 2.1 main 5.9 post 1.4)`. Photo mode hides all of it unless `H` is held.

## 7. Technical design

### 7.1 Why a project with a custom host

The stock player draws only the 2D kit (user guide ch. 8), so a 3D game owns its renderer. The game is
still a **project**: `mce_add_project(solstice_reach …)` builds the module `libsolstice_reach.so` (the
reflected components below, which the editor's inspector and the MCP server use to author the scene),
the per-project cook, editor and MCP server, and the pack. The project's `HOST` is a 3D host modelled
on `samples/05_ashen_court/main.cpp` that links the game sources in statically and `mce::mce_engine`
for the renderer, mounts the pack, instantiates the start scene with `ecs_scene`, runs the game's
`install()` and draws the 3D components it finds in the World. Against the SDK the same
`CMakeLists.txt` builds with `find_package(MCE)` (`mce build`, `mce play`, `mce editor`, `mce mcp`,
`mce ship` all work); the editor viewport shows the hierarchy and the inspector but no 3D preview,
which is the gap this game documents (§9).

### 7.2 Module components (`games/solstice_reach/module/components.h`)

A minimal 3D kit, reflected so scenes carry the island layout:

| Component | Fields | Purpose |
|---|---|---|
| `MeshRef` | `asset::AssetRef<asset::Mesh> mesh`, `vec4 tint`, `bool cast_shadows`, `f32 cull_radius` | a placed model; the host draws `WorldTransform × mesh` |
| `Scatter` | `mesh`, `mesh_b`, `mesh_c`, `vec2 half_extent`, `f32 density`, `f32 min_scale`, `max_scale`, `max_slope_deg`, `u32 seed`, `u8 align_to_ground` | a volume filled deterministically at Startup with instances (kept in a resource, not as entities, so 9 k tufts do not cost 9 k rows) |
| `Light3D` | `Color color`, `f32 intensity`, `radius`, `spot_outer`, `spot_inner`, `vec3 direction`, `u8 kind` (Lantern / Firefly / Sconce / Flare), `u8 night_only`, `f32 flicker` | a point or spot light; the host flickers lanterns and moves fireflies |
| `Heliostat` | `u8 index`, `Entity next`, `vec3 dish_offset`, `f32 aligned` (Save), `f32 turn` (Save) | one of the seven mirrors |
| `Sunshard` | `u8 taken` (Save) | a collectible |
| `KeeperSpawn` | — | where the character starts |
| `Animated` | `FixedString<16> clip`, `f32 speed`, `f32 phase`, `f32 wind` | plays a node clip on the mesh |
| `Plaque` | `FixedString<64> line` | a journal line read with Interact |
| `ReachState` (resource) | `hour`, `target_hour`, `mirrors`, `shards`, `mode`, `finale_t`, `photo`, `save fields` | run state |

Terrain chunks, water and architecture are `MeshRef` entities placed by a script through the MCP server
(`tools/author_island_mcp.py`, the Kindle pattern), so the scene is an ordinary `.mcscene` the editor
opens; the scatter volumes and lights are authored the same way.

### 7.3 Time of day

`hour` lerps toward `target_hour` at 1 game hour per 8 s. The host evaluates a `SkyKey` table at
`hour` (sun elevation and azimuth, sun colour and intensity, ambient sky/ground, zenith/horizon/ground,
fog colour and density, star intensity, moon intensity, exposure, lantern gain) with smoothstep blends
between keys. The sun's direction comes from elevation/azimuth; below −6° the directional light switches
to the moon (cool, intensity 0.08, shadows kept) so night still has cascades.

### 7.4 Scene submission

The host's `fill_scene`: culling is the renderer's (`Scene3D::build`), but the scatter resource
pre-buckets instances into 16 m cells and submits only cells whose sphere passes `sphere_visible`, so
nine thousand tufts do not cost nine thousand `add` calls per frame. Beams are built per frame as unit
strips scaled between mirror positions. Lights: every `Light3D` in range (the Forward+ pass clips to 16
per cluster; the HUD counts submitted lights). HUD through the sample `SpriteBatch` path.

### 7.5 Determinism and tests

The World (character, mirrors, shards, hour, scatter seed) hashes like every project; the host's
renderer state is not in the hash. Tests: `solstice_reach_headless_300` (the project's standard
headless test, through the autopilot), `test_solstice_reach` (doctest: time-of-day keys monotonic,
the autopilot aligns all seven within 7 000 ticks, shards count, scatter determinism across two
Worlds), `solstice_reach_gpu` (60 frames under Xvfb). `-frames 600 -autoplay 1` prints the budget
table.

### 7.6 Build and ship

Checkout: `cmake --build --preset linux-gcc-debug --target solstice_reach solstice_reach_mcp`;
SDK: `mce build SolsticeReach && mce play SolsticeReach`. Ship: `mce ship SolsticeReach --version 0.1`
(the host, the module, `libmce_engine.so`, the pack).

## 8. Production plan (this thread)

1. Generator: textures and the architectural kit; terrain; sea; props; the Keeper. Budgets in the table.
2. Module: components, time of day, character, heliostats, shards, autopilot, journal lines, saves.
3. Host: renderer, sky keys, scatter cells, beams, HUD, photo mode, capture, budget table.
4. Island authoring through MCP (`tools/author_island_mcp.py`), iteration on the look per regime.
5. Tests, local CI, screenshots per regime into `/mnt/project-files/showcase-games/solstice-reach/`.
6. README, CREDITS, the engine-gap log below, user-guide cross-links.

## 9. Engine and SDK gaps this game exposes

Filled in as they are hit; each row says what was done about it in the game.

| Gap | Impact | Workaround in the game | Suggested engine work |
|---|---|---|---|
| No 3D kit (`MeshRenderer`, `Light3D`, `Camera3D` components) and the stock player draws only 2D | a 3D project needs its own host; the editor viewport shows nothing for it | the module declares its own `MeshRef` / `Light3D` / `Scatter`; the host draws them | a `mce_kit3d` with these components and a 3D extract in the player (ROADMAP post-v1.0) |
| No 3D particles | no dust motes, waterfall, firefly sprites | fireflies are moving point lights with a tiny emissive mesh | a 3D billboard particle path in Forward3D |
| No skinning | the Keeper is a rigid node proxy | node clips on body/staff/lantern | FR-R3D-060 — *built engine-side since (M-SKIN, ADR-0036); the Keeper still uses the rigid proxy (`tools/gen_assets.py` exports no skin)* |
| No vertex colours in the glTF importer | terrain blending must be per-submesh | terrain split into material submeshes by slope and altitude | COLOR_0 → a vertex attribute the PBR shader multiplies |
| No IBL | metals reflect the sky gradient only | `specular_ambient` per regime | prefiltered equirect |
| `Sky` texture replaced the gradient entirely | a star field at night would black out the horizon glow | added `Sky::texture_additive` to the engine (this thread): gradient + texture | done; keep |
| A command-line convar value starting with `-` reads as the next flag | `-cam "-10,20,..."` is silently dropped (one warning) | the host accepts a `@` prefix on the value | `-name=value` spelling in `parse_command_line` |
| No `save::remove` (only `exists`, `path_of`) | the title's NEW GAME cannot drop the autosave through the API | `std::remove(save::path_of(name))` in the host, then an in-place `reset_run` | a `save::remove(name)` |
| `Scatter`-style placement is per game | every 3D game rewrites the seeded placement, the cells and the distance thinning | `ScatterCache` resource in the module (16 m cells) | part of a future `mce_kit3d` |
| The editor viewport cannot show a 3D scene | the island was placed by numbers (`author_island_mcp.py` + `island.height`) and checked in captures | the authoring script drops every prop on the height field; `-photo 1 -cam` captures replace the viewport | a 3D extract in the editor's preview |
| The stock headless runner has no bot hook | `run_headless` through MCP plays without the autopilot, so a route check needs the host | `solstice_reach -headless 1 -autoplay 1 -trace N`; the ctest presets the `ReachState` resource with `autopilot = 1` | a `preset resources` argument on `run_headless` |

## 10. As built (what differs from the plan above)

- **Mirror order and route.** The chain is fixed, not free-order: 1 windmill terrace, 2 Cistern
  Garden, 3 Crystal Cut, 4 Colonnade, 5 Stair of Hours top, 6 Stair landing, 7 the Vault. The walk
  is the loop a player finds: Landing, terraces, the garden, up to the quarry, along the ridge to the
  colonnade, down the Stair, back east along the terraces and up the goat path (two legs and a
  landing) to the summit. Each mirror's beam goes to the next; the seventh goes up into the oculus.
- **Island layout.** `tools/island.py` applies plateaus first and ramps after, with each ramp's
  ends inside the plateaus it joins and a mostly linear grade (`grade(t)`); `python3 tools/island.py`
  prints the steepest grade of every ramp (all under 40°, the character's limit is 52°). The
  summit sits on a 35 m pinnacle (the base terrain is 15 m there), which reads as a Meteora rock.
- **Counts.** 49 sunshards (not 42; the HUD counts whatever the scene has), 87 lanterns, 12
  fireflies, 8 sconces, 34 columns, 33 waypoints, 416 entities, about 6.7 k scatter instances.
- **Autopilot** is `Waypoint` entities in the scene (not tick-indexed input segments): the bot
  walks to each in order, holds Interact on the mirror ones, hops when its walk stalls for a third
  of a second, and sprints when the target is far. The full run takes about 7,200 ticks.
- **Photo mode** keys: `1`..`8` hours, B bloom, G fog, T anti-aliasing, H HUD, C capture (a `.ppm`
  beside the save, not PNG); no `[` `]` nudge (the key map has no bracket keys). `-photo 1 -hour H
  -cam "@x,y,z,yaw,pitch" -frames 2 -capture f.ppm` is how the showcase captures were taken.
- **Time of day** is nine `SkyKey`s (sun or moon direction, colours, ambient, fog, stars, exposure,
  lantern gain) smooth-stepped by the hour; fog densities were halved from the first draft so the
  island reads from the sea.
- **Saves** restore `Progress`, the mirrors, the shards and the Keeper (`SaveKey`s); NEW GAME on
  the title deletes the autosave and resets the run in place (`reset_run`).
- **Not built**: plaques (`Plaque`), the sunshard-count-driven crystal glow, the Vault's inner glow
  at 42 shards, gamepad testing, Windows captures. (Metal captures were recorded on 2026-10-11, `docs/milestones/M-MAC.md`.)

Screenshots (llvmpipe, 1280×720): `/mnt/project-files/showcase-games/solstice-reach/`.
