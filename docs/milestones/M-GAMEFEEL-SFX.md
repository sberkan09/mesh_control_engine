# M-GAMEFEEL-SFX: sounds for the sample's game events

**Status:** plan (2026-11-06). Closes the "throw sound, blast effect", "melee sound and hit feedback" and "pickup sound" items left
open by [M-GRENADE](M-GRENADE.md), [M-MELEE](M-MELEE.md) and [M-AMMOPICKUP](M-AMMOPICKUP.md). Sits beside
[M-OPENWORLD-AUDIO](M-OPENWORLD-AUDIO.md) (engines, shots, footsteps, sirens) and [M-HITMARKER](M-HITMARKER.md) (hit and kill ticks).
Game layer only: `samples/09_openworld` plus one new kit module. No ECS, render or audio core.

## Why

Throwing a grenade, its blast, a melee swing, picking up cash or an ammo crate and finishing a goal all happen in silence. The events
exist (`GrenadeThrown3D`, `GrenadeBlast3D`, `MeleeSwung3D`, `CashPickedUp3D`, `AmmoPickedUp3D`, `GoalDone3D`) but no component turns an event
into a sound, and `SfxTrigger` fires on a component condition, not on an event.

## Design

`EventSfx3D` on one entity (the player): an `AssetRef<Sfx>` and a gain for each event, a null clip is silent. System `kit_event_sfx`
(FixedUpdate_Post, after `audio_bridge`'s sibling `kit_world_audio`) reads the six events and pushes one `audio::PlaySfx` each;
blasts and pickups play 3D at the event's point, the rest 2D. A cooldown per event kind (default 0.05 s) stops a burst of
events from stacking on one tick. No state beyond the cooldowns, padding-free with a `static_assert(sizeof)`.

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `EventSfx3D`, system `kit_event_sfx`, unit tests (each event pushes one `PlaySfx`, a null clip is silent, cooldown, 3D position for blast and pickup). No sample change. | open |
| 2 feature | Sample: clips from the existing CC0 set (throw: `hit_1` quiet; blast: `crash_2` +3 dB; melee: `hit_3`; cash: `hit_tick`; ammo: `hit_tick` lower; goal: `kill_ding`), `test_mcp_openworld` inspect check, regenerated assets, temporary sample_coverage line removed. | open |
| 3 polish | User guide, closing doc, strike-throughs in M-GRENADE, M-MELEE, M-AMMOPICKUP and M-OPENWORLD-GAPS. | open |

## Decisions taken (change them in the plan PR if wrong)

- Reuse the sounds already in the sample; a dedicated whoosh and explosion need a new CC0 fetch rule and a listen on real speakers,
  so they are a later swap of the clip fields (no code change).
- One component for all game events, not one per feature, so the next event costs a field, not a module.
- Not heard here: the cloud has no output device; the unit tests check the `PlaySfx` events, loudness is a first guess.

## Notes

- Step 1 adds a component: the golden component count, `capabilities.md` and the three `engine_components.cpp` files move with it.
- Linear: the Backlog holds only work that needs real devices or accounts, so this file is the record.
