- Night sky feed, M-NIGHTSKY step 14e: `Kit3DExtract` fills `render::NightSky` from the `Moon`, `WorldClock`, the cloud
  coverage and the fog: moon direction toward the moon, phase, disk size and intensity, colour, star density, brightness and
  twinkle, `star_time`, and `night_fade = (1 - smoothstep(-0.1, 0.1, sun_elevation)) * (1 - cloud_coverage) * (1 -
  smoothstep(0, 0.05, fog_density))`, through `apply_quality`. Not set without a `Moon`, when disabled, without a clock or
  under an equirect `sky_texture`. While the moon holds the directional light the sky's own sun disk is dropped (no flat
  second disk on the phased moon). Scenes without a `Moon` are unchanged. See guide 9.19j.
