# M-OPENWORLD-LIGHT — the M-VISUAL looks switched on in the open-world sample

**Status:** done in the cloud (2026-10-31); source: `assessments/gta-gorsel-kalite-3d-uretim-arastirmasi-2026-10-31.md` (step 0a/0c). Real-GPU frame
times are not measured (llvmpipe only). Before/after captures: `screenshots/gta-isik-2026-10-31/` in the project files.

## What changed

| Piece | How |
|---|---|
| Sky | `Environment3D.sky_enabled 1` + `atmosphere 1`: the physically based sky with the clouds, placed by the clock's `time_of_day` |
| Ambient / shade | The `DayCycle`'s dawn, noon and dusk keys now carry about twice the sky and ground ambient (the default 0.35 is a tenth of the sun, so grass in a building's shade rendered black). Night keys are unchanged |
| Contact | `ssao` (0.8 m), `contact_shadows`, `ssr` (windows and wet roads) and a `GiVolume3D` (16 x 4 x 16 probes over the middle 240 x 240 m, added last so the older entities keep their order) |
| Grade | `textures/city_grade.png`: a 256 x 16 colour-grade LUT written by `gen_assets.py` (S-curve, +12 % saturation, warm highlights, cool shadows), `color_grade_strength 0.8` |
| TAA | Nothing to set: the High tier turns TAA on for every scene whose `fxaa` is 1 (the default) |

Cost follows the quality tiers (`render/quality.h`): Low drops SSR, contact shadows, volumetric fog and the GI runtime updates (baked volume only), uses one
cascade and FXAA; Medium and High keep the rest, High adds TAA. `r.ssao 0`, `r.ssr 0`, `r.gi 0` and `r.contact_shadows 0` switch single features off per player.

## Engine changes (`Kit3DExtract`, no header)

Both are render-side copies; `Environment3D` is never written, and scenes without an atmosphere draw as before.

1. **A DayCycle owns the ambient.** With `atmosphere` on and an enabled `DayCycle` writing the environment (`time_of_day >= 0`), the atmosphere no longer
   overwrites `ambient_sky` / `ambient_ground`: it still draws the sky and tints the sun. The atmosphere's own ambient was close to black in the shade; this
   closes the open item of [M-OPENWORLD-DAYNIGHT](M-OPENWORLD-DAYNIGHT.md) ("drive the atmosphere's own ambient").
2. **The atmosphere yields to the moon.** The atmosphere takes its sun from the directional light's direction, and at night the moon holds that slot, so
   it painted a daylight sky around the moon. While the moon holds the light the atmosphere is off and the sky clears to the fog colour (the night sky draws
   the stars and the moon), exactly the pre-atmosphere night.

`tests/kit/test_atmosphere_extract.cpp` covers both.

## Findings and open items

- **`sky_ibl` stays off here.** With the atmosphere sky, IBL left shaded surfaces black (the SH irradiance of the baked atmosphere sky is near zero; scaling
  `Sky::diffuse_ambient` did not change it). IBL on the gradient sky works. Needs a render-core look (bake units of the atmosphere sky).
- **`auto_exposure` stays off here.** It aims at middle grey, so a dark night is lifted to look like day (Environment3D has no min/max exposure; `PostFx::AutoExposure`
  has `min_exposure` / `max_exposure` but the extract leaves them at 0.03 / 32). Exposing a clamp (a header seam PR) would allow it.
- **Dusk hand-off is abrupt (follow-up).** The atmosphere yields to the moon on the same discrete flag as the light itself (`moon_holds_light`), so the sky switches from the atmosphere to the fog colour in one frame. The sun is below the horizon then and the atmosphere sky is nearly dark, but the transition frames were not captured. To do: smooth it (fade by the moon weight) and verify the transition frames with captures. Separate task, not in this PR.
- The grass is still dark in deep shade and the trees are flat-shaded low poly: art direction (see the assessment), not lighting.
- Not done: volumetric fog (cost; the exp² fog stays), lamp-post light pools at night were not re-checked.

## Follow-up (2026-10-31): twilight, sky IBL, auto exposure

Closes the three open items above. Shipped as a seam PR (wide headers) then the feature PR.

| Step | PR | Status |
|---|---|---|
| Seam: `Environment3D.auto_exposure_min/max` (kit3d.h), `Atmosphere::sky_sun_direction/sky_sun_light` (atmosphere.h), Scene3D honours them | (this PR) | review |
| Feature: smooth dusk hand-off, sky IBL level, auto exposure on in 09_openworld | — | open |

## Verification

- Xvfb + llvmpipe captures, same camera and ticks (90, 500) before and after, plus 21:30 after: shade readable, sky with horizon and clouds, windows reflect, night dark with the moon.
- `test_kit` (new atmosphere cases), `openworld_sample_player` (hash replay), `scripts/ci/local_ci.sh --task`.
