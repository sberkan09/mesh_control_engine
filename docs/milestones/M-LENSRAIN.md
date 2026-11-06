# M-LENSRAIN — Rain on the camera lens: drops that gather on the glass, bend the picture and run off (milestone record)

**Status:** planned (2026-11-06), nothing built yet. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-RAINFX (its "Not in scope" names "rain on the camera lens"), M-WETSURFACE, M-LENSFLARE and the 3D visual assessment (`assessments/3d-gorsellik-degerlendirmesi.md`, item 8, post-process). Today the frame has rain particles, wet surfaces, ripples and splashes, and the camera look (vignette, grain, chromatic aberration, lens dirt, flare), but a camera in a storm sees a perfectly clean lens. No Linear issues (free issue limit); this record is the tracker.

**Naming:** no type, file or milestone called `LensRain`, `LensDrops` or `lens_rain` exists (`src/`, `docs/`, `tests/`, `samples/` checked 2026-11-06; the "droplet" hits are terrain and asset-pipeline prose). The render setting is `render::LensRain` (in `PostFx::rain`), the pure helpers live in `mce/render/lens_rain.h`, the kit component is `LensRain3D` (`mce/kit/lens_rain.h`).

**Goal.** An opt-in post pass lays a procedural field of water drops on the lens. Each drop is a small lens: inside it the picture is sampled at a mirrored, magnified offset (the classic refracting droplet), so a drop shows a tiny upside-down image of the scene behind it with a thin bright rim; a faint trail marks where the larger drops ran down. The field grows while it rains (drop count and size follow a `wetness` value), dries slowly after, and is blurred a little between drops, as a wet lens is. The pass runs on the HDR frame **before bloom** (like the flare), so bloom, the tonemapper and the grade act on the drops. **Render-side only: nothing is written to a component and nothing is hashed.** With `PostFx::rain.enabled` false (the default), no `LensRain3D`, the Low tier or `r.lens_rain 0`, the pass is not recorded and the frame is bit-identical to today.

**Not in scope:** drops that follow a camera shake or react to its motion (the field is fixed to the screen), rain on a render-to-texture camera other than the main one, snow or mud on the lens, a hand wipe, splashes from a puddle onto the lens, authored drop textures (the field is procedural), a Luau API (the component has no runtime state to script beyond its fields), and the MCP `render_frame` `environment` override.

## Design

- **Render setting (seam, 42a).** A POD `render::LensRain` in `scene3d.h` just above `PostFx`, and `LensRain rain;` in `PostFx` after `flare`. Fields (defaults, ranges `apply_quality` clamps to; NaN falls back to the default):

  | Field | Default | Range | Meaning |
  |---|---|---|---|
  | `bool enabled` | false | | off draws the frame without the pass |
  | `f32 amount` | 0.0 | 0..1 | how wet the lens is now (the kit feeds it from the weather); 0 records nothing |
  | `f32 density` | 1.0 | 0..4 | drops per cell, a multiplier |
  | `f32 size` | 1.0 | 0.25..4 | drop radius multiplier |
  | `f32 refraction` | 1.0 | 0..2 | strength of the mirrored magnification inside a drop |
  | `f32 blur` | 0.35 | 0..1 | how much the glass between the drops softens the picture |
  | `f32 trail` | 0.5 | 0..1 | strength of the runs behind larger drops |
  | `f32 fall_speed` | 0.25 | 0..2 | how fast the big drops creep down, in view heights per minute |
  | `u32 seed` | 1 | | selects the drop pattern |

  `scene3d.h` and `forward3d.h` are over the 150 warning (172 and 155 TUs on 2026-11-03), so 42a is the seam PR for both, data only, merged first, with the `include_fanout.py` numbers. In `forward3d.h` it adds `bool lens_rain = false; f32 lens_rain_amount = 0.0f;` to `Forward3DStats` and the private members 42c needs (`rain_pipe_`, `void record_lens_rain(rhi::CommandBuffer&, const Scene3D&);`), the handle in the destroy list. No later step touches either header.
- **Pure CPU helpers (render, 42b).** Narrow header `mce/render/lens_rain.h` (includes `mce/math` only, forward-declares `struct LensRain;`), bodies in `src/lens_rain.cpp`; f32, fixed evaluation order, no `rand`:
  - `f32 lens_rain_wetness_step(f32 wetness, bool raining, f32 intensity, f32 dt)`: the drying and wetting curve the kit applies is a pure function (rises toward `intensity` in about 4 s of rain, falls to 0 in about 40 s), clamped, NaN as 0.
  - `struct LensRainFrame { f32 amount, density, size, refraction, blur, trail, fall_speed, time, aspect; u32 seed; };` and `LensRainFrame lens_rain_frame(const LensRain&, f32 time_s, f32 aspect)`: everything the pass needs, `amount` exactly 0 for `enabled` false, `amount` 0, or NaN input (the pass skips on 0). The time is the renderer's wrapped frame time, never the simulation tick, so replays are untouched.
  - `u32 lens_rain_hash(u32 cell_x, u32 cell_y, u32 seed)`: the integer hash the shader mirrors (a lowbias32 chain), pinned by golden values so a CPU test proves the GLSL port constant for constant.
