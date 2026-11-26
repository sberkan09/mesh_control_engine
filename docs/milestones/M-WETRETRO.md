# M-WETRETRO: wet asphalt dims the retro-reflective paint

Follow-up of [M-HEADLIGHT](M-HEADLIGHT.md) and [M-NIGHTSHOT](M-NIGHTSHOT.md), added 2026-11-26, owner: the M-VISUAL routine 2 thread. Render core (one shader line), so the PR waits for coordinator review.

**Why.** M-NIGHTSHOT 2 showed the retro term is identical in rain (5949 changed pixels wet, 5819 dry). Real glass beads lose most of their return under a water film (the film changes the bead's refraction and scatters the light), so a wet marking reads dimmer under a headlight than a dry one.

**Design.** In the Forward+ local-light loop the retro factor is scaled by `1 - 0.6 * min(p_wet.x, 1)`: dry stays bit-identical (`p_wet.x` is 0 when the scene has no wetness), full wetness keeps 40%. No new uniform, no new field, no cook change. The 0.6 is a constant for now (a `.mcmat` key `retro_wet_loss` would be a second material field and is not needed until two paints disagree).

**Glare is not added.** The wet road already pulls roughness toward `wet_roughness` (`apply_wet`), so the existing specular term of the headlight spot gives the glare of wet asphalt; a second glare term would double count.

**Test.** `test_render_gl`: with `darken` 0 (so `apply_wet`'s albedo loss does not halve the gain by itself), the retro gain on the lit box (reflective minus plain) at wetness 1 must lie between 30% and 70% of the dry gain (measured 49 against 88; the tonemapper makes it a little above the linear 40%). Without the shader scale the two gains are equal. The existing headlight test and every golden run with wetness 0 and are unchanged.

**Status.** Implemented in the same PR. The ROADMAP row is the coordinator's to add.
