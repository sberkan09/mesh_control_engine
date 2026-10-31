- 09_openworld dusk, sky IBL and auto exposure (M-OPENWORLD-LIGHT follow-up): with a DayCycle and the atmosphere sky,
  `Kit3DExtract` keeps the atmosphere on when the moon takes the light (its sky follows the sun, so the hand-off no
  longer jumps from a dark sky to a flat fog colour and the clouds stay), fades a fog-coloured twilight gradient in
  under the sky as the sun sets, and scales the sky IBL's diffuse to the DayCycle's ambient (shade no longer black).
  Forward3D honours `Sky::texture_additive` under the atmosphere. 09_openworld turns on `sky_ibl` and `auto_exposure`
  (clamped to 0.8..2, so the night stays dark). Scenes without a DayCycle are unchanged.
