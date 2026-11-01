# M-PARTICLEUSER — A user-facing particle knob: `r.particles` scales every weather particle, rain and snow included (milestone record)

**Status:** planned (2026-11-01), steps 29a to 29c. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of [M-PARTICLEQ](M-PARTICLEQ.md), whose Follow-ups list a user-facing `r.particles` console variable and routing the rain and snow precipitation through `particle_scale`. No Linear issues (free issue limit); this record is the tracker.

**Goal.** A player (or a phone profile) can trade particles for frame time without changing the whole tier: one saved console variable `r.particles` (0..1, default 1) multiplies `QualitySettings::particle_scale`. The weather rain and snow emitters (`precipitation.cpp`) read the same scale, so Low also lightens a storm, which the six opt-in emitters already do.

**Not in scope:** a non-zero Medium value (needs a device measurement), changing the particle simulation or the render particle pass, non-weather particle systems.

## Design

- **Seam (render, 29a).** `quality.cpp` gains `MCE_CVAR(cv_particles, "r.particles", 1.0f, ..., CVarFlags::Saved)`; `quality_settings` multiplies `particle_scale` by the clamped value (NaN reads 1). No header change: the field already exists, so the seam is one `.cpp` and a test beside `test_particle_quality.cpp`. Default 1 leaves every tier bit-identical.
- **Kit (29b).** The precipitation emitter budget (`precipitation.cpp`, `rate` and `max_particles`) is multiplied by `particle_scale_of(quality_)` through the same `scaled_budget` rule (never below 1 while the scale is above 0; 0 switches the precipitation emitter off). Low now halves rain and snow too. A scene with no precipitation is unchanged. One extract case pins scale 1, 0.5 and 0.
- **Determinism.** Only a local render emitter's numbers change; nothing is written or hashed.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 29a. Seam | `r.particles` cvar read in `quality_settings`; test beside `test_particle_quality.cpp` | render | Sonnet |
| 29b. Precipitation | rain and snow rate and budget follow `particle_scale`; one extract case | kit | Sonnet |
| 29c. Docs and close | user guide quality paragraph (`r.particles`), milestone status, siblings' follow-up lines | docs | Sonnet |

No step needs Opus: nothing writes a shader.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 29a | planned | | |
| 29b | planned | | |
| 29c | planned | | |

## Follow-ups

- A non-zero Medium `particle_scale` once a real device measurement says the weather particles cost frame time.
