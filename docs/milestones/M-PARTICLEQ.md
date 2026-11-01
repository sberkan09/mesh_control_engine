# M-PARTICLEQ — A real particle quality knob: the render-side weather particles scale with the quality tier through `QualitySettings`, not through a recognised Low profile (milestone record)

**Status:** done (2026-11-01), steps 28a to 28c (PRs 991, 992, 994, 995). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-LEAFFALL, M-FIREFLIES, M-POLLEN, M-DUSTWIND, M-BIRDFLOCK and M-BUTTERFLY, whose records all list "a real particle quality switch in `quality.h`" as an open follow-up. Today `Kit3DExtract` halves the rate and budget of the six opt-in emitters on the Low tier by calling `is_low_quality`, which *recognises* Low from the settings it alone sets (single-sample, no GI updates, no volumetrics) because `QualitySettings` has no particle field. That guess breaks the moment another tier shares those three settings, and a phone cannot pick a particle budget separately from the rest of the tier. No Linear issues (free issue limit); this record is the tracker.

**Goal.** `QualitySettings` gets one field, `particle_scale` (0..1, default 1): the share of the authored emitter rate and `max_particles` a render-side weather emitter keeps. Low is 0.5, Medium and High are 1. `Kit3DExtract` reads `quality_.particle_scale` in every block that now calls `is_low_quality` and drops that function. The result is bit-identical to today on every tier (Low halves, the others do not), but the knob is now explicit, testable and tunable per tier.

**Not in scope:** a user-facing `r.particles` console variable (a later step if a phone wants it), changing the Low halving to another factor, changing the particle simulation or the render particle pass, and the non-weather particle systems.

## Design

- **Seam (render, 28a).** `quality.h` gains `f32 particle_scale = 1.0f;` next to the other weather knobs, with the comment "M-PARTICLEQ 28a: the share of rate and max_particles a render-side weather emitter keeps (0.5 on Low)". `quality_settings(Low)` sets 0.5; Medium and High keep 1. A small test `tests/render/test_particle_quality.cpp` pins the three tiers (mirroring `test_meteor_quality.cpp`). `quality.h` is included by about 50 translation units, below the wide-header warning, and the change is one field, so it ships alone and first.
- **Kit (28b).** Each block that does `const bool low = is_low_quality(quality_)` becomes `const f32 scale = quality_.particle_scale` and multiplies the rate and the clamped `max_particles` by it (rounding as the halving does today: `max_particles` stays at least the block's lower clamp). `is_low_quality` is deleted. The existing "Low tier halves" extract tests keep passing unchanged, and one new case sets `particle_scale` by hand on a Medium-tier `QualitySettings` and checks the budget follows it.
- **Determinism.** The scale only changes a local render emitter's numbers; nothing is written or hashed, and a scene without the opt-in components is byte-identical.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 28a. Seam | `particle_scale` in `QualitySettings` and `quality_settings`; `tests/render/test_particle_quality.cpp`; the quality table in the render docs | render | Sonnet |
| 28b. Kit switch | every `is_low_quality` caller reads `particle_scale`; `is_low_quality` removed; one new extract case | kit | Sonnet |
| 28c. Docs and close | user guide quality paragraph, milestone status, follow-ups, the six sibling records' follow-up line closed | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 28a | done | 992 | |
| 28b | done | 994 | |
| 28c | done | 995 | The six sibling records' follow-up line closed; the milestone record closed with follow-ups. |

## Follow-ups

None blocks the milestone.
- A user-facing `r.particles` console variable (a phone could trade particles for frame time without changing the tier); the knob is `QualitySettings::particle_scale`, so it is one `MCE_CVAR` and one multiply in `quality_settings`.
- A non-zero Medium value (for example 0.75 on a phone-class Medium) once a real device measurement says the weather particles cost frame time.
- The weather rain and snow particles are drawn by the precipitation path, not the six opt-in emitters, and still use their own budget; routing them through `particle_scale` is open.
