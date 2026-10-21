# M-WINDDIR — A wind with a direction (milestone record)

**Status:** planned 2026-10-21, step 8a next. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER, M-WINDSWAY and M-SNOWCOVER: `Weather.wind` is a strength only, foliage sways along a per-batch `wind_direction` the author sets by hand, and the snow layer leans toward a hard-wired +x. A storm that turns, or a scene whose wind blows west, cannot be authored once for the whole world. No Linear issues (free issue limit); this record is the tracker.

**Goal.** One scene-level wind direction, authored in degrees and optionally drifting, that `Foliage3D` sway and the snow lee bias both follow, deterministically, saved with the scene, opt-in, and a scene without it renders and hashes exactly as today.

**Not in scope:** changing `Weather` (its bytes would change every weather sample's hash for no reason), per-region wind, wind affecting physics bodies or particles, a wind audio loop, gust fronts that travel across the map.

## Design

- **State (kit).** New component `WindDirection` (`mce/kit/wind_direction.h`, one per scene, registered by `install_weather`): authoring `degrees` 0 (0 = +x, counter-clockwise seen from above, wrapped to 0..360), `drift_degrees_per_minute` 0, and the engine-written, saved `angle_q` (degrees in 1/65536 units as an integer, advanced per tick from the drift, so no float drift) with an explicit `pad_`. A system `kit_wind_direction` (FixedUpdate_Pre, after `kit_weather`) advances it. The resolved unit vector is computed from `angle_q` with a deterministic table or `sin`/`cos` of a quantized angle in the extract, never stored.
- **Render carrier (seam).** `render::SnowCover` gets `wind_dir` (xz unit vector, default +x, so every existing value is bit-identical) in `scene3d.h`; `apply_quality` normalizes it (NaN or zero length falls back to +x).
- **Snow shader.** The lee bias uses `dot(ng.xz, wind_dir)` instead of `ng.x`, behind the same `p_snow` branch; with `wind_dir = (1, 0)` the result is bit-identical to today (all `snow_plaza` goldens byte-identical). Opus writes, independent Opus review.
- **Foliage.** The extract rotates each batch's `wind_direction` by the `WindDirection` angle only when a `WindDirection` and a `Weather` exist; the batch cache and `entry->version` stay untouched (same rule as the wind sway), so a direction change never re-scatters.
- **Luau.** `weather.wind_direction()`, `weather.set_wind_direction{degrees, drift}` and getters; `samples/12_weather` gets a slowly turning wind.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 8a. Kit state | `WindDirection`, `kit_wind_direction`, padding audit, determinism and save tests | kit, tests/kit | Sonnet |
| 8b. Render carrier | `render::SnowCover::wind_dir`, `apply_quality` normalize, test (seam PR for `scene3d.h`) | render, tests/render | Sonnet |
| 8c. Snow shader | lee bias from `wind_dir`, `snow_plaza` goldens byte-identical, a new `snow_plaza_west` golden | render, tests/render_gl | **Opus writes**, independent Opus review |
| 8d. Extract | snow wind direction and foliage rotation from `WindDirection` | kit | Sonnet |
| 8e. Luau, sample, docs | the setter and getters, `samples/12_weather`, guide, close | script, samples | Sonnet |

## Gates
- A scene without `WindDirection` has `SnowCover::wind_dir == (1, 0)` and an unchanged state hash; every existing golden under `tests/render_gl/*` stays byte-identical.
- Kit: `angle_q` is integer and saved; a save/load mid-drift resumes with the same state hash; 360 degrees of drift returns to the start angle exactly.
- Rotating the wind by 180 degrees mirrors the snow lee side in the new golden (pixel test), and the sway direction of a foliage batch follows the angle.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 8a | done | #663 | `WindDirection` also carries a saved `frac` (drift remainder) and `seeded` flag |
| 8b | done | #664 | |
| 8c | planned | | needs 8b |
| 8d | planned | | needs 8a, 8b |
| 8e | planned | | needs 8c, 8d |
