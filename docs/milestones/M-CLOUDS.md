# M-CLOUDS — A cloud layer in the sky that follows the weather (milestone record)

**Status:** planned 2026-10-21, 9a to 9c done, step 9d next. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER, M-DAYNIGHT and M-WINDDIR: the weather turns overcast, rains and storms, the sun and ambient dim, and the sky stays an empty gradient. M-WEATHER listed clouds as not in scope; this is the cheap, shader-only version (no volumetrics, no geometry). No Linear issues (free issue limit); this record is the tracker. Render steps (9b) are Opus-written and merged after an independent Opus review plus a green `--task` (Sami, 2026-10-20: "İzin veriyorum sormayın").

**Goal.** A procedural cloud layer drawn in the sky pass: broken white clouds in fair weather, a thick grey deck in overcast, rain and storm, drifting along the scene wind direction at the wind's speed, lit by the sun and dimmed at night, deterministic, saved with the scene, opt-in, and a scene without it renders exactly as today.

**Not in scope:** volumetric or raymarched clouds, cloud shadows on the ground, god rays through clouds, clouds in the IBL bake or in reflections (the layer is drawn into the main sky pass only; probe and GI captures stay cloudless, as with snow and ripples), clouds under an equirect sky texture, precipitation falling from specific clouds.

## Design

- **Render carrier (seam).** `render::Clouds {coverage, density, scale_m, speed, wind_dir (xz), height_m, enabled, pad_}` in `scene3d.h` next to `Sky`, `set_clouds()` / `clouds()`, `QualitySettings::clouds` and cvar `r.clouds` (Low drops them), by-value `apply_quality` overload (NaN to 0, ranges clamped, `wind_dir` normalized with `unit_wind_dir`).
- **Sky shader (Opus).** One `vec4 p_cloud` (coverage, density, 1 / scale, height) and one `vec4 p_cloud2` (the drift `wind_dir * speed * 10 m/s * time` in noise cells, wrapped on the CPU in f64 to the noise period; 0, 0) appended after `p_snow2` in `Forward3DPassU` and `MCE_GLSL_PASS_U`; GLSL in `kSkyFS` only, behind `p_cloud.x > 0.0` so a cloudless frame is bit-identical. The ray hits a plane `height_m` above the camera (only for `d.y > 0`), a 3-octave periodic integer-hash value noise (same hashing family as the puddle and snow noise, periodic so there is no float precision drift) is thresholded by coverage into a mask with a soft edge; the cloud is lit by the sun (brighter toward the sun side, a silver lining near the disk, dark base under a thick deck) and its color is the sky's own radiance in the cloud direction scaled by the light color, so day, dusk and night follow without new inputs. It fades to the horizon haze so it never cuts at the plane's far edge. The sun disk is dimmed by the cloud mask.
- **State (kit).** New component `CloudLayer` (`mce/kit/cloud_layer.h`, one per scene, registered by `install_weather`): authoring `strength` 1, `fair_coverage` 0.35, `overcast_coverage` 0.9, `scale_m` 900, `speed_scale` 1, `height_m` 2000. The extract derives coverage as the blend of `fair_coverage` toward `overcast_coverage` by `Weather.current` kind (Clear 0, Overcast 1, Rain 1, Storm 1, Snow 0.9) and `Weather.intensity`, the wind from `Weather.wind` and `WindDirection` (absent: +x), and the time from `Scene3D::time()`.
- **Luau.** `weather.set_clouds{...}` and getters; `samples/12_weather` gets a cloud layer.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 9a. Render data | `render::Clouds` POD, `set_clouds()`, `QualitySettings::clouds`, `r.clouds`, `apply_quality` overload, test | render, tests/render | Sonnet; seam PR for `scene3d.h` |
| 9b. Sky shader | `p_cloud` / `p_cloud2`, GLSL in `kSkyFS`, `cloud_sky` golden and pixel tests, all goldens byte-identical | render, tests/render_gl | **Opus writes**, independent Opus review |
| 9c. Kit | `CloudLayer`, extract from Weather, WindDirection and time, padding audit, save test | kit, tests/kit | Sonnet |
| 9d. Luau, sample, docs | `weather.set_clouds`, `samples/12_weather`, guide, close | script, samples | Sonnet |

## Gates
- A scene without `CloudLayer` (or without `Weather`) gives `Scene3D::clouds()` with coverage 0 and an unchanged state hash; every existing golden under `tests/render_gl/*` stays byte-identical.
- New golden `cloud_sky` with pixel tests: clouds change only sky pixels (not geometry), coverage 0 and Low equal the cloudless frame, more coverage covers more sky, the cloud layer moves with time and a 180 degree wind turn moves it the other way, an origin shift of 1000 m leaves the sky unchanged (the layer is camera-relative in xz and keyed to the camera position only through the wind drift, document which), a night sky stays dark.
- Kit: all new state is authoring (the layer has no engine-written state; time comes from the scene clock); a scene without it hashes as today.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 9a | merged | #671 | `render::Clouds`, `set_clouds()`, `QualitySettings::clouds`, `r.clouds`, `apply_quality` overload, `test_cloud_quality` |
| 9b | done | #672 | `p_cloud` (coverage, density, 1 / cell, height) and `p_cloud2` (xy: the drift in noise cells, wrapped in f64 on the CPU to the 64-cell period, so no float drift as the clock runs; zw 0) after `p_snow2`, filled by `cloud_params` (forward3d.cpp; drift = `wind_dir * speed * 10 m/s * Scene3D::time()`); the layer is camera-relative (neither the camera's xz nor the floating origin enters; a rebase is bit-identical, a camera 1000 m away in render space moves 64 / 46 of 14524 sky pixels by more than 2 luma on GL / SDL GPU, f32 ray precision only); every existing golden (35 per backend) byte-identical on both backends but `static_merge_kit`, which this machine's rebaseline changes with or without the step; new golden `cloud_sky` (coverage 0.5); `test_render3d_cloud_gl`: sky pixels changed by more than 2 luma at coverage 0.2 / 0.4 / 0.7 / 1: 1421 / 4534 / 10846 / 13277 of 14524, none at or below the horizon; 10 s of drift moves the pattern 3 px right (wind +x) / 3 px left (wind -x); overcast night sky mean luma 7.3 (day 167.9) |
| 9c | done | TBD | `CloudLayer` (`mce/kit/cloud_layer.h`, six f32, no padding; `install_weather`), extract in `Kit3DExtract` (coverage = (fair + (overcast - fair) x kind x intensity) x strength with kind Clear 0, Overcast / Rain / Storm 1, Snow 0.9; speed = `Weather.wind` x `speed_scale`; wind from the first `WindDirection`, +x without; through `apply_quality`); `test_cloud_layer`, padding entry |
| 9d | planned | | needs 9b, 9c |