- **Quality tiers (render, 42b).** `QualitySettings::lens_rain` (Low false, Medium and High true) and `QualitySettings::lens_rain_layers` (Medium 1, High 2: the number of overlaid drop grids), cvar `r.lens_rain` (bool, Saved, "Rain on the camera lens (PostFx::rain; off = the frame without it)") next to `r.lens_flare` in `quality.cpp`; `apply_quality` clamps the ranges. `quality.h` only gains the two fields.
- **Kit component (kit, 42d).** Opt-in `LensRain3D` (`mce/kit/lens_rain.h`, one per scene, the first in ECS order is read, registered by `install_kit3d` next to `LensFlare3D`; a separate component so the wide `kit3d.h` stays untouched). Fields (all `Save`, with the ranges above): `enabled` 1, `density`, `size`, `refraction`, `blur`, `trail`, `fall_speed`, `seed`, `wet_rate` 1.0 (0.1..4: scales how fast it wets and dries), an explicit zeroed `pad_`. It has one piece of runtime state, `wet_q` (the lens wetness as an integer out of 2^24, `Save`, like `Weather::wet_q`, so a save and a replay resume the same), advanced by a small system in the kit tick from `Weather` (`current` rain or storm raises it toward `intensity`, anything else dries it); deterministic, integer, hashed. `Kit3DExtract` (`.cpp` only) copies the fields into `post.rain`, `amount = wet_q / 2^24`. A scene with no `Weather` keeps `amount` at 0 unless `wet_q` is written by hand (agents, Luau `world:set_component`).
- **Determinism.** The drops are a pure function of the screen position, the seed, `amount` and the renderer's frame time; the only state is `LensRain3D::wet_q`, an integer advanced by a fixed rule from the `Weather` state, so it is hashed and replayable; the frame never feeds it. A scene without the component is byte-identical.

## Pass design (42c)

- **Where it sits.** `record_lens_rain(cmd, scene)` runs in `Forward3D::record` right after `record_flare` and before `record_post`: after TAA, depth of field, motion blur and the exposure histogram, so the drops do not pump the exposure and are not smeared by TAA or its history. Its time falls in the existing `bloom` timing bucket; no new timestamp.
- **CPU side.** `lens_rain_frame(scene.post().rain, frame_time, aspect)`; `amount == 0` returns before recording anything (bit-identical frame, `stats_.lens_rain` false).
- **Composite pass.** One fullscreen pass (`kLensRainFS`) from `post_source_` into `post_fx_target(post_size_)` (the ping-pong pair depth of field and motion blur use), then `post_source_` = that target; never in place, because with TAA `post_source_` is the history slot. The shader (GLSL 450, constant loop bounds):
  - a grid of cells (about 6 across at `size` 1, scaled by `aspect`), the cell hash from `lens_rain_hash` chooses a drop position inside the cell, its radius, its fall phase and whether it exists (`amount * density` is the probability); a drop's centre creeps down at `fall_speed` and is re-seeded when it leaves the cell;
  - inside a drop (`d < r`) the sample offset is `-(p - c) * refraction * k` with `k` a smooth dome profile, a mirrored magnified lookup of the HDR source; a thin rim brightens by `1 + 0.5 * rim`;
  - a trail is a vertical smear of the source above a big drop, weighted by `trail`;
  - outside the drops the picture is a 9-tap tent blur of the source weighted by `blur * amount`, constant taps;
  - `out = mix(src, drops_and_blur, coverage)`; `coverage` 0 everywhere when `amount` is 0.
  A uniform block `LensRainU` (bank `kPass`; 3 `vec4`) is declared in `forward3d.cpp` next to `PostU` and in `shaders3d.h` with `static_assert`s on size and offsets. No change to `PassU`, `FrameU` or any existing block.
