# M-CLOUDSHADOW — Cloud shadows on the ground (milestone record)

**Status:** planned 2026-11-10 (plan only, nothing built). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-CLOUDS ("cloud shadows on the ground" was not in scope), M-SUNSHAFTS and the open-world look. No Linear issues (free issue limit); this record is the tracker.

**Naming:** no type, file or cvar called `CloudShadow`, `cloud_shadow` or `M-CLOUDSHADOW` exists (`src/`, `docs/`, `tests/`, `samples/` checked 2026-11-10; the only hits are the M-CLOUDS "not in scope" line).

**Goal.** Drifting patches of shade on the terrain, buildings and water that match the clouds in the sky: where the sky layer has a thick cloud between a point and the sun, the point loses part of the sun. The patches move with the same wind and clock as the clouds, the shade deepens with the weather's coverage, and a scene without the component renders exactly as today.

**Not in scope:** shadows of clouds on other clouds, shadows in the IBL bake, GI captures and reflection probes (they stay cloudless, as with the sky layer), shadows from a moon, a physically based sun term (the pass scales the frame, it does not know which light a pixel received), cloud shadows through the volumetric fog or under water.

## Design

- **Why a post pass.** The lit shaders `kMeshFS` (62954 bytes) and `kTerrainFS` (62976) sit under the 65536-byte string literal limit, and all 16 sampler banks of the lit pass are taken (M-LIGHTCOOKIE). A per-fragment cloud lookup there would need the sky's noise copied into both and a texture or a large splice. A post pass on the HDR frame has neither limit and needs no change to the lit shaders.
- **The pass.** Runs on the HDR frame after the exposure histogram and before the sun shafts, the flare and bloom. For each non-sky pixel the world position is rebuilt from the prepass depth; the point is moved along the sun direction to the cloud plane (`height_m` above the camera, same plane as the sky layer, same camera-relative xz); the sky's periodic integer-hash noise (`p_cloud`, `p_cloud2` drift) is read there and the same coverage threshold gives a cloud density `c` in 0..1. The pixel is multiplied by `1 - strength * c * sun_share`, where `sun_share` (0..1) is the sun's share of the frame's light, taken from the sun and ambient intensities in `Scene3D` (so a dim overcast or dusk frame darkens less than a bright noon). Sky pixels are untouched. The edge is soft (a smoothstep over the cloud's own edge) and the effect fades with the sun's elevation (`light.y`) toward the horizon, where the projected point leaves the layer.
- **Render carrier (seam).** `render::CloudShadow {strength, softness, enabled, pad_}` in `PostFx` (`scene3d.h`, wide header: its own seam PR), `QualitySettings::cloud_shadows`, cvar `r.cloud_shadows`; Low off, Medium a half-size noise (2 octaves), High 3 octaves. `apply_quality` clamps (NaN to 0).
- **Pure helpers** in `mce/render/cloud_shadow.h`: `cloud_shadow_point(world, sun_dir, cam, height)`, `cloud_density(p, coverage, ...)` (the CPU twin of the sky's noise, shared with the sky test), `cloud_shadow_factor(...)`. The pass shader is tested against them.
- **Kit.** Opt-in `CloudShadow3D` (`mce/kit/cloud_shadow.h`: `strength` 0.6, `softness` 0.3; no state, the clouds come from `CloudLayer` and the weather, `kit3d.h` untouched). A scene without a `CloudLayer` gets no shadow. `Kit3DExtract` feeds `PostFx::cloud_shadow` and nothing is hashed.
- **Luau.** `weather.set_cloud_shadow{strength=, softness=}` and getters.
- **Sample.** The `tour_cloudshadow` scene in `kit3d_tour` and `samples/12_weather`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 51a. Seam | `render::CloudShadow` in `PostFx`, `QualitySettings::cloud_shadows`, `r.cloud_shadows` | render, tests/render | Sonnet; seam PR for `scene3d.h` |
| 51b. Helpers | `cloud_shadow.h` (point, density, factor), `apply_quality` overload, tests | render, tests/render | Sonnet |
| 51c. Pass | the post pass, quality tiers, stats, `cloud_shadow` golden and pixel tests | render, tests/render_gl | **Opus writes**, independent Opus review |
| 51d. Kit | `CloudShadow3D`, extract feed, padding audit | kit, tests/kit | Sonnet |
| 51e. Luau, sample | `weather.set_cloud_shadow`, `tour_cloudshadow`, `samples/12_weather` | script, samples | Sonnet |
| 51f. Docs, close | user guide, `capabilities.md`, record | docs | Sonnet |

## Gates
- A scene without `CloudShadow3D` (or with `r.cloud_shadows 0`, or on Low) is bit-identical to today; every existing golden stays byte-identical.
- New golden `cloud_shadow` with pixel tests: only non-sky pixels change; more coverage shades more of the ground; the patches move with time and a 180 degree wind turn moves them the other way; a rebase of the floating origin is bit-identical; the sun near the horizon fades the effect to nothing; the sky stays as it is.
- The shader agrees with the CPU helpers on a grid of sample points.
- GL validation clean; GPU cost of the pass recorded per tier.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 51a to 51f | not started | | plan PR |
