# M-WINDSWAY — Foliage that sways with the weather's wind (milestone record)

**Status:** done 2026-10-19 (step 1 merged, step 2 implemented). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-WEATHER: `Weather.wind` (0..1, a function of the weather state) is computed and exposed to Luau, but `Foliage3D` and `TerrainFoliage3D` still sway at their authored constants ("Wind is not applied to Foliage3D here", `weather.h`). No Linear issues (free issue limit); this record is the tracker.

**Goal.** Trees and grass sway gently in clear weather and thrash in a storm, with gusts, from `Weather.wind`, deterministically, saved with the scene, with no per-tick script and no change to a scene that does not opt in.

**Not in scope:** a physical wind field (direction per location, turbulence), wind on cloth, particles or sound (precipitation already slants with `Weather.wind`), changing the foliage vertex shader, wind-driven tree-fall.

## Design

- **No mutation.** The wind is applied where the foliage becomes a render batch: `Kit3DExtract` (`kit3d_extract.cpp`, the two batch sites that copy `wind_amplitude` / `wind_frequency` / `wind_direction` and call `foliage_look`) scales the authored values by a factor derived from `Weather.wind`. The `Foliage3D` / `TerrainFoliage3D` components are never written, so there is no base/applied bookkeeping, no state-hash change and no compounding.
- **Authoring.** New kit component `WindSway` (`mce/kit/wind_sway.h`, one per scene, registered by `install_weather`; `kit3d.h` is not touched): `amplitude_gain` (extra amplitude multiplier at wind 1, default 4), `frequency_gain` (default 1.5), `gust_gain` (adds to `wind_gust` at wind 1, default 0.6), `flutter_gain` (default 3), explicit pad. The factor for a feature is `1 + (gain - 1) * wind`, so wind 0 is the authored look times 1 and a scene with no `WindSway` or no `Weather` is bit-identical to today.
- **Gust and flutter.** `wind_gust = clamp(authored + gust_gain * wind, 0, 1)`; `wind_flutter` scales like the amplitude.
- **Direction.** The authored `wind_direction` is kept (the scene author owns the compass); a later step may add `WindSway.direction_from_weather`.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 1. Kit | `WindSway` component, the scaling in `Kit3DExtract` (both batch sites), registration, kit tests (identity without the component or without `Weather`; scale at wind 0, 0.5, 1; authored values untouched in the world; state hash unchanged), `test_padding.cpp` | kit, tests/kit | Sonnet |
| 2. Luau, sample, docs | `weather.wind_sway{...}` getters/setter in `api_weather.cpp`, `samples/12_weather` gets a foliage patch and a `WindSway`, the headless run pins the batch amplitude per weather kind, guide and milestone closed | script, samples | Sonnet |

## Gates
- A scene without `WindSway` (or without `Weather`) extracts byte-identical batches to today (compare the batch fields in a kit test); every existing golden stays unchanged.
- With `WindSway`, the batch `wind_amplitude` at clear, rain and storm follows `authored * (1 + (gain - 1) * Weather.wind)` exactly (pinned numbers in `tests/kit`).
- `WindSway` is saved and hash-stable; `test_padding.cpp` covers it.
- Risks: the foliage batch cache keys on `entry->version`, so a wind change must not force a re-scatter (the scaling is applied after the cached transforms, per frame, to batch fields only; verify in the test).

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 1 | merged | #614 | `WindSway` + the scaling at both batch sites in `Kit3DExtract`; `tests/kit/test_wind_sway.cpp` |
| 2 | done | | `weather.sway_amplitude()` / `sway_frequency()` / `sway_gust()` / `sway_flutter()` and `weather.set_sway{...}` (clamped to the Range, named errors, nothing written on error) in `api_weather.cpp`, typed in `definitions.cpp`; `samples/12_weather` carries a `WindSway` and a `Foliage3D` patch (60 copies of the building box `platform.glb`, no new mesh asset, on the Weather entity so the entity count stays 11); the script logs the foliage wind factor `1 + (gain - 1) * wind` per weather kind and `weather_test.cmake` pins it (clear 1.300, rain 1.780 at tick 300, rain 2.380 at 600, storm 2.924 at 1500, snow 1.420 at 2100). The headless player has no batch readout, so the factor is the log line (the batch fields themselves are pinned in `tests/kit/test_wind_sway.cpp`). `WindSway` leaves `sample_coverage.allow`. The state hash changes (600 ticks `cf6ff5d99fbcdab5` -> `ebbfa932caeff3b6`, 2700 ticks `6830a72faee5be4d` -> `45aa6c741b08b87a`; neither is pinned, both plays are compared with each other). |