- **Per backend.** Plain GLSL 450 like every pass; only the banks the post passes use (`kAlbedo` for the source), one uniform block, no SSBO, no compute, no derivatives in non-uniform control flow, explicit `textureLod` (GL, SDL GPU through SPIR-V and MSL, Metal through `translate_glsl`, WebGPU through its WGSL cross compile).

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 42a. Seam: setting, stats and members | `render::LensRain` and `PostFx::rain` in `scene3d.h`; `Forward3DStats::lens_rain` / `lens_rain_amount`, the private pipeline member and the `record_lens_rain` declaration in `forward3d.h`; a compile test of the defaults; the `include_fanout.py` numbers in the PR description. Data only | render | Sonnet |
| 42b. Pure helpers and quality | `mce/render/lens_rain.h` / `src/lens_rain.cpp` (`lens_rain_wetness_step`, `lens_rain_frame`, `lens_rain_hash`), `QualitySettings::lens_rain` / `lens_rain_layers`, `r.lens_rain`, the clamps; `tests/render/test_lens_rain.cpp` (hash goldens, wetting and drying curve, the off cases, the tiers, the cvar) | render | Sonnet |
| 42c. The pass | `kLensRainFS`, `LensRainU` and its `static_assert`s, the pipeline, `record_lens_rain` after `record_flare`, stats; `tests/render_gl/test_render3d_lens_rain_gl.cpp` (GL and SDL GPU), goldens `lens_rain_drops` and `lens_rain_off` in `linux-gl` and `linux-sdlgpu`, the `test_shader_spirv` and `test_shader_cross` cases | render, tests | **Opus** (independent Opus review) |
| 42d. Kit component and feed | `LensRain3D` with `wet_q`, its tick system, registration in `install_kit3d`, zeroed pad and `test_padding` entry, the `engine_components.cpp` includes, the `capabilities` row and a `sample_coverage.allow` line until 42e, the MCP transcript golden bump; the feed in `kit3d_extract.cpp`; `tests/kit/test_lens_rain.cpp` (wetting and drying over ticks, save round trip, hash of the world with and without) and `test_lens_rain_extract.cpp` | kit | Sonnet |
| 42e. Sample | `samples/12_weather` gets a `LensRain3D` if its weather cycle reaches rain at a capture tick (42e checks), else a new `kit3d_tour` scene `tour_lensrain.mcscene` (named so it sorts after `tour.mcscene`) with a deterministic generator; the sample's expectations, the allow-line removed | samples | Sonnet |
| 42f. Docs and close | the `08-3d` post-processing paragraph (what the pass is, wetness, the tiers, `r.lens_rain`, the component and its `wet_q`), the render API table row for `PostFx`, milestone status, follow-ups, ROADMAP row, changelog | docs | Sonnet |

42c is the only step that writes a shader or a draw path: Opus-written, independent Opus review (the author does not review it). Checklist: the off-check, the order against the exposure histogram and bloom, no write into the TAA history, the `LensRainU` mirror, the hash port, constant loop bounds, the backend limits. 42a, 42b, 42c, 42d, 42e, 42f stack in order; 42d may start after 42a. To save CI cycles 42a+42b ship as one PR, 42c+42d as a second, 42e+42f as a third.

## Gates

- With `rain.enabled` false, the Low tier, `r.lens_rain 0` or no `LensRain3D`, every existing golden and sample hash is byte-identical through 42e (only the 42e sample's expectations change).
- Wide headers touched only in 42a (`scene3d.h`, `forward3d.h`); `mce/render/lens_rain.h` and `mce/kit/lens_rain.h` are included only by their `.cpp`, `forward3d.cpp`, `quality.cpp`, the extract, the install and the tests.
- `wet_q` is the only state; a kit test hashes the world with and without the component over a run and checks a save and a replay resume the same wetness.
- Slow tests: GPU cases render one small frame (64×64 or 128×128); any case over 60 s ships a smoke variant and `mce_mark_slow`.
- Generated files change only as far as 42d's and 42e's own checks need.

## Risks

- **Look.** The drop size, the dome profile, the rim and the blur are look choices only a real display can judge; llvmpipe pixels prove placement, determinism and the off checks. Goes to the GPU list.
- **Drop flicker.** Re-seeding a drop at the cell edge could pop; the fall phase fades the drop in and out over its first and last tenth, and a test renders two frames a few ms apart and bounds the change.
- **Cost.** One fullscreen pass with up to 9 + 2·layers taps; skipped entirely when dry, Low off. The real cost goes to the GPU list.
- **Wetness state.** `wet_q` makes this the first post effect with saved state; the replay test above is the guard, and the system is integer-only.

## GPU verification

Headless (llvmpipe, lavapipe): off equals the plain frame exactly; `amount` > 0 changes pixels, and only inside drop discs plus the blur; the same seed and time give the same frame, a different seed another; a refracted drop shows the mirrored scene (a split-colour source gives the opposite colour inside the drop); `amount` 1 covers more than 0.3; TAA history stays free of drops (two frames, the second with `amount` 0, equal the plain frame). Real-GPU look and cost: row A15 of `/mnt/project-files/assessments/gpu-gereken-isler.md`.

## Follow-ups

(none yet)
