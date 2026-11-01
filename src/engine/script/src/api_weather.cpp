// mce_script (M-WEATHER step 4a): the `weather` global, a thin binding over the kit's Weather component
// (mce/kit/weather.h). Like `clock` it holds no state: the kind, the target and the blend live in the Weather
// component, so saves, replays and the editor see them. The functions act on the FIRST Weather in ECS order (a scene
// has one). A world without one gets a named error, never a crash.
//
//   weather.kind() -> string       the kind in effect: clear, overcast, rain, storm or snow (Weather.current)
//   weather.target() -> string     the kind the weather is moving to (Weather.target)
//   weather.set(kind, seconds?)    writes Weather.target; with `seconds` also Weather.transition_seconds (0..3600)
//   weather.intensity() -> number  0..1 blend of the kind in effect
//   weather.wind() -> number       0..1
//   weather.wetness() -> number    0..1
//
// Wet surfaces (M-WETSURFACE step 5d) are a thin binding over the first kit::WetSurfaces (mce/kit/wet_surfaces.h),
// which only tunes the look of the wet world. A world without one gets a named error.
//
//   weather.wet_strength() -> number      WetSurfaces.strength (0..4), scales Weather.wetness
//   weather.wet_darken() -> number        WetSurfaces.darken (0..1)
//   weather.wet_puddles() -> number       WetSurfaces.puddles (0..1)
//   weather.wet_puddle_scale() -> number  WetSurfaces.puddle_scale_m (0.1..100)
//   weather.set_wet{strength=, darken=, puddles=, puddle_scale=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Wind sway (M-WINDSWAY step 2) is the same kind of binding over the first kit::WindSway (mce/kit/wind_sway.h): the
// gains Kit3DExtract scales the foliage wind by (factor 1 + (gain - 1) * Weather.wind). Named errors as above.
//
//   weather.sway_amplitude() -> number    WindSway.amplitude_gain (0..50)
//   weather.sway_frequency() -> number    WindSway.frequency_gain (0..10)
//   weather.sway_gust() -> number         WindSway.gust_gain (-1..1)
//   weather.sway_flutter() -> number      WindSway.flutter_gain (0..50)
//   weather.set_sway{amplitude=, frequency=, gust=, flutter=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Snow cover (M-SNOWCOVER step 6d) is the same kind of binding over the first kit::SnowCover (mce/kit/snow_cover.h).
// Named errors as above.
//
//   weather.snow_cover() -> number        SnowCover.cover (0..1), engine-written by kit_snow_cover; read-only
//   weather.snow_strength() -> number     SnowCover.strength (0..4)
//   weather.snow_build() -> number        SnowCover.build_seconds (0..86400)
//   weather.snow_melt() -> number         SnowCover.melt_seconds (0..86400)
//   weather.snow_coverage() -> number     SnowCover.coverage (0..1)
//   weather.snow_scale() -> number        SnowCover.noise_scale_m (0.1..100)
//   weather.snow_wind() -> number         SnowCover.wind_bias (0..1)
//   weather.set_snow{strength=, build=, melt=, coverage=, scale=, wind=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Rain effects (M-RAINFX step 7d) are the same kind of binding over the first kit::RainRipples (mce/kit/rain_ripples.h),
// kit::RainSplash (mce/kit/rain_splash.h) and kit::ThunderDelay (mce/kit/thunder_delay.h). Named errors as above.
// The engine-written fields (RainSplash.counter, ThunderDelay.pending, due, gain_db) and ThunderDelay.clip are not
// reachable from Luau.
//
//   weather.ripples_strength() -> number  RainRipples.strength (0..1)
//   weather.ripples_density() -> number   RainRipples.density (0..1)
//   weather.ripples_speed() -> number     RainRipples.speed (0.1..10)
//   weather.ripples_ring_width() -> number RainRipples.ring_width (0..1)
//   weather.set_ripples{strength=, density=, speed=, ring_width=}
//   weather.splash_rate() -> number       RainSplash.rate (0..64)
//   weather.splash_height() -> number     RainSplash.height_m (0.01..5)
//   weather.splash_life() -> number       RainSplash.life_seconds (0.02..5)
//   weather.splash_radius() -> number     RainSplash.radius_m (0.5..500)
//   weather.splash_seed() -> number       RainSplash.seed (0..4294967295)
//   weather.set_splash{rate=, height=, life=, radius=, seed=}
//   weather.thunder_speed() -> number     ThunderDelay.speed_mps (1..1000000)
//   weather.thunder_volume() -> number    ThunderDelay.volume (0..4)
//   weather.set_thunder{speed=, volume=}
//                                         each setter takes a table, every key optional, each value clamped to the
//                                         field's Range; an unknown key or a non-number raises an error that names
//                                         the call
//
// Wind direction (M-WINDDIR step 8e) is the same kind of binding over the first kit::WindDirection
// (mce/kit/wind_direction.h). Named errors as above. Only the authored degrees and drift are settable: angle_q, frac
// and seeded are engine-written (kit_wind_direction) and an unknown key names them.
//
//   weather.wind_degrees() -> number      WindDirection.degrees (0..360), the authored heading
//   weather.wind_drift() -> number        WindDirection.drift_degrees_per_minute (-3600..3600)
//   weather.wind_heading() -> number      the resolved heading in degrees, 0..360 (from the engine-written angle_q)
//   weather.set_wind_direction{degrees=, drift=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Clouds (M-CLOUDS step 9d) are the same kind of binding over the first kit::CloudLayer (mce/kit/cloud_layer.h). Named
// errors as above. All six fields are authoring, so all are settable.
//
//   weather.cloud_strength() -> number           CloudLayer.strength (0..1)
//   weather.cloud_fair_coverage() -> number      CloudLayer.fair_coverage (0..1)
//   weather.cloud_overcast_coverage() -> number  CloudLayer.overcast_coverage (0..1)
//   weather.cloud_scale() -> number              CloudLayer.scale_m (50..20000)
//   weather.cloud_speed_scale() -> number        CloudLayer.speed_scale (0..10)
//   weather.cloud_height() -> number             CloudLayer.height_m (100..20000)
//   weather.set_clouds{strength=, fair_coverage=, overcast_coverage=, scale=, speed_scale=, height=}
//                                         a table, every key optional, each value clamped to the field's Range;
//                                         an unknown key or a non-number raises an error that names the call
//
// Rain and wind ambience (M-WEATHERAUDIO step 10c) is the same kind of binding over the first kit::WeatherAudio
// (mce/kit/weather_audio.h). Named errors as above. The two sources and the fade levels belong to the scene and the
// engine, so they are not settable; the levels are read-only.
//
//   weather.rain_level() -> number        the current rain bed level (WeatherAudio.rain_q / 2^24, 0..1)
//   weather.wind_level() -> number        the current wind bed level (WeatherAudio.wind_q / 2^24, 0..1)
//   weather.audio_rain_db() -> number     WeatherAudio.rain_db (-60..12)
//   weather.audio_wind_db() -> number     WeatherAudio.wind_db (-60..12)
//   weather.audio_fade() -> number        WeatherAudio.fade_seconds (0..60)
//   weather.audio_enabled() -> number     WeatherAudio.enabled (0 or 1)
//   weather.set_audio{rain_db=, wind_db=, fade=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (enabled: any non-zero number is 1); an unknown key or a non-number
//                                         raises an error that names the call
//
// Lightning bolts (M-LIGHTNINGBOLT step 11d) are the same kind of binding over the first kit::LightningBolt
// (mce/kit/lightning_bolt.h). Named errors as above; the last strike belongs to the engine and is read-only.
//
//   weather.bolt_min_range() / bolt_max_range() -> number   LightningBolt.min_range_m / max_range_m (10..5000)
//   weather.bolt_ground_y() -> number                       LightningBolt.ground_y
//   weather.bolt_height() -> number                         LightningBolt.height_m (50..3000)
//   weather.bolt_width() -> number                          LightningBolt.width_m (0.05..20)
//   weather.bolt_segments() -> number                       LightningBolt.segments_log2 (2..7)
//   weather.bolt_jitter() -> number                         LightningBolt.jitter (0..0.6)
//   weather.bolt_fork() / bolt_enabled() -> number          0 or 1
//   weather.bolt_light() / bolt_light_radius() -> number    light_intensity (0..100000) / light_radius_m (1..5000)
//   weather.bolt_distance() -> number                       strike_dist_m, the last strike's distance from the camera
//   weather.bolt_pos() -> x, z                              strike_x, strike_z, the last strike's ground position
//   weather.bolt_active() -> number                         active, the flash ticks left
//   weather.set_bolt{min_range=, max_range=, ground_y=, height=, width=, segments=, jitter=, fork=, light=,
//                    light_radius=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (segments rounded down; fork and enabled: any non-zero number is 1); an
//                                         unknown key or a non-number raises an error that names the call
//
// Wind gusts (M-WINDGUST step 12d) are the same kind of binding over the first kit::WindGust
// (mce/kit/wind_gust.h). Named errors as above; the setter never adds the component, and the gust state is
// engine-written and read-only.
//
//   weather.gust_strength() -> number                       WindGust.strength (0..3)
//   weather.gust_min_gap() / gust_max_gap() -> number       min_gap_s / max_gap_s (0..120)
//   weather.gust_min_rise() / gust_max_rise() -> number     min_rise_s / max_rise_s (0.05..30)
//   weather.gust_min_fall() / gust_max_fall() -> number     min_fall_s / max_fall_s (0.05..60)
//   weather.gust_min_peak() / gust_max_peak() -> number     min_peak / max_peak (0..1)
//   weather.gust_seed() / gust_affects() -> number          seed / affects (u32)
//   weather.gust_enabled() -> number                        0 or 1
//   weather.gust_level() -> number                          gust_q / 2^24 (0..1), read-only
//   weather.effective_wind() -> number                      effective_wind(Weather, WindGust, all consumer bits)
//   weather.set_gust{strength=, min_gap=, max_gap=, min_rise=, max_rise=, min_fall=, max_fall=, min_peak=,
//                    max_peak=, seed=, affects=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (seed and affects rounded down; enabled: any non-zero number is 1); an
//                                         unknown key or a non-number raises an error that names the call
//
// Ground mist (M-GROUNDMIST step 13c) is the same kind of binding over the first kit::GroundMist
// (mce/kit/ground_mist.h). Named errors as above; the setter never adds the component. mist_level is derived, read-only.
//
//   weather.mist_strength() -> number                       GroundMist.strength (0..2)
//   weather.mist_peak_hour() -> number                      peak_hour (0..24)
//   weather.mist_rise() / mist_burn() -> number             rise_hours / burn_hours (0.1..12)
//   weather.mist_base_humidity() / mist_wet_gain() -> number  base_humidity / wet_gain (0..1)
//   weather.mist_weather_damp() -> number                   weather_damp (0..1)
//   weather.mist_density() -> number                        density (0..1)
//   weather.mist_thickness() -> number                      thickness_m (0.5..200)
//   weather.mist_base_height() -> number                    base_height_m (-1000..1000)
//   weather.mist_exp2_density() -> number                   exp2_density (0..0.2)
//   weather.mist_color() -> r, g, b                         color
//   weather.mist_enabled() -> number                        0 or 1
//   weather.mist_level() -> number                          kit::mist_level on Environment3D.time_of_day and the first
//                                                           Weather (none: no damping), 0 without a clock; read-only
//   weather.set_mist{strength=, peak_hour=, rise_hours=, burn_hours=, base_humidity=, wet_gain=, weather_damp=,
//                    density=, thickness=, base_height=, exp2_density=, color={r,g,b}, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range (color
//                                         lanes 0..1; enabled: any non-zero number is 1); an unknown key, a non-number
//                                         or a color that is not three numbers raises an error that names the call
//
// Moon (M-NIGHTSKY step 14f) is the same kind of binding over the first kit::Moon (mce/kit/moon.h). Named errors as
// above; the setter never adds the component. The moon_phase / moon_illumination / moon_direction / moon_level values
// are derived with the moon.h helpers on the current clock and are read-only. The clock is the hour mist_level uses
// (Environment3D.time_of_day; -1 = no clock) and the first WorldClock's day (0 without one); the first Weather, if any,
// damps the level. Authoring names follow the component, `inclination` is Moon.inclination_deg (degrees).
//
//   weather.moon_phase_offset() / moon_cycle_days() / moon_freeze_phase() -> number   phase_offset, cycle_days, freeze_phase
//   weather.moon_inclination() -> number                    inclination_deg (0..30)
//   weather.moon_light_intensity() -> number                light_intensity (0..2)
//   weather.moon_light_color() -> r, g, b                   light_color
//   weather.moon_light_shadows() / moon_enabled() -> number 0 or 1
//   weather.moon_disk_size() / moon_disk_intensity() -> number   disk_size (0.99..1), disk_intensity (0..50)
//   weather.moon_star_density() / moon_star_brightness() / moon_star_twinkle() -> number
//   weather.moon_phase() -> number                          kit::moon_phase, 0..1 (0 new, 0.5 full)
//   weather.moon_illumination() -> number                   kit::moon_illumination of that phase, 0..1
//   weather.moon_direction() -> x, y, z                     kit::moon_direction: the unit direction the moonlight
//                                                           travels (negate it for the direction toward the moon)
//   weather.moon_level() -> number                          kit::moon_level, 0..1, 0 without a clock or below the horizon
//   weather.set_moon{phase_offset=, cycle_days=, freeze_phase=, inclination=, light_intensity=, light_color={r,g,b},
//                    light_shadows=, disk_size=, disk_intensity=, star_density=, star_brightness=, star_twinkle=,
//                    enabled=}
//                                         a table, every key optional, each value clamped to the field's Range (color
//                                         lanes 0..1; light_shadows and enabled: any non-zero number is 1); an unknown
//                                         key, a non-number or a color that is not three numbers raises an error that
//                                         names the call
//
// Aurora (M-AURORA step 16d) lives in the same table, next to the moon: the authoring over the first kit::Aurora
// (mce/kit/aurora.h) and two read-only values from its helpers. Named errors as above ("weather.set_aurora: the scene has
// no Aurora entity"); the setter never adds the component. The authoring names follow the set keys with an `aurora_`
// prefix. aurora_gate() is kit::aurora_latitude_gate of the authored latitude (0..1); aurora_level() is
// kit::aurora_level with the inputs Kit3DExtract uses: the night fade of the current hour (the sun's elevation, 0 by day
// and 1 well below the horizon; 0 with no clock, so with no clock only night_only 0 shows) and the cloud coverage of the
// first Weather and CloudLayer (the High tier's: the extract's own value before the quality gate). Both read the
// authoring even when `enabled` is 0 (aurora_level is 0 then).
//
//   weather.aurora_latitude() / aurora_gate_low() / aurora_gate_high() / aurora_strength() / aurora_curtain_count() /
//   weather.aurora_base_height() / aurora_height_span() / aurora_azimuth() / aurora_width() / aurora_speed() /
//   weather.aurora_night_only() / aurora_enabled() -> number   latitude_deg, gate_low_deg, gate_high_deg, strength,
//                                                              curtain_count, base_height, height_span, azimuth_deg,
//                                                              width_deg, speed, night_only, enabled
//   weather.aurora_low_color() / aurora_high_color() -> r, g, b
//   weather.aurora_gate() -> number                 0..1, smoothstep(gate_low, gate_high, abs(latitude))
//   weather.aurora_level() -> number                0..4, the brightness gain the renderer gets now
//   weather.set_aurora{latitude=, gate_low=, gate_high=, strength=, curtain_count=, base_height=, height_span=,
//                      azimuth=, width=, speed=, low_color={r,g,b}, high_color={r,g,b}, night_only=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (curtain_count rounded down; color lanes 0..1; night_only and enabled: any
//                                         non-zero number is 1); an unknown key, a non-number or a color that is not
//                                         three numbers raises an error that names the call and nothing is written
//
// Rainbow (M-RAINBOW step 17d) lives in the same table, next to the aurora: the authoring over the first kit::Rainbow
// (mce/kit/rainbow.h) and the read-only rainbow_level(). Named errors as above ("weather.set_rainbow: the scene has no
// Rainbow entity"); the setter never adds the component. The authoring names follow the set keys with a `rainbow_`
// prefix. rainbow_level() is kit::rainbow_level with the inputs Kit3DExtract uses: the sun elevation of the current hour
// (Environment3D.time_of_day, else its sun_direction), the first Weather's wetness and rain (its intensity while it rains
// or storms), and the cloud coverage of the first Weather and CloudLayer. It is 0 without a Weather. The getters read the
// authoring even when `enabled` is 0 (rainbow_level is 0 then).
//
//   weather.rainbow_strength() / rainbow_band_width() / rainbow_secondary() / rainbow_min_wetness() /
//   weather.rainbow_max_sun_elevation() / rainbow_enabled() -> number   strength, band_width, secondary, min_wetness,
//                                                                       max_sun_elevation_deg, enabled
//   weather.rainbow_level() -> number               0..4, the brightness gain the renderer gets now
//   weather.set_rainbow{strength=, band_width=, secondary=, min_wetness=, max_sun_elevation=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range
//                                         (enabled: any non-zero number is 1); an unknown key or a non-number raises an
//                                         error that names the call and nothing is written
//
// Meteor (M-METEOR step 18d) lives in the same table, next to the rainbow: the authoring over the first kit::Meteor
// (mce/kit/meteor.h) and the read-only meteor_level(). Named errors as above ("weather.set_meteor: the scene has no Meteor
// entity"); the setter never adds the component. The authoring names follow the set keys with a `meteor_` prefix.
// meteor_level() is kit::meteor_level with the inputs Kit3DExtract uses: the night fade of the current hour (0 with no
// clock, so with no clock only night_only 0 shows), the cloud coverage of the first Weather and CloudLayer and the first
// Weather's rain (its intensity while it rains or storms). It is 0 under an equirect sky texture. The getters read the
// authoring even when `enabled` is 0 (meteor_level is 0 then).
//
//   weather.meteor_rate() / meteor_brightness() / meteor_length() / meteor_speed() / meteor_min_darkness() /
//   weather.meteor_night_only() / meteor_seed() / meteor_enabled() -> number
//   weather.meteor_level() -> number                0..1, the level the renderer gets now
//   weather.set_meteor{rate=, brightness=, length=, speed=, min_darkness=, night_only=, seed=, enabled=}
//                                         a table, every key optional, each value clamped to the field's Range (seed
//                                         rounded down; night_only and enabled: any non-zero number is 1); an unknown
//                                         key or a non-number raises an error that names the call and nothing is written
//
// Season (M-SEASONS step 19c) lives in the same table: the authoring over the first kit::Season (mce/kit/season.h) and the
// read-only season_phase(). Named errors as above ("weather.set_season: the scene has no Season entity"); the setter never
// adds the component. The authoring names follow the set keys with a `season_` prefix; the tints return r, g, b.
// season_phase() is kit::season_phase on the first WorldClock's day and hour (day 0 and hour 0 with no clock, so the fixed
// start_phase). The getters read the authoring even when `enabled` is 0.
//
//   weather.season_enabled() / season_year_days() / season_start_phase() / season_winter_density() /
//   weather.season_snow_cover_bias() / season_cold_bias() / season_strength() -> number
//   weather.season_spring_tint() / season_autumn_tint() / season_winter_tint() -> r, g, b
//   weather.season_phase() -> number                0..1, the year phase now
//   weather.set_season{enabled=, year_days=, start_phase=, spring_tint={r,g,b}, autumn_tint={r,g,b}, winter_tint={r,g,b},
//                      winter_density=, snow_cover_bias=, cold_bias=, strength=}
//                                         a table, every key optional, each value clamped to the field's Range (tints
//                                         0..4 per channel; enabled: any non-zero number is 1); an unknown key or a
//                                         non-number raises an error that names the call and nothing is written
//
// Water ice (M-FREEZE step 20c) lives in the same table: the authoring over the first kit::WaterIce (mce/kit/water_ice.h)
// and the read-only water_frozen(). Named errors as above ("weather.set_water_ice: the scene has no WaterIce entity"); the
// setter never adds the component. The getters are named weather.water_ice_<key>; the colour returns r, g, b.
// water_frozen() is kit::water_ice_amount on the first Season at the first WorldClock's day and hour (day 0 and hour 0 with
// no clock; no Season is the manual floor), the same amount the extract draws, 0 when `enabled` or `strength` is 0.
// The getters read the authoring even when `enabled` is 0.
//
//   weather.water_ice_enabled() / water_ice_freeze_start() / water_ice_freeze_full() / water_ice_manual() /
//   weather.water_ice_ice_roughness() / water_ice_ice_reflectivity() / water_ice_ice_depth_m() / water_ice_calm() /
//   weather.water_ice_strength() -> number
//   weather.water_ice_ice_color() -> r, g, b
//   weather.water_frozen() -> number                0..1, the freeze amount now
//   weather.set_water_ice{enabled=, freeze_start=, freeze_full=, manual=, ice_color={r,g,b}, ice_roughness=,
//                         ice_reflectivity=, ice_depth_m=, calm=, strength=}
//                                         a table, every key optional, each value clamped to the field's Range (ice_depth_m
//                                         0.05..10, the rest 0..1, colour lanes 0..1; enabled: any non-zero number is 1);
//                                         an unknown key or a non-number raises an error that names the call and nothing
//                                         is written
//
// Leaf fall (M-LEAFFALL step 21c) lives in the same table: the authoring over the first kit::LeafFall (mce/kit/leaf_fall.h)
// and the read-only leaf_falling(). Named errors as above ("weather.set_leaf_fall: the scene has no LeafFall entity"); the
// setter never adds the component. The getters are named weather.leaf_fall_<key>; the colour returns r, g, b.
// leaf_falling() is kit::leaf_fall_amount on the first Season at the first WorldClock's day and hour (day 0 and hour 0 with
// no clock; no Season is the manual floor), the same amount the extract draws, 0 when `enabled` or `strength` is 0.
// The getters read the authoring even when `enabled` is 0.
//
//   weather.leaf_fall_enabled() / leaf_fall_autumn_start() / leaf_fall_autumn_full() / leaf_fall_autumn_end_full() /
//   weather.leaf_fall_autumn_end() / leaf_fall_manual() / leaf_fall_rate() / leaf_fall_box_xz_m() / leaf_fall_height_m() /
//   weather.leaf_fall_size_m() / leaf_fall_lifetime_s() / leaf_fall_fall_speed() / leaf_fall_wind_follow() /
//   weather.leaf_fall_tint_follow() / leaf_fall_max_particles() / leaf_fall_seed() / leaf_fall_strength() -> number
//   weather.leaf_fall_leaf_color() -> r, g, b
//   weather.leaf_falling() -> number                0..1, the leaf amount now
//   weather.set_leaf_fall{enabled=, autumn_start=, autumn_full=, autumn_end_full=, autumn_end=, manual=, rate=, box_xz_m=,
//                         height_m=, size_m=, lifetime_s=, fall_speed=, wind_follow=, leaf_color={r,g,b}, tint_follow=,
//                         max_particles=, seed=, strength=}
//                                         a table, every key optional, each value clamped to the field's Range (see
//                                         mce/kit/leaf_fall.h; colour lanes 0..1; enabled: any non-zero number is 1);
//                                         an unknown key or a non-number raises an error that names the call and nothing
//                                         is written
//
// Fireflies (M-FIREFLIES step 23c) live in the same table: the authoring over the first kit::Firefly (mce/kit/firefly.h) and
// the read-only fireflies_glowing(). Named errors as above ("weather.set_firefly: the scene has no Firefly entity"); the
// setter never adds the component. The getters are named weather.firefly_<key>; the colour returns r, g, b.
// fireflies_glowing() is kit::firefly_amount on the first Season at the first WorldClock's day and hour, with the inputs the
// extract uses: the night fade of the Environment3D hour (0 with no clock, so only night_only 0 shows) and the first Weather's rain intensity while it rains or storms (0 without a Weather); no Season is the manual
// floor. 0 when `enabled` or `strength` is 0. The getters read the authoring even when `enabled` is 0.
//
//   weather.firefly_enabled() / firefly_summer_start() / firefly_summer_full() / firefly_summer_end_full() /
//   weather.firefly_summer_end() / firefly_manual() / firefly_night_only() / firefly_min_darkness() / firefly_rate() /
//   weather.firefly_box_xz_m() / firefly_height_m() / firefly_size_m() / firefly_lifetime_s() / firefly_drift_speed() /
//   weather.firefly_max_particles() / firefly_seed() / firefly_strength() -> number
//   weather.firefly_color() -> r, g, b
//   weather.fireflies_glowing() -> number           0..1, the firefly amount now
//   weather.set_firefly{enabled=, summer_start=, summer_full=, summer_end_full=, summer_end=, manual=, night_only=,
//                       min_darkness=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, drift_speed=, color={r,g,b},
//                       max_particles=, seed=, strength=}
//                                         a table, every key optional, each value clamped to the field's Range (see
//                                         mce/kit/firefly.h; colour lanes 0..1; enabled and night_only: any non-zero number
//                                         is 1); an unknown key or a non-number raises an error that names the call and
//                                         nothing is written
//
// Pollen (M-POLLEN step 24c) lives in the same table: the authoring over the first kit::Pollen (mce/kit/pollen.h) and the
// read-only pollen_drifting(). Named errors as above ("weather.set_pollen: the scene has no Pollen entity"); the setter
// never adds the component. The getters are named weather.pollen_<key>; the colour returns r, g, b. pollen_drifting() is
// kit::pollen_amount on the first Season at the first WorldClock's day and hour, with the inputs the extract uses: the night
// fade of the Environment3D hour (0 with no clock, so only day_only 0 shows) and the first Weather's rain intensity while it
// rains or storms (0 without a Weather); no Season is the manual floor. 0 when `enabled` or `strength` is 0. The getters read
// the authoring even when `enabled` is 0.
//
//   weather.pollen_enabled() / pollen_spring_start() / pollen_spring_full() / pollen_spring_end_full() /
//   weather.pollen_spring_end() / pollen_manual() / pollen_day_only() / pollen_min_daylight() / pollen_rate() /
//   weather.pollen_box_xz_m() / pollen_height_m() / pollen_size_m() / pollen_lifetime_s() / pollen_fall_speed() /
//   weather.pollen_wind_follow() / pollen_max_particles() / pollen_seed() / pollen_strength() -> number
//   weather.pollen_color() -> r, g, b
//   weather.pollen_drifting() -> number           0..1, the pollen amount now
//   weather.set_pollen{enabled=, spring_start=, spring_full=, spring_end_full=, spring_end=, manual=, day_only=,
//                      min_daylight=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=, fall_speed=, wind_follow=,
//                      color={r,g,b}, max_particles=, seed=, strength=}
//                                         a table, every key optional, each value clamped to the field's Range (see
//                                         mce/kit/pollen.h; colour lanes 0..1; enabled and day_only: any non-zero number
//                                         is 1); an unknown key or a non-number raises an error that names the call and
//                                         nothing is written
//
// Dust (M-DUSTWIND step 25c) lives in the same table: the authoring over the first kit::Dust (mce/kit/dust.h) and the
// read-only dust_blowing(). Named errors as above ("weather.set_dust: the scene has no Dust entity"); the setter never adds
// the component. The getters are named weather.dust_<key>; the colour returns r, g, b. dust_blowing() is kit::dust_amount
// with the inputs the extract uses: the effective wind of the first Weather with the gust sway of the first WindGust (0
// with no Weather) and the first Weather's rain intensity while it rains or storms. 0 when `enabled` or `strength` is 0.
// The getters read the authoring even when `enabled` is 0.
//
//   weather.dust_enabled() / dust_wind_start() / dust_wind_full() / dust_manual() / dust_rate() / dust_box_xz_m() /
//   weather.dust_height_m() / dust_size_m() / dust_lifetime_s() / dust_speed() / dust_wind_follow() /
//   weather.dust_max_particles() / dust_seed() / dust_strength() -> number
//   weather.dust_color() -> r, g, b
//   weather.dust_blowing() -> number              0..1, the dust amount now
//   weather.set_dust{enabled=, wind_start=, wind_full=, manual=, rate=, box_xz_m=, height_m=, size_m=, lifetime_s=,
//                    speed=, wind_follow=, color={r,g,b}, max_particles=, seed=, strength=}
//                                         a table, every key optional, each value clamped to the field's Range (see
//                                         mce/kit/dust.h; colour lanes 0..1; enabled: any non-zero number is 1); an
//                                         unknown key or a non-number raises an error that names the call and nothing
//                                         is written
//
// Water (M-WATER step 15d) is the `water` table over the first kit::Water (mce/kit/water.h). Named errors as above
// ("water.set: the scene has no Water entity"); the setter never adds the component. height_at / normal_at / submerged
// evaluate the kit helpers (the drawn surface) at the scene time tick * dt (seconds, wrapped to the 64 s period in f64)
// with the main wave direction the extract uses: the WindDirection heading when wind_follow > 0 and the scene has one,
// else wind_dir. They read the authoring even when `enabled` is 0. Authoring names follow the set keys.
//
//   water.level() / half_extent() / wave_count() / amplitude() / wavelength() / steepness() / wind_dir() /
//   water.wind_follow() / speed_scale() / depth_scale() / reflectivity() / roughness() / foam_amount() /
//   water.foam_width() / enabled() -> number    level_y, half_extent, wave_count, amplitude, wavelength, steepness,
//                                               wind_dir_deg, wind_follow, speed_scale, depth_scale_m, reflectivity,
//                                               roughness, foam_amount, foam_width_m, enabled
//   water.shallow_color() / deep_color() -> r, g, b
//   water.height_at(x, z) -> number             the surface height at that world position now
//   water.normal_at(x, z) -> x, y, z            the unit surface normal there
//   water.submerged(x, y, z) -> boolean         y < height_at(x, z)
//   water.set{level=, half_extent=, wave_count=, amplitude=, wavelength=, steepness=, wind_dir=, wind_follow=,
//             speed_scale=, shallow_color={r,g,b}, deep_color={r,g,b}, depth_scale=, reflectivity=, roughness=,
//             foam_amount=, foam_width=, enabled=}
//                                         a table, every key optional, each value clamped to the component's Range
//                                         (wave_count rounded down; colour lanes 0..1; enabled: any non-zero number
//                                         is 1); an unknown key, a non-number or a colour that is not three numbers
//                                         raises an error that names the call and nothing is written
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/kit/aurora.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/rain_ripples.h"
#include "mce/kit/rain_splash.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/thunder_delay.h"
#include "mce/kit/wind_direction.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_sway.h"
#include "mce/kit/wet_surfaces.h"
#include "mce/kit/weather_audio.h"
#include "mce/kit/lightning_bolt.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/ground_mist.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/moon.h"
#include "mce/kit/meteor.h"
#include "mce/kit/firefly.h"
#include "mce/kit/pollen.h"
#include "mce/kit/dust.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/season.h"
#include "mce/kit/rainbow.h"
#include "mce/kit/water.h"
#include "mce/kit/water_ice.h"
#include "mce/kit/world_clock.h"
#include "mce/render/atmosphere.h"

namespace mce::script::detail {

namespace {

constexpr const char* kKindNames[] = {"clear", "overcast", "rain", "storm", "snow"};
constexpr int kKindCount = 5;

const char* kind_name(kit::WeatherKind k) {
    const usize i = static_cast<usize>(k);
    return i < static_cast<usize>(kKindCount) ? kKindNames[i] : "clear";
}

// The first Weather, or a Luau error that names the call.
kit::Weather& check_weather(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("Weather"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::Weather* found = nullptr;
    w.query<kit::Weather>().each([&](Entity, kit::Weather& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no Weather entity", fn);
    return *found;
}

// The first WetSurfaces, or a Luau error that names the call.
kit::WetSurfaces& check_wet(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("WetSurfaces"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::WetSurfaces* found = nullptr;
    w.query<kit::WetSurfaces>().each([&](Entity, kit::WetSurfaces& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no WetSurfaces entity", fn);
    return *found;
}

// The first WindSway, or a Luau error that names the call.
kit::WindSway& check_sway(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("WindSway"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::WindSway* found = nullptr;
    w.query<kit::WindSway>().each([&](Entity, kit::WindSway& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no WindSway entity", fn);
    return *found;
}

// The first SnowCover, or a Luau error that names the call.
kit::SnowCover& check_snow(lua_State* L, const char* fn) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId("SnowCover"));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    kit::SnowCover* found = nullptr;
    w.query<kit::SnowCover>().each([&](Entity, kit::SnowCover& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no SnowCover entity", fn);
    return *found;
}

// The first component of type T (registered under `name`), or a Luau error that names the call.
template <typename T>
T& check_first(lua_State* L, const char* fn, const char* name) {
    World& w = world_of(L);
    const refl::TypeRecord* type = refl::find_type(StringId(name));
    if (!type || w.find_component(type->type_hash) == ecs::kInvalidComponent)
        luaL_error(L, "%s: this world has no weather (install_weather)", fn);
    T* found = nullptr;
    w.query<T>().each([&](Entity, T& c) {
        if (!found) found = &c;
    });
    if (!found) luaL_error(L, "%s: the scene has no %s entity", fn, name);
    return *found;
}

kit::RainRipples& check_ripples(lua_State* L, const char* fn) { return check_first<kit::RainRipples>(L, fn, "RainRipples"); }
kit::RainSplash& check_splash(lua_State* L, const char* fn) { return check_first<kit::RainSplash>(L, fn, "RainSplash"); }
kit::ThunderDelay& check_thunder(lua_State* L, const char* fn) { return check_first<kit::ThunderDelay>(L, fn, "ThunderDelay"); }
kit::WindDirection& check_wind_direction(lua_State* L, const char* fn) {
    return check_first<kit::WindDirection>(L, fn, "WindDirection");
}

kit::CloudLayer& check_clouds(lua_State* L, const char* fn) { return check_first<kit::CloudLayer>(L, fn, "CloudLayer"); }

kit::WeatherAudio& check_audio(lua_State* L, const char* fn) { return check_first<kit::WeatherAudio>(L, fn, "WeatherAudio"); }

kit::LightningBolt& check_bolt(lua_State* L, const char* fn) { return check_first<kit::LightningBolt>(L, fn, "LightningBolt"); }

kit::WindGust& check_gust(lua_State* L, const char* fn) { return check_first<kit::WindGust>(L, fn, "WindGust"); }

kit::GroundMist& check_mist(lua_State* L, const char* fn) { return check_first<kit::GroundMist>(L, fn, "GroundMist"); }

kit::Moon& check_moon(lua_State* L, const char* fn) { return check_first<kit::Moon>(L, fn, "Moon"); }

int l_weather_kind(lua_State* L) {
    lua_pushstring(L, kind_name(check_weather(L, "weather.kind").current));
    return 1;
}

int l_weather_target(lua_State* L) {
    lua_pushstring(L, kind_name(check_weather(L, "weather.target").target));
    return 1;
}

int l_weather_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int kind = -1;
    for (int i = 0; i < kKindCount; ++i) {
        if (std::strcmp(name, kKindNames[i]) == 0) kind = i;
    }
    if (kind < 0) luaL_error(L, "weather.set: unknown weather '%s' (clear, overcast, rain, storm, snow)", name);
    bool has_seconds = false;
    f64 seconds = 0.0;
    if (!lua_isnoneornil(L, 2)) {
        seconds = luaL_checknumber(L, 2);
        if (std::isnan(seconds)) luaL_error(L, "weather.set: the transition seconds are not a number");
        seconds = seconds < 0.0 ? 0.0 : (seconds > 3600.0 ? 3600.0 : seconds);
        has_seconds = true;
    }
    kit::Weather& wx = check_weather(L, "weather.set");
    wx.target = static_cast<kit::WeatherKind>(kind);
    if (has_seconds) wx.transition_seconds = static_cast<f32>(seconds);
    return 0;
}

int l_weather_intensity(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_weather(L, "weather.intensity").intensity));
    return 1;
}

int l_weather_wind(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_weather(L, "weather.wind").wind));
    return 1;
}

int l_weather_wetness(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_weather(L, "weather.wetness").wetness));
    return 1;
}

int l_weather_wet_strength(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_strength").strength));
    return 1;
}

int l_weather_wet_darken(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_darken").darken));
    return 1;
}

int l_weather_wet_puddles(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_puddles").puddles));
    return 1;
}

int l_weather_wet_puddle_scale(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wet(L, "weather.wet_puddle_scale").puddle_scale_m));
    return 1;
}

f64 clamp_range(f64 v, f64 lo, f64 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// weather.set_wet{strength=, darken=, puddles=, puddle_scale=}: every key optional, validated before anything is
// written (an error leaves the component as it was), values clamped to the component's Range.
int l_weather_set_wet(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Slots: 0 strength, 1 darken, 2 puddles, 3 puddle_scale.
    static const char* const kKeys[4] = {"strength", "darken", "puddles", "puddle_scale"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "weather.set_wet: the keys are names (strength, darken, puddles, puddle_scale)");
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) slot = i;
        }
        if (slot < 0)
            luaL_error(L, "weather.set_wet: unknown key '%s' (strength, darken, puddles, puddle_scale)", key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "weather.set_wet: '%s' is not a number", key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "weather.set_wet: '%s' is not a number", key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
    kit::WetSurfaces& ws = check_wet(L, "weather.set_wet");
    if (given[0]) ws.strength = static_cast<f32>(clamp_range(value[0], 0.0, 4.0));
    if (given[1]) ws.darken = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) ws.puddles = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) ws.puddle_scale_m = static_cast<f32>(clamp_range(value[3], 0.1, 100.0));
    return 0;
}

int l_weather_sway_amplitude(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_amplitude").amplitude_gain));
    return 1;
}

int l_weather_sway_frequency(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_frequency").frequency_gain));
    return 1;
}

int l_weather_sway_gust(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_gust").gust_gain));
    return 1;
}

int l_weather_sway_flutter(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_sway(L, "weather.sway_flutter").flutter_gain));
    return 1;
}

// weather.set_sway{amplitude=, frequency=, gust=, flutter=}: like set_wet, validated before anything is written.
int l_weather_set_sway(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Slots: 0 amplitude, 1 frequency, 2 gust, 3 flutter.
    static const char* const kKeys[4] = {"amplitude", "frequency", "gust", "flutter"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "weather.set_sway: the keys are names (amplitude, frequency, gust, flutter)");
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < 4; ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) slot = i;
        }
        if (slot < 0)
            luaL_error(L, "weather.set_sway: unknown key '%s' (amplitude, frequency, gust, flutter)", key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "weather.set_sway: '%s' is not a number", key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "weather.set_sway: '%s' is not a number", key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
    kit::WindSway& ws = check_sway(L, "weather.set_sway");
    if (given[0]) ws.amplitude_gain = static_cast<f32>(clamp_range(value[0], 0.0, 50.0));
    if (given[1]) ws.frequency_gain = static_cast<f32>(clamp_range(value[1], 0.0, 10.0));
    if (given[2]) ws.gust_gain = static_cast<f32>(clamp_range(value[2], -1.0, 1.0));
    if (given[3]) ws.flutter_gain = static_cast<f32>(clamp_range(value[3], 0.0, 50.0));
    return 0;
}

int l_weather_snow_cover(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_cover").cover));
    return 1;
}

int l_weather_snow_strength(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_strength").strength));
    return 1;
}

int l_weather_snow_build(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_build").build_seconds));
    return 1;
}

int l_weather_snow_melt(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_melt").melt_seconds));
    return 1;
}

int l_weather_snow_coverage(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_coverage").coverage));
    return 1;
}

int l_weather_snow_scale(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_scale").noise_scale_m));
    return 1;
}

int l_weather_snow_wind(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_snow(L, "weather.snow_wind").wind_bias));
    return 1;
}

// weather.set_snow{strength=, build=, melt=, coverage=, scale=, wind=}: like set_wet, validated before anything is
// written. (`cover` and `snow_q` are engine-written and not settable.)
int l_weather_set_snow(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Slots: 0 strength, 1 build, 2 melt, 3 coverage, 4 scale, 5 wind.
    static const char* const kKeys[6] = {"strength", "build", "melt", "coverage", "scale", "wind"};
    f64 value[6] = {0, 0, 0, 0, 0, 0};
    bool given[6] = {false, false, false, false, false, false};
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "weather.set_snow: the keys are names (strength, build, melt, coverage, scale, wind)");
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < 6; ++i) {
            if (std::strcmp(key, kKeys[i]) == 0) slot = i;
        }
        if (slot < 0)
            luaL_error(L, "weather.set_snow: unknown key '%s' (strength, build, melt, coverage, scale, wind)", key);
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "weather.set_snow: '%s' is not a number", key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "weather.set_snow: '%s' is not a number", key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
    kit::SnowCover& sc = check_snow(L, "weather.set_snow");
    if (given[0]) sc.strength = static_cast<f32>(clamp_range(value[0], 0.0, 4.0));
    if (given[1]) sc.build_seconds = static_cast<f32>(clamp_range(value[1], 0.0, 86400.0));
    if (given[2]) sc.melt_seconds = static_cast<f32>(clamp_range(value[2], 0.0, 86400.0));
    if (given[3]) sc.coverage = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) sc.noise_scale_m = static_cast<f32>(clamp_range(value[4], 0.1, 100.0));
    if (given[5]) sc.wind_bias = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    return 0;
}

// Reads the settings table at index 1 into value[]/given[] (slot i is keys[i]); raises on a non-name key, an unknown
// key or a non-number (NaN included) before anything is written. `list` is the key list for the messages.
void read_settings(lua_State* L, const char* fn, const char* const* keys, int n, const char* list, f64* value,
                   bool* given, int vec3_slot = -1, f64* vec3 = nullptr, int vec3b_slot = -1, f64* vec3b = nullptr, int vec3c_slot = -1,
                   f64* vec3c = nullptr) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "%s: the keys are names (%s)", fn, list);
        const char* key = lua_tostring(L, -2);
        int slot = -1;
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(key, keys[i]) == 0) slot = i;
        }
        if (slot < 0) luaL_error(L, "%s: unknown key '%s' (%s)", fn, key, list);
        f64* const triple = slot == vec3_slot && vec3    ? vec3
                            : slot == vec3b_slot && vec3b ? vec3b
                            : slot == vec3c_slot && vec3c ? vec3c
                                                          : nullptr;
        if (triple) {  // a {r, g, b} table of three numbers
            if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "%s: '%s' is not a table of three numbers", fn, key);
            for (int i = 0; i < 3; ++i) {
                lua_rawgeti(L, -1, i + 1);
                const f64 c = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : std::nan("");
                lua_pop(L, 1);
                if (std::isnan(c)) luaL_error(L, "%s: '%s' is not a table of three numbers", fn, key);
                triple[i] = c;
            }
            given[slot] = true;
            lua_pop(L, 1);
            continue;
        }
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "%s: '%s' is not a number", fn, key);
        const f64 v = lua_tonumber(L, -1);
        if (std::isnan(v)) luaL_error(L, "%s: '%s' is not a number", fn, key);
        value[slot] = v;
        given[slot] = true;
        lua_pop(L, 1);
    }
}

#define MCE_WEATHER_GETTER(luaname, check, field)                                                  \
    int l_weather_##luaname(lua_State* L) {                                                       \
        lua_pushnumber(L, static_cast<double>(check(L, "weather." #luaname).field));              \
        return 1;                                                                                  \
    }
MCE_WEATHER_GETTER(ripples_strength, check_ripples, strength)
MCE_WEATHER_GETTER(ripples_density, check_ripples, density)
MCE_WEATHER_GETTER(ripples_speed, check_ripples, speed)
MCE_WEATHER_GETTER(ripples_ring_width, check_ripples, ring_width)
MCE_WEATHER_GETTER(splash_rate, check_splash, rate)
MCE_WEATHER_GETTER(splash_height, check_splash, height_m)
MCE_WEATHER_GETTER(splash_life, check_splash, life_seconds)
MCE_WEATHER_GETTER(splash_radius, check_splash, radius_m)
MCE_WEATHER_GETTER(splash_seed, check_splash, seed)
MCE_WEATHER_GETTER(thunder_speed, check_thunder, speed_mps)
MCE_WEATHER_GETTER(thunder_volume, check_thunder, volume)
#undef MCE_WEATHER_GETTER

// weather.set_ripples{strength=, density=, speed=, ring_width=}: validated before anything is written.
int l_weather_set_ripples(lua_State* L) {
    static const char* const kKeys[4] = {"strength", "density", "speed", "ring_width"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    read_settings(L, "weather.set_ripples", kKeys, 4, "strength, density, speed, ring_width", value, given);
    kit::RainRipples& r = check_ripples(L, "weather.set_ripples");
    if (given[0]) r.strength = static_cast<f32>(clamp_range(value[0], 0.0, 1.0));
    if (given[1]) r.density = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) r.speed = static_cast<f32>(clamp_range(value[2], 0.1, 10.0));
    if (given[3]) r.ring_width = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    return 0;
}

// weather.set_splash{rate=, height=, life=, radius=, seed=}. (`counter` is engine-written and not settable.)
int l_weather_set_splash(lua_State* L) {
    static const char* const kKeys[5] = {"rate", "height", "life", "radius", "seed"};
    f64 value[5] = {0, 0, 0, 0, 0};
    bool given[5] = {false, false, false, false, false};
    read_settings(L, "weather.set_splash", kKeys, 5, "rate, height, life, radius, seed", value, given);
    kit::RainSplash& s = check_splash(L, "weather.set_splash");
    if (given[0]) s.rate = static_cast<f32>(clamp_range(value[0], 0.0, 64.0));
    if (given[1]) s.height_m = static_cast<f32>(clamp_range(value[1], 0.01, 5.0));
    if (given[2]) s.life_seconds = static_cast<f32>(clamp_range(value[2], 0.02, 5.0));
    if (given[3]) s.radius_m = static_cast<f32>(clamp_range(value[3], 0.5, 500.0));
    if (given[4]) s.seed = static_cast<u32>(std::floor(clamp_range(value[4], 0.0, 4294967295.0)));
    return 0;
}

// weather.set_thunder{speed=, volume=}. (The clip, pending, due and gain_db are not settable.)
int l_weather_set_thunder(lua_State* L) {
    static const char* const kKeys[2] = {"speed", "volume"};
    f64 value[2] = {0, 0};
    bool given[2] = {false, false};
    read_settings(L, "weather.set_thunder", kKeys, 2, "speed, volume", value, given);
    kit::ThunderDelay& t = check_thunder(L, "weather.set_thunder");
    if (given[0]) t.speed_mps = static_cast<f32>(clamp_range(value[0], 1.0, 1000000.0));
    if (given[1]) t.volume = static_cast<f32>(clamp_range(value[1], 0.0, 4.0));
    return 0;
}

#define MCE_WEATHER_GETTER_WD(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_wind_direction(L, "weather." #luaname).field)); \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_WD(wind_degrees, degrees)
MCE_WEATHER_GETTER_WD(wind_drift, drift_degrees_per_minute)
#undef MCE_WEATHER_GETTER_WD

// weather.wind_heading(): the resolved heading in degrees from the quantized angle_q (1/65536 degree), 0..360.
int l_weather_wind_heading(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_wind_direction(L, "weather.wind_heading").angle_q) / 65536.0);
    return 1;
}

// weather.set_wind_direction{degrees=, drift=}: validated before anything is written. (angle_q, frac and seeded are
// engine-written and not settable.)
int l_weather_set_wind_direction(lua_State* L) {
    static const char* const kKeys[2] = {"degrees", "drift"};
    f64 value[2] = {0, 0};
    bool given[2] = {false, false};
    read_settings(L, "weather.set_wind_direction", kKeys, 2, "degrees, drift", value, given);
    kit::WindDirection& d = check_wind_direction(L, "weather.set_wind_direction");
    if (given[0]) d.degrees = static_cast<f32>(clamp_range(value[0], 0.0, 360.0));
    if (given[1]) d.drift_degrees_per_minute = static_cast<f32>(clamp_range(value[1], -3600.0, 3600.0));
    return 0;
}

#define MCE_WEATHER_GETTER_CL(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_clouds(L, "weather." #luaname).field));         \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_CL(cloud_strength, strength)
MCE_WEATHER_GETTER_CL(cloud_fair_coverage, fair_coverage)
MCE_WEATHER_GETTER_CL(cloud_overcast_coverage, overcast_coverage)
MCE_WEATHER_GETTER_CL(cloud_scale, scale_m)
MCE_WEATHER_GETTER_CL(cloud_speed_scale, speed_scale)
MCE_WEATHER_GETTER_CL(cloud_height, height_m)
#undef MCE_WEATHER_GETTER_CL

// weather.set_clouds{strength=, fair_coverage=, overcast_coverage=, scale=, speed_scale=, height=}: validated before
// anything is written.
int l_weather_set_clouds(lua_State* L) {
    static const char* const kKeys[6] = {"strength", "fair_coverage", "overcast_coverage", "scale", "speed_scale", "height"};
    f64 value[6] = {0, 0, 0, 0, 0, 0};
    bool given[6] = {false, false, false, false, false, false};
    read_settings(L, "weather.set_clouds", kKeys, 6, "strength, fair_coverage, overcast_coverage, scale, speed_scale, height",
                  value, given);
    kit::CloudLayer& c = check_clouds(L, "weather.set_clouds");
    if (given[0]) c.strength = static_cast<f32>(clamp_range(value[0], 0.0, 1.0));
    if (given[1]) c.fair_coverage = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) c.overcast_coverage = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) c.scale_m = static_cast<f32>(clamp_range(value[3], 50.0, 20000.0));
    if (given[4]) c.speed_scale = static_cast<f32>(clamp_range(value[4], 0.0, 10.0));
    if (given[5]) c.height_m = static_cast<f32>(clamp_range(value[5], 100.0, 20000.0));
    return 0;
}

#define MCE_WEATHER_GETTER_WA(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_audio(L, "weather." #luaname).field));         \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_WA(audio_rain_db, rain_db)
MCE_WEATHER_GETTER_WA(audio_wind_db, wind_db)
MCE_WEATHER_GETTER_WA(audio_fade, fade_seconds)
MCE_WEATHER_GETTER_WA(audio_enabled, enabled)
#undef MCE_WEATHER_GETTER_WA

// The levels are saved as integers out of 2^24 (the Weather.wet_q convention).
int l_weather_rain_level(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_audio(L, "weather.rain_level").rain_q) / 16777216.0);
    return 1;
}

int l_weather_wind_level(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(check_audio(L, "weather.wind_level").wind_q) / 16777216.0);
    return 1;
}

// weather.set_audio{rain_db=, wind_db=, fade=, enabled=}: validated before anything is written. (The sources and the
// levels are not settable.)
int l_weather_set_audio(lua_State* L) {
    static const char* const kKeys[4] = {"rain_db", "wind_db", "fade", "enabled"};
    f64 value[4] = {0, 0, 0, 0};
    bool given[4] = {false, false, false, false};
    read_settings(L, "weather.set_audio", kKeys, 4, "rain_db, wind_db, fade, enabled", value, given);
    kit::WeatherAudio& a = check_audio(L, "weather.set_audio");
    if (given[0]) a.rain_db = static_cast<f32>(clamp_range(value[0], -60.0, 12.0));
    if (given[1]) a.wind_db = static_cast<f32>(clamp_range(value[1], -60.0, 12.0));
    if (given[2]) a.fade_seconds = static_cast<f32>(clamp_range(value[2], 0.0, 60.0));
    if (given[3]) a.enabled = value[3] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_LB(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_bolt(L, "weather." #luaname).field));           \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_LB(bolt_min_range, min_range_m)
MCE_WEATHER_GETTER_LB(bolt_max_range, max_range_m)
MCE_WEATHER_GETTER_LB(bolt_ground_y, ground_y)
MCE_WEATHER_GETTER_LB(bolt_height, height_m)
MCE_WEATHER_GETTER_LB(bolt_width, width_m)
MCE_WEATHER_GETTER_LB(bolt_segments, segments_log2)
MCE_WEATHER_GETTER_LB(bolt_jitter, jitter)
MCE_WEATHER_GETTER_LB(bolt_fork, fork)
MCE_WEATHER_GETTER_LB(bolt_enabled, enabled)
MCE_WEATHER_GETTER_LB(bolt_light, light_intensity)
MCE_WEATHER_GETTER_LB(bolt_light_radius, light_radius_m)
MCE_WEATHER_GETTER_LB(bolt_distance, strike_dist_m)
MCE_WEATHER_GETTER_LB(bolt_active, active)
#undef MCE_WEATHER_GETTER_LB

int l_weather_bolt_pos(lua_State* L) {
    const kit::LightningBolt& b = check_bolt(L, "weather.bolt_pos");
    lua_pushnumber(L, static_cast<double>(b.strike_x));
    lua_pushnumber(L, static_cast<double>(b.strike_z));
    return 2;
}

// weather.set_bolt{...}: validated before anything is written. (The last strike is engine-written, not settable.)
int l_weather_set_bolt(lua_State* L) {
    static const char* const kKeys[11] = {"min_range", "max_range", "ground_y", "height", "width", "segments",
                                          "jitter", "fork", "light", "light_radius", "enabled"};
    f64 value[11] = {};
    bool given[11] = {};
    read_settings(L, "weather.set_bolt", kKeys, 11,
                  "min_range, max_range, ground_y, height, width, segments, jitter, fork, light, light_radius, enabled",
                  value, given);
    kit::LightningBolt& b = check_bolt(L, "weather.set_bolt");
    if (given[0]) b.min_range_m = static_cast<f32>(clamp_range(value[0], 10.0, 5000.0));
    if (given[1]) b.max_range_m = static_cast<f32>(clamp_range(value[1], 10.0, 5000.0));
    if (given[2]) b.ground_y = static_cast<f32>(clamp_range(value[2], -1.0e6, 1.0e6));
    if (given[3]) b.height_m = static_cast<f32>(clamp_range(value[3], 50.0, 3000.0));
    if (given[4]) b.width_m = static_cast<f32>(clamp_range(value[4], 0.05, 20.0));
    if (given[5]) b.segments_log2 = static_cast<u32>(std::floor(clamp_range(value[5], 2.0, 7.0)));
    if (given[6]) b.jitter = static_cast<f32>(clamp_range(value[6], 0.0, 0.6));
    if (given[7]) b.fork = value[7] != 0.0 ? 1 : 0;
    if (given[8]) b.light_intensity = static_cast<f32>(clamp_range(value[8], 0.0, 100000.0));
    if (given[9]) b.light_radius_m = static_cast<f32>(clamp_range(value[9], 1.0, 5000.0));
    if (given[10]) b.enabled = value[10] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_WG(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_gust(L, "weather." #luaname).field));            \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_WG(gust_strength, strength)
MCE_WEATHER_GETTER_WG(gust_min_gap, min_gap_s)
MCE_WEATHER_GETTER_WG(gust_max_gap, max_gap_s)
MCE_WEATHER_GETTER_WG(gust_min_rise, min_rise_s)
MCE_WEATHER_GETTER_WG(gust_max_rise, max_rise_s)
MCE_WEATHER_GETTER_WG(gust_min_fall, min_fall_s)
MCE_WEATHER_GETTER_WG(gust_max_fall, max_fall_s)
MCE_WEATHER_GETTER_WG(gust_min_peak, min_peak)
MCE_WEATHER_GETTER_WG(gust_max_peak, max_peak)
MCE_WEATHER_GETTER_WG(gust_seed, seed)
MCE_WEATHER_GETTER_WG(gust_affects, affects)
MCE_WEATHER_GETTER_WG(gust_enabled, enabled)
#undef MCE_WEATHER_GETTER_WG

int l_weather_gust_level(lua_State* L) {
    const kit::WindGust& g = check_gust(L, "weather.gust_level");
    lua_pushnumber(L, static_cast<double>(g.gust_q) / 16777216.0);
    return 1;
}

int l_weather_effective_wind(lua_State* L) {
    const kit::Weather& wx = check_weather(L, "weather.effective_wind");
    const kit::WindGust& g = check_gust(L, "weather.effective_wind");
    lua_pushnumber(L, static_cast<double>(kit::effective_wind(wx, &g, kit::kGustAll)));
    return 1;
}

// weather.set_gust{...}: validated before anything is written. (The gust state is engine-written, not settable.)
int l_weather_set_gust(lua_State* L) {
    static const char* const kKeys[12] = {"strength", "min_gap", "max_gap", "min_rise", "max_rise", "min_fall",
                                          "max_fall", "min_peak", "max_peak", "seed", "affects", "enabled"};
    f64 value[12] = {};
    bool given[12] = {};
    read_settings(L, "weather.set_gust", kKeys, 12,
                  "strength, min_gap, max_gap, min_rise, max_rise, min_fall, max_fall, min_peak, max_peak, seed, "
                  "affects, enabled",
                  value, given);
    kit::WindGust& g = check_gust(L, "weather.set_gust");
    if (given[0]) g.strength = static_cast<f32>(clamp_range(value[0], 0.0, 3.0));
    if (given[1]) g.min_gap_s = static_cast<f32>(clamp_range(value[1], 0.0, 120.0));
    if (given[2]) g.max_gap_s = static_cast<f32>(clamp_range(value[2], 0.0, 120.0));
    if (given[3]) g.min_rise_s = static_cast<f32>(clamp_range(value[3], 0.05, 30.0));
    if (given[4]) g.max_rise_s = static_cast<f32>(clamp_range(value[4], 0.05, 30.0));
    if (given[5]) g.min_fall_s = static_cast<f32>(clamp_range(value[5], 0.05, 60.0));
    if (given[6]) g.max_fall_s = static_cast<f32>(clamp_range(value[6], 0.05, 60.0));
    if (given[7]) g.min_peak = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) g.max_peak = static_cast<f32>(clamp_range(value[8], 0.0, 1.0));
    if (given[9]) g.seed = static_cast<u32>(std::floor(clamp_range(value[9], 0.0, 4294967295.0)));
    if (given[10]) g.affects = static_cast<u32>(std::floor(clamp_range(value[10], 0.0, 4294967295.0)));
    if (given[11]) g.enabled = value[11] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_GM(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_mist(L, "weather." #luaname).field));            \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_GM(mist_strength, strength)
MCE_WEATHER_GETTER_GM(mist_peak_hour, peak_hour)
MCE_WEATHER_GETTER_GM(mist_rise, rise_hours)
MCE_WEATHER_GETTER_GM(mist_burn, burn_hours)
MCE_WEATHER_GETTER_GM(mist_base_humidity, base_humidity)
MCE_WEATHER_GETTER_GM(mist_wet_gain, wet_gain)
MCE_WEATHER_GETTER_GM(mist_weather_damp, weather_damp)
MCE_WEATHER_GETTER_GM(mist_density, density)
MCE_WEATHER_GETTER_GM(mist_thickness, thickness_m)
MCE_WEATHER_GETTER_GM(mist_base_height, base_height_m)
MCE_WEATHER_GETTER_GM(mist_exp2_density, exp2_density)
MCE_WEATHER_GETTER_GM(mist_enabled, enabled)
#undef MCE_WEATHER_GETTER_GM

int l_weather_mist_color(lua_State* L) {
    const kit::GroundMist& m = check_mist(L, "weather.mist_color");
    lua_pushnumber(L, static_cast<double>(m.color.x));
    lua_pushnumber(L, static_cast<double>(m.color.y));
    lua_pushnumber(L, static_cast<double>(m.color.z));
    return 3;
}

// The hour the extract would use now: the Environment3D resource (else the first component, else the default, which
// has no clock) gives it.
f32 current_hour(World& w) {
    f32 hour = kit::Environment3D{}.time_of_day;
    if (const kit::Environment3D* r = w.try_resource<kit::Environment3D>()) {
        hour = r->time_of_day;
    } else {
        bool found = false;
        w.query<const kit::Environment3D&>().each([&](const kit::Environment3D& c) {
            if (found) return;
            found = true;
            hour = c.time_of_day;
        });
    }
    return hour;
}

// The first Weather, if the world has any (the damping source of mist_level and moon_level); nullptr otherwise.
const kit::Weather* first_weather(World& w) {
    const kit::Weather* wx = nullptr;
    const refl::TypeRecord* type = refl::find_type(StringId("Weather"));
    if (type && w.find_component(type->type_hash) != ecs::kInvalidComponent) {
        w.query<kit::Weather>().each([&](Entity, kit::Weather& c) {
            if (!wx) wx = &c;
        });
    }
    return wx;
}

// The level the extract would apply now: the current hour, and the first Weather's wetness and damping.
int l_weather_mist_level(lua_State* L) {
    const kit::GroundMist& m = check_mist(L, "weather.mist_level");
    World& w = world_of(L);
    lua_pushnumber(L, static_cast<double>(kit::mist_level(m, current_hour(w), first_weather(w))));
    return 1;
}

// weather.set_mist{...}: validated before anything is written.
int l_weather_set_mist(lua_State* L) {
    static const char* const kKeys[13] = {"strength", "peak_hour", "rise_hours", "burn_hours", "base_humidity",
                                          "wet_gain", "weather_damp", "density", "thickness", "base_height",
                                          "exp2_density", "color", "enabled"};
    f64 value[13] = {};
    bool given[13] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_mist", kKeys, 13,
                  "strength, peak_hour, rise_hours, burn_hours, base_humidity, wet_gain, weather_damp, density, "
                  "thickness, base_height, exp2_density, color, enabled",
                  value, given, 11, color);
    kit::GroundMist& m = check_mist(L, "weather.set_mist");
    if (given[0]) m.strength = static_cast<f32>(clamp_range(value[0], 0.0, 2.0));
    if (given[1]) m.peak_hour = static_cast<f32>(clamp_range(value[1], 0.0, 24.0));
    if (given[2]) m.rise_hours = static_cast<f32>(clamp_range(value[2], 0.1, 12.0));
    if (given[3]) m.burn_hours = static_cast<f32>(clamp_range(value[3], 0.1, 12.0));
    if (given[4]) m.base_humidity = static_cast<f32>(clamp_range(value[4], 0.0, 1.0));
    if (given[5]) m.wet_gain = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) m.weather_damp = static_cast<f32>(clamp_range(value[6], 0.0, 1.0));
    if (given[7]) m.density = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) m.thickness_m = static_cast<f32>(clamp_range(value[8], 0.5, 200.0));
    if (given[9]) m.base_height_m = static_cast<f32>(clamp_range(value[9], -1000.0, 1000.0));
    if (given[10]) m.exp2_density = static_cast<f32>(clamp_range(value[10], 0.0, 0.2));
    if (given[11]) {
        m.color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        m.color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        m.color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[12]) m.enabled = value[12] != 0.0 ? 1 : 0;
    return 0;
}

#define MCE_WEATHER_GETTER_MOON(luaname, field)                                                   \
    int l_weather_##luaname(lua_State* L) {                                                         \
        lua_pushnumber(L, static_cast<double>(check_moon(L, "weather." #luaname).field));            \
        return 1;                                                                                   \
    }
MCE_WEATHER_GETTER_MOON(moon_phase_offset, phase_offset)
MCE_WEATHER_GETTER_MOON(moon_cycle_days, cycle_days)
MCE_WEATHER_GETTER_MOON(moon_freeze_phase, freeze_phase)
MCE_WEATHER_GETTER_MOON(moon_inclination, inclination_deg)
MCE_WEATHER_GETTER_MOON(moon_light_intensity, light_intensity)
MCE_WEATHER_GETTER_MOON(moon_light_shadows, light_shadows)
MCE_WEATHER_GETTER_MOON(moon_disk_size, disk_size)
MCE_WEATHER_GETTER_MOON(moon_disk_intensity, disk_intensity)
MCE_WEATHER_GETTER_MOON(moon_star_density, star_density)
MCE_WEATHER_GETTER_MOON(moon_star_brightness, star_brightness)
MCE_WEATHER_GETTER_MOON(moon_star_twinkle, star_twinkle)
MCE_WEATHER_GETTER_MOON(moon_enabled, enabled)
#undef MCE_WEATHER_GETTER_MOON

int l_weather_moon_light_color(lua_State* L) {
    const kit::Moon& m = check_moon(L, "weather.moon_light_color");
    lua_pushnumber(L, static_cast<double>(m.light_color.x));
    lua_pushnumber(L, static_cast<double>(m.light_color.y));
    lua_pushnumber(L, static_cast<double>(m.light_color.z));
    return 3;
}

// WorldClock.day of the first WorldClock (0 without one), the day the extract hands to the moon helpers.
u32 current_day(World& w) {
    u32 day = 0;
    const refl::TypeRecord* type = refl::find_type(StringId("WorldClock"));
    if (type && w.find_component(type->type_hash) != ecs::kInvalidComponent) {
        bool found = false;
        w.query<kit::WorldClock>().each([&](Entity, kit::WorldClock& c) {
            if (found) return;
            found = true;
            day = c.day;
        });
    }
    return day;
}

int l_weather_moon_phase(lua_State* L) {
    const kit::Moon& m = check_moon(L, "weather.moon_phase");
    World& w = world_of(L);
    lua_pushnumber(L, static_cast<double>(kit::moon_phase(m, current_day(w), current_hour(w))));
    return 1;
}

int l_weather_moon_illumination(lua_State* L) {
    const kit::Moon& m = check_moon(L, "weather.moon_illumination");
    World& w = world_of(L);
    lua_pushnumber(L, static_cast<double>(kit::moon_illumination(kit::moon_phase(m, current_day(w), current_hour(w)))));
    return 1;
}

int l_weather_moon_direction(lua_State* L) {
    const kit::Moon& m = check_moon(L, "weather.moon_direction");
    World& w = world_of(L);
    const f32 hour = current_hour(w);
    const math::vec3 d = kit::moon_direction(m, hour, kit::moon_phase(m, current_day(w), hour));
    lua_pushnumber(L, static_cast<double>(d.x));
    lua_pushnumber(L, static_cast<double>(d.y));
    lua_pushnumber(L, static_cast<double>(d.z));
    return 3;
}

int l_weather_moon_level(lua_State* L) {
    const kit::Moon& m = check_moon(L, "weather.moon_level");
    World& w = world_of(L);
    lua_pushnumber(L, static_cast<double>(kit::moon_level(m, current_day(w), current_hour(w), first_weather(w))));
    return 1;
}

// weather.set_moon{...}: validated before anything is written.
int l_weather_set_moon(lua_State* L) {
    static const char* const kKeys[13] = {"phase_offset", "cycle_days", "freeze_phase", "inclination",
                                          "light_intensity", "light_color", "light_shadows", "disk_size",
                                          "disk_intensity", "star_density", "star_brightness", "star_twinkle",
                                          "enabled"};
    f64 value[13] = {};
    bool given[13] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_moon", kKeys, 13,
                  "phase_offset, cycle_days, freeze_phase, inclination, light_intensity, light_color, light_shadows, "
                  "disk_size, disk_intensity, star_density, star_brightness, star_twinkle, enabled",
                  value, given, 5, color);
    kit::Moon& m = check_moon(L, "weather.set_moon");
    if (given[0]) m.phase_offset = static_cast<f32>(clamp_range(value[0], 0.0, 1.0));
    if (given[1]) m.cycle_days = static_cast<f32>(clamp_range(value[1], 2.0, 120.0));
    if (given[2]) m.freeze_phase = static_cast<f32>(clamp_range(value[2], -1.0, 1.0));
    if (given[3]) m.inclination_deg = static_cast<f32>(clamp_range(value[3], 0.0, 30.0));
    if (given[4]) m.light_intensity = static_cast<f32>(clamp_range(value[4], 0.0, 2.0));
    if (given[5]) {
        m.light_color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        m.light_color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        m.light_color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[6]) m.light_shadows = value[6] != 0.0 ? 1 : 0;
    if (given[7]) m.disk_size = static_cast<f32>(clamp_range(value[7], 0.99, 1.0));
    if (given[8]) m.disk_intensity = static_cast<f32>(clamp_range(value[8], 0.0, 50.0));
    if (given[9]) m.star_density = static_cast<f32>(clamp_range(value[9], 0.0, 1.0));
    if (given[10]) m.star_brightness = static_cast<f32>(clamp_range(value[10], 0.0, 4.0));
    if (given[11]) m.star_twinkle = static_cast<f32>(clamp_range(value[11], 0.0, 1.0));
    if (given[12]) m.enabled = value[12] != 0.0 ? 1 : 0;
    return 0;
}

// ---- Aurora (M-AURORA step 16d) ----

kit::Aurora& check_aurora(lua_State* L, const char* fn) { return check_first<kit::Aurora>(L, fn, "Aurora"); }

#define MCE_WEATHER_GETTER_AURORA(luaname, field)                                                        \
    int l_weather_##luaname(lua_State* L) {                                                               \
        lua_pushnumber(L, static_cast<double>(check_aurora(L, "weather." #luaname).field));                \
        return 1;                                                                                        \
    }
MCE_WEATHER_GETTER_AURORA(aurora_latitude, latitude_deg)
MCE_WEATHER_GETTER_AURORA(aurora_gate_low, gate_low_deg)
MCE_WEATHER_GETTER_AURORA(aurora_gate_high, gate_high_deg)
MCE_WEATHER_GETTER_AURORA(aurora_strength, strength)
MCE_WEATHER_GETTER_AURORA(aurora_curtain_count, curtain_count)
MCE_WEATHER_GETTER_AURORA(aurora_base_height, base_height)
MCE_WEATHER_GETTER_AURORA(aurora_height_span, height_span)
MCE_WEATHER_GETTER_AURORA(aurora_azimuth, azimuth_deg)
MCE_WEATHER_GETTER_AURORA(aurora_width, width_deg)
MCE_WEATHER_GETTER_AURORA(aurora_speed, speed)
MCE_WEATHER_GETTER_AURORA(aurora_night_only, night_only)
MCE_WEATHER_GETTER_AURORA(aurora_enabled, enabled)
#undef MCE_WEATHER_GETTER_AURORA

int push_aurora_color(lua_State* L, const math::vec3& c) {
    lua_pushnumber(L, static_cast<double>(c.x));
    lua_pushnumber(L, static_cast<double>(c.y));
    lua_pushnumber(L, static_cast<double>(c.z));
    return 3;
}
int l_weather_aurora_low_color(lua_State* L) { return push_aurora_color(L, check_aurora(L, "weather.aurora_low_color").low_color); }
int l_weather_aurora_high_color(lua_State* L) { return push_aurora_color(L, check_aurora(L, "weather.aurora_high_color").high_color); }

int l_weather_aurora_gate(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(kit::aurora_latitude_gate(check_aurora(L, "weather.aurora_gate"))));
    return 1;
}

// The night fade Kit3DExtract hands aurora_level: 0 with no clock, else 1 - smoothstep of the sun's own elevation over
// -0.1..0.1 (the sun direction of the hour, before any moonlight hand-off).
f32 aurora_night_fade(World& w) {
    const f32 hour = current_hour(w);
    if (!(hour >= 0.0f)) return 0.0f;
    const f32 elevation = -render::sun_direction_at(hour).y;
    const f32 t = std::clamp((elevation + 0.1f) / 0.2f, 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

// The cloud coverage the extract hands aurora_level (before the quality gate): the CloudLayer's fair-to-overcast blend by
// the first Weather's kind and intensity, times its strength; 0 without a Weather or a CloudLayer.
f32 aurora_cloud_cover(World& w) {
    const kit::Weather* wx = first_weather(w);
    const kit::CloudLayer* layer = nullptr;
    const refl::TypeRecord* type = refl::find_type(StringId("CloudLayer"));
    if (type && w.find_component(type->type_hash) != ecs::kInvalidComponent) {
        w.query<kit::CloudLayer>().each([&](Entity, kit::CloudLayer& c) {
            if (!layer) layer = &c;
        });
    }
    if (!wx || !layer) return 0.0f;
    f32 kind = 1.0f;  // Clear 0, Overcast / Rain / Storm 1, Snow 0.9
    if (wx->current == kit::WeatherKind::Clear) kind = 0.0f;
    else if (wx->current == kit::WeatherKind::Snow) kind = 0.9f;
    f32 inten = wx->intensity;
    inten = inten > 0.0f ? (inten < 1.0f ? inten : 1.0f) : 0.0f;
    const f32 blend = kind * inten;
    const f32 cover = (layer->fair_coverage + (layer->overcast_coverage - layer->fair_coverage) * blend) * layer->strength;
    return cover > 0.0f ? (cover < 1.0f ? cover : 1.0f) : 0.0f;
}

int l_weather_aurora_level(lua_State* L) {
    const kit::Aurora& a = check_aurora(L, "weather.aurora_level");
    World& w = world_of(L);
    lua_pushnumber(L, static_cast<double>(kit::aurora_level(a, aurora_night_fade(w), aurora_cloud_cover(w))));
    return 1;
}

// weather.set_aurora{...}: validated before anything is written.
int l_weather_set_aurora(lua_State* L) {
    static const char* const kKeys[14] = {"latitude", "gate_low", "gate_high", "strength", "curtain_count",
                                          "base_height", "height_span", "azimuth", "width", "speed", "low_color",
                                          "high_color", "night_only", "enabled"};
    f64 value[14] = {};
    bool given[14] = {};
    f64 low[3] = {};
    f64 high[3] = {};
    read_settings(L, "weather.set_aurora", kKeys, 14,
                  "latitude, gate_low, gate_high, strength, curtain_count, base_height, height_span, azimuth, width, "
                  "speed, low_color, high_color, night_only, enabled",
                  value, given, 10, low, 11, high);
    kit::Aurora& a = check_aurora(L, "weather.set_aurora");
    if (given[0]) a.latitude_deg = static_cast<f32>(clamp_range(value[0], -90.0, 90.0));
    if (given[1]) a.gate_low_deg = static_cast<f32>(clamp_range(value[1], 0.0, 90.0));
    if (given[2]) a.gate_high_deg = static_cast<f32>(clamp_range(value[2], 0.0, 90.0));
    if (given[3]) a.strength = static_cast<f32>(clamp_range(value[3], 0.0, 4.0));
    if (given[4]) a.curtain_count = static_cast<u8>(std::floor(clamp_range(value[4], 1.0, 6.0)));
    if (given[5]) a.base_height = static_cast<f32>(clamp_range(value[5], 0.02, 0.6));
    if (given[6]) a.height_span = static_cast<f32>(clamp_range(value[6], 0.05, 0.8));
    if (given[7]) a.azimuth_deg = static_cast<f32>(clamp_range(value[7], 0.0, 360.0));
    if (given[8]) a.width_deg = static_cast<f32>(clamp_range(value[8], 10.0, 180.0));
    if (given[9]) a.speed = static_cast<f32>(clamp_range(value[9], 0.0, 4.0));
    if (given[10]) {
        a.low_color.x = static_cast<f32>(clamp_range(low[0], 0.0, 1.0));
        a.low_color.y = static_cast<f32>(clamp_range(low[1], 0.0, 1.0));
        a.low_color.z = static_cast<f32>(clamp_range(low[2], 0.0, 1.0));
    }
    if (given[11]) {
        a.high_color.x = static_cast<f32>(clamp_range(high[0], 0.0, 1.0));
        a.high_color.y = static_cast<f32>(clamp_range(high[1], 0.0, 1.0));
        a.high_color.z = static_cast<f32>(clamp_range(high[2], 0.0, 1.0));
    }
    if (given[12]) a.night_only = value[12] != 0.0 ? 1 : 0;
    if (given[13]) a.enabled = value[13] != 0.0 ? 1 : 0;
    return 0;
}

// ---- Rainbow (M-RAINBOW step 17d) ----

kit::Rainbow& check_rainbow(lua_State* L, const char* fn) { return check_first<kit::Rainbow>(L, fn, "Rainbow"); }

#define MCE_WEATHER_GETTER_RAINBOW(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                               \
        lua_pushnumber(L, static_cast<double>(check_rainbow(L, "weather." #luaname).field));               \
        return 1;                                                                                        \
    }
MCE_WEATHER_GETTER_RAINBOW(rainbow_strength, strength)
MCE_WEATHER_GETTER_RAINBOW(rainbow_band_width, band_width)
MCE_WEATHER_GETTER_RAINBOW(rainbow_secondary, secondary)
MCE_WEATHER_GETTER_RAINBOW(rainbow_min_wetness, min_wetness)
MCE_WEATHER_GETTER_RAINBOW(rainbow_max_sun_elevation, max_sun_elevation_deg)
MCE_WEATHER_GETTER_RAINBOW(rainbow_enabled, enabled)
#undef MCE_WEATHER_GETTER_RAINBOW

// weather.rainbow_level(): kit::rainbow_level with the inputs Kit3DExtract's rainbow block uses. 0 without a Weather,
// under an equirect sky texture or with a non-finite input (the extract's default carrier). The sun elevation is the
// asin of the unit light direction's -y: sun_direction_at(hour) with a clock, else the Environment3D's sun_direction;
// wetness is the first Weather's, rain its intensity while it rains or storms, and the cloud cover is the one the
// aurora uses (the extract's own value before the quality gate).
int l_weather_rainbow_level(lua_State* L) {
    const kit::Rainbow& r = check_rainbow(L, "weather.rainbow_level");
    World& w = world_of(L);
    const kit::Weather* wx = first_weather(w);
    kit::Environment3D env;
    if (const kit::Environment3D* res = w.try_resource<kit::Environment3D>()) {
        env = *res;
    } else {
        bool found = false;
        w.query<const kit::Environment3D&>().each([&](const kit::Environment3D& c) {
            if (found) return;
            found = true;
            env = c;
        });
    }
    f32 level = 0.0f;
    if (r.enabled && wx && env.sky_texture.guid == asset::Guid{}) {
        const math::vec3 light = env.time_of_day >= 0.0f ? render::sun_direction_at(env.time_of_day) : env.sun_direction;
        const f32 len = std::sqrt(light.x * light.x + light.y * light.y + light.z * light.z);
        const bool raining = wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm;
        const f32 rain = raining ? wx->intensity : 0.0f;
        const f32 cover = aurora_cloud_cover(w);
        if (std::isfinite(len) && len > 1e-6f && std::isfinite(wx->wetness) && std::isfinite(rain) && std::isfinite(cover)) {
            const f32 sine = std::clamp(-light.y / len, -1.0f, 1.0f);
            const f32 elevation_deg = static_cast<f32>(std::asin(static_cast<f64>(sine)) * (180.0 / 3.14159265358979323846));
            level = kit::rainbow_level(r, elevation_deg, wx->wetness, rain, cover);
            if (!std::isfinite(level)) level = 0.0f;
        }
    }
    lua_pushnumber(L, static_cast<double>(level));
    return 1;
}

// weather.set_rainbow{...}: validated before anything is written.
int l_weather_set_rainbow(lua_State* L) {
    static const char* const kKeys[6] = {"strength", "band_width", "secondary", "min_wetness", "max_sun_elevation", "enabled"};
    f64 value[6] = {};
    bool given[6] = {};
    read_settings(L, "weather.set_rainbow", kKeys, 6, "strength, band_width, secondary, min_wetness, max_sun_elevation, enabled",
                  value, given);
    kit::Rainbow& r = check_rainbow(L, "weather.set_rainbow");
    if (given[0]) r.strength = static_cast<f32>(clamp_range(value[0], 0.0, 4.0));
    if (given[1]) r.band_width = static_cast<f32>(clamp_range(value[1], 0.5, 2.0));
    if (given[2]) r.secondary = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) r.min_wetness = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) r.max_sun_elevation_deg = static_cast<f32>(clamp_range(value[4], 10.0, 60.0));
    if (given[5]) r.enabled = value[5] != 0.0 ? 1 : 0;
    return 0;
}

// ---- Meteor (M-METEOR step 18d) ----

kit::Meteor& check_meteor(lua_State* L, const char* fn) { return check_first<kit::Meteor>(L, fn, "Meteor"); }

#define MCE_WEATHER_GETTER_METEOR(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_meteor(L, "weather." #luaname).field));               \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_METEOR(meteor_rate, rate)
MCE_WEATHER_GETTER_METEOR(meteor_brightness, brightness)
MCE_WEATHER_GETTER_METEOR(meteor_length, length)
MCE_WEATHER_GETTER_METEOR(meteor_speed, speed)
MCE_WEATHER_GETTER_METEOR(meteor_min_darkness, min_darkness)
MCE_WEATHER_GETTER_METEOR(meteor_night_only, night_only)
MCE_WEATHER_GETTER_METEOR(meteor_seed, seed)
MCE_WEATHER_GETTER_METEOR(meteor_enabled, enabled)
#undef MCE_WEATHER_GETTER_METEOR

// weather.meteor_level(): kit::meteor_level with the inputs Kit3DExtract's meteor block uses. 0 under an equirect sky
// texture. The night fade is the aurora's (0 with no clock, so only night_only 0 shows), the rain is the first Weather's
// intensity while it rains or storms (0 without a Weather) and the cloud cover is the one the aurora uses (the extract's
// own value before the quality gate).
int l_weather_meteor_level(lua_State* L) {
    const kit::Meteor& m = check_meteor(L, "weather.meteor_level");
    World& w = world_of(L);
    kit::Environment3D env;
    if (const kit::Environment3D* res = w.try_resource<kit::Environment3D>()) {
        env = *res;
    } else {
        bool found = false;
        w.query<const kit::Environment3D&>().each([&](const kit::Environment3D& c) {
            if (found) return;
            found = true;
            env = c;
        });
    }
    f32 level = 0.0f;
    if (m.enabled && env.sky_texture.guid == asset::Guid{}) {
        const kit::Weather* wx = first_weather(w);
        const bool raining = wx && (wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm);
        const f32 rain = raining ? wx->intensity : 0.0f;
        level = kit::meteor_level(m, aurora_night_fade(w), aurora_cloud_cover(w), rain);
        if (!std::isfinite(level)) level = 0.0f;
    }
    lua_pushnumber(L, static_cast<double>(level));
    return 1;
}

// weather.set_meteor{...}: validated before anything is written.
int l_weather_set_meteor(lua_State* L) {
    static const char* const kKeys[8] = {"rate", "brightness", "length", "speed", "min_darkness", "night_only", "seed", "enabled"};
    f64 value[8] = {};
    bool given[8] = {};
    read_settings(L, "weather.set_meteor", kKeys, 8, "rate, brightness, length, speed, min_darkness, night_only, seed, enabled",
                  value, given);
    kit::Meteor& m = check_meteor(L, "weather.set_meteor");
    if (given[0]) m.rate = static_cast<f32>(clamp_range(value[0], 0.0, 30.0));
    if (given[1]) m.brightness = static_cast<f32>(clamp_range(value[1], 0.0, 4.0));
    if (given[2]) m.length = static_cast<f32>(clamp_range(value[2], 0.5, 2.0));
    if (given[3]) m.speed = static_cast<f32>(clamp_range(value[3], 0.5, 2.0));
    if (given[4]) m.min_darkness = static_cast<f32>(clamp_range(value[4], 0.0, 1.0));
    if (given[5]) m.night_only = value[5] != 0.0 ? 1 : 0;
    if (given[6]) m.seed = static_cast<u32>(std::floor(clamp_range(value[6], 0.0, 65535.0)));
    if (given[7]) m.enabled = value[7] != 0.0 ? 1 : 0;
    return 0;
}

// ---- Season (M-SEASONS step 19c) ----

kit::Season& check_season(lua_State* L, const char* fn) { return check_first<kit::Season>(L, fn, "Season"); }

#define MCE_WEATHER_GETTER_SEASON(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_season(L, "weather." #luaname).field));               \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_SEASON(season_enabled, enabled)
MCE_WEATHER_GETTER_SEASON(season_year_days, year_days)
MCE_WEATHER_GETTER_SEASON(season_start_phase, start_phase)
MCE_WEATHER_GETTER_SEASON(season_winter_density, winter_density)
MCE_WEATHER_GETTER_SEASON(season_snow_cover_bias, snow_cover_bias)
MCE_WEATHER_GETTER_SEASON(season_cold_bias, cold_bias)
MCE_WEATHER_GETTER_SEASON(season_strength, strength)
#undef MCE_WEATHER_GETTER_SEASON

int push_season_tint(lua_State* L, const math::vec3& c) {
    lua_pushnumber(L, static_cast<double>(c.x));
    lua_pushnumber(L, static_cast<double>(c.y));
    lua_pushnumber(L, static_cast<double>(c.z));
    return 3;
}
int l_weather_season_spring_tint(lua_State* L) { return push_season_tint(L, check_season(L, "weather.season_spring_tint").spring_tint); }
int l_weather_season_autumn_tint(lua_State* L) { return push_season_tint(L, check_season(L, "weather.season_autumn_tint").autumn_tint); }
int l_weather_season_winter_tint(lua_State* L) { return push_season_tint(L, check_season(L, "weather.season_winter_tint").winter_tint); }

// weather.season_phase(): kit::season_phase on the first WorldClock's day and hour (day 0, hour 0 with no clock).
int l_weather_season_phase(lua_State* L) {
    const kit::Season& s = check_season(L, "weather.season_phase");
    World& w = world_of(L);
    u32 day = 0;
    f32 hour = 0.0f;
    const refl::TypeRecord* type = refl::find_type(StringId("WorldClock"));
    if (type && w.find_component(type->type_hash) != ecs::kInvalidComponent) {
        bool found = false;
        w.query<kit::WorldClock>().each([&](Entity, kit::WorldClock& c) {
            if (found) return;
            found = true;
            day = c.day;
            hour = c.hour;
        });
    }
    lua_pushnumber(L, static_cast<double>(kit::season_phase(s, day, hour)));
    return 1;
}

// weather.set_season{...}: validated before anything is written.
int l_weather_set_season(lua_State* L) {
    static const char* const kKeys[10] = {"enabled", "year_days", "start_phase", "spring_tint", "autumn_tint",
                                          "winter_tint", "winter_density", "snow_cover_bias", "cold_bias", "strength"};
    f64 value[10] = {};
    bool given[10] = {};
    f64 spring[3] = {};
    f64 autumn[3] = {};
    f64 winter[3] = {};
    read_settings(L, "weather.set_season", kKeys, 10,
                  "enabled, year_days, start_phase, spring_tint, autumn_tint, winter_tint, winter_density, "
                  "snow_cover_bias, cold_bias, strength",
                  value, given, 3, spring, 4, autumn, 5, winter);
    kit::Season& s = check_season(L, "weather.set_season");
    auto tint = [](math::vec3& dst, const f64* c) {
        dst.x = static_cast<f32>(clamp_range(c[0], 0.0, 4.0));
        dst.y = static_cast<f32>(clamp_range(c[1], 0.0, 4.0));
        dst.z = static_cast<f32>(clamp_range(c[2], 0.0, 4.0));
    };
    if (given[0]) s.enabled = value[0] != 0.0 ? 1 : 0;
    if (given[1]) s.year_days = static_cast<f32>(clamp_range(value[1], 1.0, 3650.0));
    if (given[2]) s.start_phase = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) tint(s.spring_tint, spring);
    if (given[4]) tint(s.autumn_tint, autumn);
    if (given[5]) tint(s.winter_tint, winter);
    if (given[6]) s.winter_density = static_cast<f32>(clamp_range(value[6], 0.0, 1.0));
    if (given[7]) s.snow_cover_bias = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) s.cold_bias = static_cast<f32>(clamp_range(value[8], 0.0, 1.0));
    if (given[9]) s.strength = static_cast<f32>(clamp_range(value[9], 0.0, 1.0));
    return 0;
}

// ---- Water ice (M-FREEZE step 20c) ----

kit::WaterIce& check_water_ice(lua_State* L, const char* fn) { return check_first<kit::WaterIce>(L, fn, "WaterIce"); }

#define MCE_WEATHER_GETTER_ICE(luaname, field)                                                          \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_water_ice(L, "weather." #luaname).field));            \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_ICE(water_ice_enabled, enabled)
MCE_WEATHER_GETTER_ICE(water_ice_freeze_start, freeze_start)
MCE_WEATHER_GETTER_ICE(water_ice_freeze_full, freeze_full)
MCE_WEATHER_GETTER_ICE(water_ice_manual, manual)
MCE_WEATHER_GETTER_ICE(water_ice_ice_roughness, ice_roughness)
MCE_WEATHER_GETTER_ICE(water_ice_ice_reflectivity, ice_reflectivity)
MCE_WEATHER_GETTER_ICE(water_ice_ice_depth_m, ice_depth_m)
MCE_WEATHER_GETTER_ICE(water_ice_calm, calm)
MCE_WEATHER_GETTER_ICE(water_ice_strength, strength)
#undef MCE_WEATHER_GETTER_ICE

int l_weather_water_ice_ice_color(lua_State* L) {
    return push_season_tint(L, check_water_ice(L, "weather.water_ice_ice_color").ice_color);
}

// weather.water_frozen(): kit::water_ice_amount on the first Season at the first WorldClock's day and hour, as the extract
// computes it (day 0, hour 0 with no clock; no Season is the manual floor).
int l_weather_water_frozen(lua_State* L) {
    const kit::WaterIce& ice = check_water_ice(L, "weather.water_frozen");
    World& w = world_of(L);
    const kit::Season* season = nullptr;
    const refl::TypeRecord* season_type = refl::find_type(StringId("Season"));
    if (season_type && w.find_component(season_type->type_hash) != ecs::kInvalidComponent) {
        w.query<const kit::Season&>().each([&](const kit::Season& c) {
            if (!season) season = &c;
        });
    }
    u32 day = 0;
    f32 hour = 0.0f;
    const refl::TypeRecord* clock_type = refl::find_type(StringId("WorldClock"));
    if (clock_type && w.find_component(clock_type->type_hash) != ecs::kInvalidComponent) {
        bool found = false;
        w.query<const kit::WorldClock&>().each([&](const kit::WorldClock& c) {
            if (found) return;
            found = true;
            day = c.day;
            hour = c.hour;
        });
    }
    const f32 phase = season ? kit::season_phase(*season, day, hour) : 0.0f;
    lua_pushnumber(L, static_cast<double>(kit::water_ice_amount(ice, season, phase)));
    return 1;
}

// weather.set_water_ice{...}: validated before anything is written.
int l_weather_set_water_ice(lua_State* L) {
    static const char* const kKeys[10] = {"enabled", "freeze_start", "freeze_full", "manual", "ice_color",
                                          "ice_roughness", "ice_reflectivity", "ice_depth_m", "calm", "strength"};
    f64 value[10] = {};
    bool given[10] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_water_ice", kKeys, 10,
                  "enabled, freeze_start, freeze_full, manual, ice_color, ice_roughness, ice_reflectivity, ice_depth_m, "
                  "calm, strength",
                  value, given, 4, color);
    kit::WaterIce& ice = check_water_ice(L, "weather.set_water_ice");
    if (given[0]) ice.enabled = value[0] != 0.0 ? 1 : 0;
    if (given[1]) ice.freeze_start = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) ice.freeze_full = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) ice.manual = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) {
        ice.ice_color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        ice.ice_color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        ice.ice_color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[5]) ice.ice_roughness = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) ice.ice_reflectivity = static_cast<f32>(clamp_range(value[6], 0.0, 1.0));
    if (given[7]) ice.ice_depth_m = static_cast<f32>(clamp_range(value[7], 0.05, 10.0));
    if (given[8]) ice.calm = static_cast<f32>(clamp_range(value[8], 0.0, 1.0));
    if (given[9]) ice.strength = static_cast<f32>(clamp_range(value[9], 0.0, 1.0));
    return 0;
}

// ---- Leaf fall (M-LEAFFALL step 21c) ----

kit::LeafFall& check_leaf_fall(lua_State* L, const char* fn) { return check_first<kit::LeafFall>(L, fn, "LeafFall"); }

#define MCE_WEATHER_GETTER_LEAF(luaname, field)                                                         \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_leaf_fall(L, "weather." #luaname).field));            \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_LEAF(leaf_fall_enabled, enabled)
MCE_WEATHER_GETTER_LEAF(leaf_fall_autumn_start, autumn_start)
MCE_WEATHER_GETTER_LEAF(leaf_fall_autumn_full, autumn_full)
MCE_WEATHER_GETTER_LEAF(leaf_fall_autumn_end_full, autumn_end_full)
MCE_WEATHER_GETTER_LEAF(leaf_fall_autumn_end, autumn_end)
MCE_WEATHER_GETTER_LEAF(leaf_fall_manual, manual)
MCE_WEATHER_GETTER_LEAF(leaf_fall_rate, rate)
MCE_WEATHER_GETTER_LEAF(leaf_fall_box_xz_m, box_xz_m)
MCE_WEATHER_GETTER_LEAF(leaf_fall_height_m, height_m)
MCE_WEATHER_GETTER_LEAF(leaf_fall_size_m, size_m)
MCE_WEATHER_GETTER_LEAF(leaf_fall_lifetime_s, lifetime_s)
MCE_WEATHER_GETTER_LEAF(leaf_fall_fall_speed, fall_speed)
MCE_WEATHER_GETTER_LEAF(leaf_fall_wind_follow, wind_follow)
MCE_WEATHER_GETTER_LEAF(leaf_fall_tint_follow, tint_follow)
MCE_WEATHER_GETTER_LEAF(leaf_fall_max_particles, max_particles)
MCE_WEATHER_GETTER_LEAF(leaf_fall_seed, seed)
MCE_WEATHER_GETTER_LEAF(leaf_fall_strength, strength)
#undef MCE_WEATHER_GETTER_LEAF

int l_weather_leaf_fall_leaf_color(lua_State* L) {
    return push_season_tint(L, check_leaf_fall(L, "weather.leaf_fall_leaf_color").leaf_color);
}

// weather.leaf_falling(): kit::leaf_fall_amount on the first Season at the first WorldClock's day and hour, as the extract
// computes it (day 0, hour 0 with no clock; no Season is the manual floor).
int l_weather_leaf_falling(lua_State* L) {
    const kit::LeafFall& lf = check_leaf_fall(L, "weather.leaf_falling");
    World& w = world_of(L);
    const kit::Season* season = nullptr;
    const refl::TypeRecord* season_type = refl::find_type(StringId("Season"));
    if (season_type && w.find_component(season_type->type_hash) != ecs::kInvalidComponent) {
        w.query<const kit::Season&>().each([&](const kit::Season& c) {
            if (!season) season = &c;
        });
    }
    u32 day = 0;
    f32 hour = 0.0f;
    const refl::TypeRecord* clock_type = refl::find_type(StringId("WorldClock"));
    if (clock_type && w.find_component(clock_type->type_hash) != ecs::kInvalidComponent) {
        bool found = false;
        w.query<const kit::WorldClock&>().each([&](const kit::WorldClock& c) {
            if (found) return;
            found = true;
            day = c.day;
            hour = c.hour;
        });
    }
    const f32 phase = season ? kit::season_phase(*season, day, hour) : 0.0f;
    lua_pushnumber(L, static_cast<double>(kit::leaf_fall_amount(lf, season, phase)));
    return 1;
}

// weather.set_leaf_fall{...}: validated before anything is written.
int l_weather_set_leaf_fall(lua_State* L) {
    static const char* const kKeys[18] = {"enabled",   "autumn_start", "autumn_full",    "autumn_end_full", "autumn_end",   "manual",
                                          "rate",      "box_xz_m",     "height_m",       "size_m",          "lifetime_s",   "fall_speed",
                                          "wind_follow", "leaf_color", "tint_follow",    "max_particles",   "seed",         "strength"};
    f64 value[18] = {};
    bool given[18] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_leaf_fall", kKeys, 18,
                  "enabled, autumn_start, autumn_full, autumn_end_full, autumn_end, manual, rate, box_xz_m, height_m, size_m, "
                  "lifetime_s, fall_speed, wind_follow, leaf_color, tint_follow, max_particles, seed, strength",
                  value, given, 13, color);
    kit::LeafFall& lf = check_leaf_fall(L, "weather.set_leaf_fall");
    if (given[0]) lf.enabled = value[0] != 0.0 ? 1 : 0;
    if (given[1]) lf.autumn_start = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) lf.autumn_full = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) lf.autumn_end_full = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) lf.autumn_end = static_cast<f32>(clamp_range(value[4], 0.0, 1.0));
    if (given[5]) lf.manual = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) lf.rate = static_cast<f32>(clamp_range(value[6], 0.0, 500.0));
    if (given[7]) lf.box_xz_m = static_cast<f32>(clamp_range(value[7], 1.0, 100.0));
    if (given[8]) lf.height_m = static_cast<f32>(clamp_range(value[8], 1.0, 50.0));
    if (given[9]) lf.size_m = static_cast<f32>(clamp_range(value[9], 0.02, 1.0));
    if (given[10]) lf.lifetime_s = static_cast<f32>(clamp_range(value[10], 1.0, 30.0));
    if (given[11]) lf.fall_speed = static_cast<f32>(clamp_range(value[11], 0.0, 5.0));
    if (given[12]) lf.wind_follow = static_cast<f32>(clamp_range(value[12], 0.0, 2.0));
    if (given[13]) {
        lf.leaf_color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        lf.leaf_color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        lf.leaf_color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[14]) lf.tint_follow = static_cast<f32>(clamp_range(value[14], 0.0, 1.0));
    if (given[15]) lf.max_particles = static_cast<u32>(clamp_range(value[15], 64.0, 8192.0));
    if (given[16]) lf.seed = static_cast<u32>(clamp_range(value[16], 0.0, 4294967295.0));
    if (given[17]) lf.strength = static_cast<f32>(clamp_range(value[17], 0.0, 1.0));
    return 0;
}

// ---- Fireflies (M-FIREFLIES step 23c) ----

kit::Firefly& check_firefly(lua_State* L, const char* fn) { return check_first<kit::Firefly>(L, fn, "Firefly"); }

#define MCE_WEATHER_GETTER_FIREFLY(luaname, field)                                                      \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_firefly(L, "weather." #luaname).field));              \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_FIREFLY(firefly_enabled, enabled)
MCE_WEATHER_GETTER_FIREFLY(firefly_summer_start, summer_start)
MCE_WEATHER_GETTER_FIREFLY(firefly_summer_full, summer_full)
MCE_WEATHER_GETTER_FIREFLY(firefly_summer_end_full, summer_end_full)
MCE_WEATHER_GETTER_FIREFLY(firefly_summer_end, summer_end)
MCE_WEATHER_GETTER_FIREFLY(firefly_manual, manual)
MCE_WEATHER_GETTER_FIREFLY(firefly_night_only, night_only)
MCE_WEATHER_GETTER_FIREFLY(firefly_min_darkness, min_darkness)
MCE_WEATHER_GETTER_FIREFLY(firefly_rate, rate)
MCE_WEATHER_GETTER_FIREFLY(firefly_box_xz_m, box_xz_m)
MCE_WEATHER_GETTER_FIREFLY(firefly_height_m, height_m)
MCE_WEATHER_GETTER_FIREFLY(firefly_size_m, size_m)
MCE_WEATHER_GETTER_FIREFLY(firefly_lifetime_s, lifetime_s)
MCE_WEATHER_GETTER_FIREFLY(firefly_drift_speed, drift_speed)
MCE_WEATHER_GETTER_FIREFLY(firefly_max_particles, max_particles)
MCE_WEATHER_GETTER_FIREFLY(firefly_seed, seed)
MCE_WEATHER_GETTER_FIREFLY(firefly_strength, strength)
#undef MCE_WEATHER_GETTER_FIREFLY

int l_weather_firefly_color(lua_State* L) {
    return push_season_tint(L, check_firefly(L, "weather.firefly_color").color);
}

// weather.fireflies_glowing(): kit::firefly_amount with the inputs the extract's firefly block uses: the first Season at the
// first WorldClock's day and hour (day 0, hour 0 with no clock; no Season is the manual floor), the night fade of the
// Environment3D hour (0 with no clock) and the first Weather's rain intensity while it rains or storms.
int l_weather_fireflies_glowing(lua_State* L) {
    const kit::Firefly& ff = check_firefly(L, "weather.fireflies_glowing");
    World& w = world_of(L);
    const kit::Season* season = nullptr;
    const refl::TypeRecord* season_type = refl::find_type(StringId("Season"));
    if (season_type && w.find_component(season_type->type_hash) != ecs::kInvalidComponent) {
        w.query<const kit::Season&>().each([&](const kit::Season& c) {
            if (!season) season = &c;
        });
    }
    u32 day = 0;
    f32 hour = 0.0f;
    const refl::TypeRecord* clock_type = refl::find_type(StringId("WorldClock"));
    if (clock_type && w.find_component(clock_type->type_hash) != ecs::kInvalidComponent) {
        bool found = false;
        w.query<const kit::WorldClock&>().each([&](const kit::WorldClock& c) {
            if (found) return;
            found = true;
            day = c.day;
            hour = c.hour;
        });
    }
    const f32 phase = season ? kit::season_phase(*season, day, hour) : 0.0f;
    const kit::Weather* wx = first_weather(w);
    const bool raining = wx && (wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm);
    const f32 rain = raining ? wx->intensity : 0.0f;
    f32 amount = kit::firefly_amount(ff, season, phase, aurora_night_fade(w), aurora_cloud_cover(w), rain);
    if (!std::isfinite(amount)) amount = 0.0f;
    lua_pushnumber(L, static_cast<double>(amount));
    return 1;
}

// weather.set_firefly{...}: validated before anything is written.
int l_weather_set_firefly(lua_State* L) {
    static const char* const kKeys[18] = {"enabled",  "summer_start", "summer_full", "summer_end_full", "summer_end",    "manual",
                                          "night_only", "min_darkness", "rate",       "box_xz_m",        "height_m",     "size_m",
                                          "lifetime_s", "drift_speed", "color",       "max_particles",   "seed",         "strength"};
    f64 value[18] = {};
    bool given[18] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_firefly", kKeys, 18,
                  "enabled, summer_start, summer_full, summer_end_full, summer_end, manual, night_only, min_darkness, rate, "
                  "box_xz_m, height_m, size_m, lifetime_s, drift_speed, color, max_particles, seed, strength",
                  value, given, 14, color);
    kit::Firefly& ff = check_firefly(L, "weather.set_firefly");
    if (given[0]) ff.enabled = value[0] != 0.0 ? 1 : 0;
    if (given[1]) ff.summer_start = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) ff.summer_full = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) ff.summer_end_full = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) ff.summer_end = static_cast<f32>(clamp_range(value[4], 0.0, 1.0));
    if (given[5]) ff.manual = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) ff.night_only = value[6] != 0.0 ? 1 : 0;
    if (given[7]) ff.min_darkness = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) ff.rate = static_cast<f32>(clamp_range(value[8], 0.0, 200.0));
    if (given[9]) ff.box_xz_m = static_cast<f32>(clamp_range(value[9], 1.0, 100.0));
    if (given[10]) ff.height_m = static_cast<f32>(clamp_range(value[10], 1.0, 20.0));
    if (given[11]) ff.size_m = static_cast<f32>(clamp_range(value[11], 0.02, 0.5));
    if (given[12]) ff.lifetime_s = static_cast<f32>(clamp_range(value[12], 1.0, 30.0));
    if (given[13]) ff.drift_speed = static_cast<f32>(clamp_range(value[13], 0.0, 2.0));
    if (given[14]) {
        ff.color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        ff.color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        ff.color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[15]) ff.max_particles = static_cast<u32>(clamp_range(value[15], 16.0, 4096.0));
    if (given[16]) ff.seed = static_cast<u32>(clamp_range(value[16], 0.0, 4294967295.0));
    if (given[17]) ff.strength = static_cast<f32>(clamp_range(value[17], 0.0, 1.0));
    return 0;
}

// ---- Pollen (M-POLLEN step 24c) ----

kit::Pollen& check_pollen(lua_State* L, const char* fn) { return check_first<kit::Pollen>(L, fn, "Pollen"); }

#define MCE_WEATHER_GETTER_POLLEN(luaname, field)                                                       \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_pollen(L, "weather." #luaname).field));               \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_POLLEN(pollen_enabled, enabled)
MCE_WEATHER_GETTER_POLLEN(pollen_spring_start, spring_start)
MCE_WEATHER_GETTER_POLLEN(pollen_spring_full, spring_full)
MCE_WEATHER_GETTER_POLLEN(pollen_spring_end_full, spring_end_full)
MCE_WEATHER_GETTER_POLLEN(pollen_spring_end, spring_end)
MCE_WEATHER_GETTER_POLLEN(pollen_manual, manual)
MCE_WEATHER_GETTER_POLLEN(pollen_day_only, day_only)
MCE_WEATHER_GETTER_POLLEN(pollen_min_daylight, min_daylight)
MCE_WEATHER_GETTER_POLLEN(pollen_rate, rate)
MCE_WEATHER_GETTER_POLLEN(pollen_box_xz_m, box_xz_m)
MCE_WEATHER_GETTER_POLLEN(pollen_height_m, height_m)
MCE_WEATHER_GETTER_POLLEN(pollen_size_m, size_m)
MCE_WEATHER_GETTER_POLLEN(pollen_lifetime_s, lifetime_s)
MCE_WEATHER_GETTER_POLLEN(pollen_fall_speed, fall_speed)
MCE_WEATHER_GETTER_POLLEN(pollen_wind_follow, wind_follow)
MCE_WEATHER_GETTER_POLLEN(pollen_max_particles, max_particles)
MCE_WEATHER_GETTER_POLLEN(pollen_seed, seed)
MCE_WEATHER_GETTER_POLLEN(pollen_strength, strength)
#undef MCE_WEATHER_GETTER_POLLEN

int l_weather_pollen_color(lua_State* L) { return push_season_tint(L, check_pollen(L, "weather.pollen_color").color); }

// weather.pollen_drifting(): kit::pollen_amount with the inputs the extract's pollen block uses: the first Season at the
// first WorldClock's day and hour (day 0, hour 0 with no clock; no Season is the manual floor), the night fade of the
// Environment3D hour (0 with no clock) and the first Weather's rain intensity while it rains or storms.
int l_weather_pollen_drifting(lua_State* L) {
    const kit::Pollen& po = check_pollen(L, "weather.pollen_drifting");
    World& w = world_of(L);
    const kit::Season* season = nullptr;
    const refl::TypeRecord* season_type = refl::find_type(StringId("Season"));
    if (season_type && w.find_component(season_type->type_hash) != ecs::kInvalidComponent) {
        w.query<const kit::Season&>().each([&](const kit::Season& c) {
            if (!season) season = &c;
        });
    }
    u32 day = 0;
    f32 hour = 0.0f;
    const refl::TypeRecord* clock_type = refl::find_type(StringId("WorldClock"));
    if (clock_type && w.find_component(clock_type->type_hash) != ecs::kInvalidComponent) {
        bool found = false;
        w.query<const kit::WorldClock&>().each([&](const kit::WorldClock& c) {
            if (found) return;
            found = true;
            day = c.day;
            hour = c.hour;
        });
    }
    const f32 phase = season ? kit::season_phase(*season, day, hour) : 0.0f;
    const kit::Weather* wx = first_weather(w);
    const bool raining = wx && (wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm);
    const f32 rain = raining ? wx->intensity : 0.0f;
    f32 amount = kit::pollen_amount(po, season, phase, aurora_night_fade(w), rain);
    if (!std::isfinite(amount)) amount = 0.0f;
    lua_pushnumber(L, static_cast<double>(amount));
    return 1;
}

// weather.set_pollen{...}: validated before anything is written.
int l_weather_set_pollen(lua_State* L) {
    static const char* const kKeys[19] = {"enabled", "spring_start", "spring_full", "spring_end_full", "spring_end", "manual", "day_only", "min_daylight", "rate", "box_xz_m", "height_m", "size_m", "lifetime_s", "fall_speed", "wind_follow", "color", "max_particles", "seed", "strength"};
    f64 value[19] = {};
    bool given[19] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_pollen", kKeys, 19,
                  "enabled, spring_start, spring_full, spring_end_full, spring_end, manual, day_only, min_daylight, rate, box_xz_m, height_m, size_m, lifetime_s, fall_speed, wind_follow, color, max_particles, seed, strength",
                  value, given, 15, color);
    kit::Pollen& po = check_pollen(L, "weather.set_pollen");
    if (given[0]) po.enabled = value[0] != 0.0 ? 1 : 0;
    if (given[1]) po.spring_start = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) po.spring_full = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) po.spring_end_full = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) po.spring_end = static_cast<f32>(clamp_range(value[4], 0.0, 1.0));
    if (given[5]) po.manual = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) po.day_only = value[6] != 0.0 ? 1 : 0;
    if (given[7]) po.min_daylight = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) po.rate = static_cast<f32>(clamp_range(value[8], 0.0, 200.0));
    if (given[9]) po.box_xz_m = static_cast<f32>(clamp_range(value[9], 1.0, 100.0));
    if (given[10]) po.height_m = static_cast<f32>(clamp_range(value[10], 1.0, 30.0));
    if (given[11]) po.size_m = static_cast<f32>(clamp_range(value[11], 0.02, 0.5));
    if (given[12]) po.lifetime_s = static_cast<f32>(clamp_range(value[12], 1.0, 30.0));
    if (given[13]) po.fall_speed = static_cast<f32>(clamp_range(value[13], 0.0, 2.0));
    if (given[14]) po.wind_follow = static_cast<f32>(clamp_range(value[14], 0.0, 2.0));
    if (given[15]) {
        po.color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        po.color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        po.color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[16]) po.max_particles = static_cast<u32>(clamp_range(value[16], 16.0, 4096.0));
    if (given[17]) po.seed = static_cast<u32>(clamp_range(value[17], 0.0, 4294967295.0));
    if (given[18]) po.strength = static_cast<f32>(clamp_range(value[18], 0.0, 1.0));
    return 0;
}

// ---- Dust (M-DUSTWIND step 25c) ----

kit::Dust& check_dust(lua_State* L, const char* fn) { return check_first<kit::Dust>(L, fn, "Dust"); }

#define MCE_WEATHER_GETTER_DUST(luaname, field)                                                         \
    int l_weather_##luaname(lua_State* L) {                                                              \
        lua_pushnumber(L, static_cast<double>(check_dust(L, "weather." #luaname).field));                 \
        return 1;                                                                                       \
    }
MCE_WEATHER_GETTER_DUST(dust_enabled, enabled)
MCE_WEATHER_GETTER_DUST(dust_wind_start, wind_start)
MCE_WEATHER_GETTER_DUST(dust_wind_full, wind_full)
MCE_WEATHER_GETTER_DUST(dust_manual, manual)
MCE_WEATHER_GETTER_DUST(dust_rate, rate)
MCE_WEATHER_GETTER_DUST(dust_box_xz_m, box_xz_m)
MCE_WEATHER_GETTER_DUST(dust_height_m, height_m)
MCE_WEATHER_GETTER_DUST(dust_size_m, size_m)
MCE_WEATHER_GETTER_DUST(dust_lifetime_s, lifetime_s)
MCE_WEATHER_GETTER_DUST(dust_speed, speed)
MCE_WEATHER_GETTER_DUST(dust_wind_follow, wind_follow)
MCE_WEATHER_GETTER_DUST(dust_max_particles, max_particles)
MCE_WEATHER_GETTER_DUST(dust_seed, seed)
MCE_WEATHER_GETTER_DUST(dust_strength, strength)
#undef MCE_WEATHER_GETTER_DUST

int l_weather_dust_color(lua_State* L) { return push_season_tint(L, check_dust(L, "weather.dust_color").color); }

// weather.dust_blowing(): kit::dust_amount with the inputs the extract's dust block uses: the first Weather's rain
// intensity while it rains or storms and its effective wind with the gust sway of the first WindGust (calm, rain 0 with no
// Weather).
int l_weather_dust_blowing(lua_State* L) {
    const kit::Dust& du = check_dust(L, "weather.dust_blowing");
    World& w = world_of(L);
    const kit::Weather* wx = first_weather(w);
    const kit::WindGust* gust = nullptr;
    const refl::TypeRecord* gust_type = refl::find_type(StringId("WindGust"));
    if (gust_type && w.find_component(gust_type->type_hash) != ecs::kInvalidComponent) {
        w.query<const kit::WindGust&>().each([&](const kit::WindGust& c) {
            if (!gust) gust = &c;
        });
    }
    const bool raining = wx && (wx->current == kit::WeatherKind::Rain || wx->current == kit::WeatherKind::Storm);
    const f32 rain = raining ? wx->intensity : 0.0f;
    const f32 wind = wx ? kit::effective_wind(*wx, gust, kit::kGustSway) : 0.0f;
    f32 amount = kit::dust_amount(du, wind, rain);
    if (!std::isfinite(amount)) amount = 0.0f;
    lua_pushnumber(L, static_cast<double>(amount));
    return 1;
}

// weather.set_dust{...}: validated before anything is written.
int l_weather_set_dust(lua_State* L) {
    static const char* const kKeys[15] = {"enabled", "wind_start", "wind_full", "manual", "rate", "box_xz_m", "height_m", "size_m", "lifetime_s", "speed", "wind_follow", "color", "max_particles", "seed", "strength"};
    f64 value[15] = {};
    bool given[15] = {};
    f64 color[3] = {};
    read_settings(L, "weather.set_dust", kKeys, 15, "enabled, wind_start, wind_full, manual, rate, box_xz_m, height_m, size_m, lifetime_s, speed, wind_follow, color, max_particles, seed, strength",
                  value, given, 11, color);
    kit::Dust& du = check_dust(L, "weather.set_dust");
    if (given[0]) du.enabled = value[0] != 0.0 ? 1 : 0;
    if (given[1]) du.wind_start = static_cast<f32>(clamp_range(value[1], 0.0, 1.0));
    if (given[2]) du.wind_full = static_cast<f32>(clamp_range(value[2], 0.0, 1.0));
    if (given[3]) du.manual = static_cast<f32>(clamp_range(value[3], 0.0, 1.0));
    if (given[4]) du.rate = static_cast<f32>(clamp_range(value[4], 0.0, 400.0));
    if (given[5]) du.box_xz_m = static_cast<f32>(clamp_range(value[5], 1.0, 100.0));
    if (given[6]) du.height_m = static_cast<f32>(clamp_range(value[6], 1.0, 20.0));
    if (given[7]) du.size_m = static_cast<f32>(clamp_range(value[7], 0.01, 0.5));
    if (given[8]) du.lifetime_s = static_cast<f32>(clamp_range(value[8], 0.5, 10.0));
    if (given[9]) du.speed = static_cast<f32>(clamp_range(value[9], 0.0, 30.0));
    if (given[10]) du.wind_follow = static_cast<f32>(clamp_range(value[10], 0.0, 2.0));
    if (given[11]) {
        du.color.x = static_cast<f32>(clamp_range(color[0], 0.0, 1.0));
        du.color.y = static_cast<f32>(clamp_range(color[1], 0.0, 1.0));
        du.color.z = static_cast<f32>(clamp_range(color[2], 0.0, 1.0));
    }
    if (given[12]) du.max_particles = static_cast<u32>(clamp_range(value[12], 16.0, 4096.0));
    if (given[13]) du.seed = static_cast<u32>(clamp_range(value[13], 0.0, 4294967295.0));
    if (given[14]) du.strength = static_cast<f32>(clamp_range(value[14], 0.0, 1.0));
    return 0;
}

// ---- Water (M-WATER step 15d) ----

kit::Water& check_water(lua_State* L, const char* fn) { return check_first<kit::Water>(L, fn, "Water"); }

#define MCE_WATER_GETTER(luaname, field)                                                    \
    int l_water_##luaname(lua_State* L) {                                                     \
        lua_pushnumber(L, static_cast<double>(check_water(L, "water." #luaname).field));       \
        return 1;                                                                             \
    }
MCE_WATER_GETTER(level, level_y)
MCE_WATER_GETTER(half_extent, half_extent)
MCE_WATER_GETTER(wave_count, wave_count)
MCE_WATER_GETTER(amplitude, amplitude)
MCE_WATER_GETTER(wavelength, wavelength)
MCE_WATER_GETTER(steepness, steepness)
MCE_WATER_GETTER(wind_dir, wind_dir_deg)
MCE_WATER_GETTER(wind_follow, wind_follow)
MCE_WATER_GETTER(speed_scale, speed_scale)
MCE_WATER_GETTER(depth_scale, depth_scale_m)
MCE_WATER_GETTER(reflectivity, reflectivity)
MCE_WATER_GETTER(roughness, roughness)
MCE_WATER_GETTER(foam_amount, foam_amount)
MCE_WATER_GETTER(foam_width, foam_width_m)
MCE_WATER_GETTER(enabled, enabled)
#undef MCE_WATER_GETTER

int push_water_color(lua_State* L, const math::vec3& c) {
    lua_pushnumber(L, static_cast<double>(c.x));
    lua_pushnumber(L, static_cast<double>(c.y));
    lua_pushnumber(L, static_cast<double>(c.z));
    return 3;
}
int l_water_shallow_color(lua_State* L) { return push_water_color(L, check_water(L, "water.shallow_color").shallow_color); }
int l_water_deep_color(lua_State* L) { return push_water_color(L, check_water(L, "water.deep_color").deep_color); }

// The main wave direction the extract uses: the first WindDirection's heading when wind_follow > 0, else wind_dir_deg.
f32 water_main_direction(World& w, const kit::Water& water) {
    if (water.wind_follow > 0.0f) {
        const refl::TypeRecord* type = refl::find_type(StringId("WindDirection"));
        if (type && w.find_component(type->type_hash) != ecs::kInvalidComponent) {
            bool found = false;
            f32 deg = 0.0f;
            w.query<kit::WindDirection>().each([&](Entity, kit::WindDirection& c) {
                if (found) return;
                found = true;
                deg = static_cast<f32>(static_cast<f64>(c.angle_q) / 65536.0);
            });
            if (found) return deg;
        }
    }
    return water.wind_dir_deg;
}

// The scene time the queries use: the running tick times the fixed step, in seconds.
f64 water_scene_time(lua_State* L) {
    const Frame& f = frame_of(L);
    return static_cast<f64>(f.tick) * static_cast<f64>(f.dt);
}

// A finite number argument, or a named error.
f64 check_water_coord(lua_State* L, int idx, const char* fn, const char* what) {
    const f64 v = luaL_checknumber(L, idx);
    if (!std::isfinite(v)) luaL_error(L, "%s: %s is not a finite number", fn, what);
    return v;
}

int l_water_height_at(lua_State* L) {
    const f64 x = check_water_coord(L, 1, "water.height_at", "x");
    const f64 z = check_water_coord(L, 2, "water.height_at", "z");
    World& w = world_of(L);
    const kit::Water& water = check_water(L, "water.height_at");
    const math::vec2 p{static_cast<f32>(x), static_cast<f32>(z)};
    lua_pushnumber(L, static_cast<double>(kit::water_height_at(water, p, water_scene_time(L), water_main_direction(w, water))));
    return 1;
}

int l_water_normal_at(lua_State* L) {
    const f64 x = check_water_coord(L, 1, "water.normal_at", "x");
    const f64 z = check_water_coord(L, 2, "water.normal_at", "z");
    World& w = world_of(L);
    const kit::Water& water = check_water(L, "water.normal_at");
    const math::vec2 p{static_cast<f32>(x), static_cast<f32>(z)};
    return push_water_color(L, kit::water_normal_at(water, p, water_scene_time(L), water_main_direction(w, water)));
}

int l_water_submerged(lua_State* L) {
    const f64 x = check_water_coord(L, 1, "water.submerged", "x");
    const f64 y = check_water_coord(L, 2, "water.submerged", "y");
    const f64 z = check_water_coord(L, 3, "water.submerged", "z");
    World& w = world_of(L);
    const kit::Water& water = check_water(L, "water.submerged");
    const math::vec2 p{static_cast<f32>(x), static_cast<f32>(z)};
    const f32 h = kit::water_height_at(water, p, water_scene_time(L), water_main_direction(w, water));
    lua_pushboolean(L, static_cast<f32>(y) < h ? 1 : 0);
    return 1;
}

// water.set{...}: validated before anything is written.
int l_water_set(lua_State* L) {
    static const char* const kKeys[17] = {"level", "half_extent", "wave_count", "amplitude", "wavelength", "steepness",
                                          "wind_dir", "wind_follow", "speed_scale", "shallow_color", "deep_color",
                                          "depth_scale", "reflectivity", "roughness", "foam_amount", "foam_width",
                                          "enabled"};
    f64 value[17] = {};
    bool given[17] = {};
    f64 shallow[3] = {};
    f64 deep[3] = {};
    read_settings(L, "water.set", kKeys, 17,
                  "level, half_extent, wave_count, amplitude, wavelength, steepness, wind_dir, wind_follow, "
                  "speed_scale, shallow_color, deep_color, depth_scale, reflectivity, roughness, foam_amount, "
                  "foam_width, enabled",
                  value, given, 9, shallow, 10, deep);
    kit::Water& m = check_water(L, "water.set");
    if (given[0]) m.level_y = static_cast<f32>(clamp_range(value[0], -1000.0, 1000.0));
    if (given[1]) m.half_extent = static_cast<f32>(clamp_range(value[1], 1.0, 20000.0));
    if (given[2]) m.wave_count = static_cast<u8>(std::floor(clamp_range(value[2], 1.0, 8.0)));
    if (given[3]) m.amplitude = static_cast<f32>(clamp_range(value[3], 0.0, 10.0));
    if (given[4]) m.wavelength = static_cast<f32>(clamp_range(value[4], 1.0, 500.0));
    if (given[5]) m.steepness = static_cast<f32>(clamp_range(value[5], 0.0, 1.0));
    if (given[6]) m.wind_dir_deg = static_cast<f32>(clamp_range(value[6], 0.0, 360.0));
    if (given[7]) m.wind_follow = static_cast<f32>(clamp_range(value[7], 0.0, 1.0));
    if (given[8]) m.speed_scale = static_cast<f32>(clamp_range(value[8], 0.0, 4.0));
    if (given[9]) {
        m.shallow_color.x = static_cast<f32>(clamp_range(shallow[0], 0.0, 1.0));
        m.shallow_color.y = static_cast<f32>(clamp_range(shallow[1], 0.0, 1.0));
        m.shallow_color.z = static_cast<f32>(clamp_range(shallow[2], 0.0, 1.0));
    }
    if (given[10]) {
        m.deep_color.x = static_cast<f32>(clamp_range(deep[0], 0.0, 1.0));
        m.deep_color.y = static_cast<f32>(clamp_range(deep[1], 0.0, 1.0));
        m.deep_color.z = static_cast<f32>(clamp_range(deep[2], 0.0, 1.0));
    }
    if (given[11]) m.depth_scale_m = static_cast<f32>(clamp_range(value[11], 0.1, 200.0));
    if (given[12]) m.reflectivity = static_cast<f32>(clamp_range(value[12], 0.0, 1.0));
    if (given[13]) m.roughness = static_cast<f32>(clamp_range(value[13], 0.0, 1.0));
    if (given[14]) m.foam_amount = static_cast<f32>(clamp_range(value[14], 0.0, 1.0));
    if (given[15]) m.foam_width_m = static_cast<f32>(clamp_range(value[15], 0.05, 20.0));
    if (given[16]) m.enabled = value[16] != 0.0 ? 1 : 0;
    return 0;
}

const luaL_Reg kWaterLib[] = {
    {"level", l_water_level},
    {"half_extent", l_water_half_extent},
    {"wave_count", l_water_wave_count},
    {"amplitude", l_water_amplitude},
    {"wavelength", l_water_wavelength},
    {"steepness", l_water_steepness},
    {"wind_dir", l_water_wind_dir},
    {"wind_follow", l_water_wind_follow},
    {"speed_scale", l_water_speed_scale},
    {"shallow_color", l_water_shallow_color},
    {"deep_color", l_water_deep_color},
    {"depth_scale", l_water_depth_scale},
    {"reflectivity", l_water_reflectivity},
    {"roughness", l_water_roughness},
    {"foam_amount", l_water_foam_amount},
    {"foam_width", l_water_foam_width},
    {"enabled", l_water_enabled},
    {"height_at", l_water_height_at},
    {"normal_at", l_water_normal_at},
    {"submerged", l_water_submerged},
    {"set", l_water_set},
    {nullptr, nullptr},
};

const luaL_Reg kWeatherLib[] = {
    {"kind", l_weather_kind},
    {"target", l_weather_target},
    {"set", l_weather_set},
    {"intensity", l_weather_intensity},
    {"wind", l_weather_wind},
    {"wetness", l_weather_wetness},
    {"wet_strength", l_weather_wet_strength},
    {"wet_darken", l_weather_wet_darken},
    {"wet_puddles", l_weather_wet_puddles},
    {"wet_puddle_scale", l_weather_wet_puddle_scale},
    {"set_wet", l_weather_set_wet},
    {"sway_amplitude", l_weather_sway_amplitude},
    {"sway_frequency", l_weather_sway_frequency},
    {"sway_gust", l_weather_sway_gust},
    {"sway_flutter", l_weather_sway_flutter},
    {"set_sway", l_weather_set_sway},
    {"snow_cover", l_weather_snow_cover},
    {"snow_strength", l_weather_snow_strength},
    {"snow_build", l_weather_snow_build},
    {"snow_melt", l_weather_snow_melt},
    {"snow_coverage", l_weather_snow_coverage},
    {"snow_scale", l_weather_snow_scale},
    {"snow_wind", l_weather_snow_wind},
    {"set_snow", l_weather_set_snow},
    {"ripples_strength", l_weather_ripples_strength},
    {"ripples_density", l_weather_ripples_density},
    {"ripples_speed", l_weather_ripples_speed},
    {"ripples_ring_width", l_weather_ripples_ring_width},
    {"set_ripples", l_weather_set_ripples},
    {"splash_rate", l_weather_splash_rate},
    {"splash_height", l_weather_splash_height},
    {"splash_life", l_weather_splash_life},
    {"splash_radius", l_weather_splash_radius},
    {"splash_seed", l_weather_splash_seed},
    {"set_splash", l_weather_set_splash},
    {"thunder_speed", l_weather_thunder_speed},
    {"thunder_volume", l_weather_thunder_volume},
    {"set_thunder", l_weather_set_thunder},
    {"wind_degrees", l_weather_wind_degrees},
    {"wind_drift", l_weather_wind_drift},
    {"wind_heading", l_weather_wind_heading},
    {"set_wind_direction", l_weather_set_wind_direction},
    {"cloud_strength", l_weather_cloud_strength},
    {"cloud_fair_coverage", l_weather_cloud_fair_coverage},
    {"cloud_overcast_coverage", l_weather_cloud_overcast_coverage},
    {"cloud_scale", l_weather_cloud_scale},
    {"cloud_speed_scale", l_weather_cloud_speed_scale},
    {"cloud_height", l_weather_cloud_height},
    {"set_clouds", l_weather_set_clouds},
    {"rain_level", l_weather_rain_level},
    {"wind_level", l_weather_wind_level},
    {"audio_rain_db", l_weather_audio_rain_db},
    {"audio_wind_db", l_weather_audio_wind_db},
    {"audio_fade", l_weather_audio_fade},
    {"audio_enabled", l_weather_audio_enabled},
    {"set_audio", l_weather_set_audio},
    {"bolt_min_range", l_weather_bolt_min_range},
    {"bolt_max_range", l_weather_bolt_max_range},
    {"bolt_ground_y", l_weather_bolt_ground_y},
    {"bolt_height", l_weather_bolt_height},
    {"bolt_width", l_weather_bolt_width},
    {"bolt_segments", l_weather_bolt_segments},
    {"bolt_jitter", l_weather_bolt_jitter},
    {"bolt_fork", l_weather_bolt_fork},
    {"bolt_enabled", l_weather_bolt_enabled},
    {"bolt_light", l_weather_bolt_light},
    {"bolt_light_radius", l_weather_bolt_light_radius},
    {"bolt_distance", l_weather_bolt_distance},
    {"bolt_pos", l_weather_bolt_pos},
    {"bolt_active", l_weather_bolt_active},
    {"set_bolt", l_weather_set_bolt},
    {"gust_strength", l_weather_gust_strength},
    {"gust_min_gap", l_weather_gust_min_gap},
    {"gust_max_gap", l_weather_gust_max_gap},
    {"gust_min_rise", l_weather_gust_min_rise},
    {"gust_max_rise", l_weather_gust_max_rise},
    {"gust_min_fall", l_weather_gust_min_fall},
    {"gust_max_fall", l_weather_gust_max_fall},
    {"gust_min_peak", l_weather_gust_min_peak},
    {"gust_max_peak", l_weather_gust_max_peak},
    {"gust_seed", l_weather_gust_seed},
    {"gust_affects", l_weather_gust_affects},
    {"gust_enabled", l_weather_gust_enabled},
    {"gust_level", l_weather_gust_level},
    {"effective_wind", l_weather_effective_wind},
    {"set_gust", l_weather_set_gust},
    {"mist_strength", l_weather_mist_strength},
    {"mist_peak_hour", l_weather_mist_peak_hour},
    {"mist_rise", l_weather_mist_rise},
    {"mist_burn", l_weather_mist_burn},
    {"mist_base_humidity", l_weather_mist_base_humidity},
    {"mist_wet_gain", l_weather_mist_wet_gain},
    {"mist_weather_damp", l_weather_mist_weather_damp},
    {"mist_density", l_weather_mist_density},
    {"mist_thickness", l_weather_mist_thickness},
    {"mist_base_height", l_weather_mist_base_height},
    {"mist_exp2_density", l_weather_mist_exp2_density},
    {"mist_color", l_weather_mist_color},
    {"mist_enabled", l_weather_mist_enabled},
    {"mist_level", l_weather_mist_level},
    {"set_mist", l_weather_set_mist},
    {"moon_phase_offset", l_weather_moon_phase_offset},
    {"moon_cycle_days", l_weather_moon_cycle_days},
    {"moon_freeze_phase", l_weather_moon_freeze_phase},
    {"moon_inclination", l_weather_moon_inclination},
    {"moon_light_intensity", l_weather_moon_light_intensity},
    {"moon_light_color", l_weather_moon_light_color},
    {"moon_light_shadows", l_weather_moon_light_shadows},
    {"moon_disk_size", l_weather_moon_disk_size},
    {"moon_disk_intensity", l_weather_moon_disk_intensity},
    {"moon_star_density", l_weather_moon_star_density},
    {"moon_star_brightness", l_weather_moon_star_brightness},
    {"moon_star_twinkle", l_weather_moon_star_twinkle},
    {"moon_enabled", l_weather_moon_enabled},
    {"moon_phase", l_weather_moon_phase},
    {"moon_illumination", l_weather_moon_illumination},
    {"moon_direction", l_weather_moon_direction},
    {"moon_level", l_weather_moon_level},
    {"set_moon", l_weather_set_moon},
    {"aurora_latitude", l_weather_aurora_latitude},
    {"aurora_gate_low", l_weather_aurora_gate_low},
    {"aurora_gate_high", l_weather_aurora_gate_high},
    {"aurora_strength", l_weather_aurora_strength},
    {"aurora_curtain_count", l_weather_aurora_curtain_count},
    {"aurora_base_height", l_weather_aurora_base_height},
    {"aurora_height_span", l_weather_aurora_height_span},
    {"aurora_azimuth", l_weather_aurora_azimuth},
    {"aurora_width", l_weather_aurora_width},
    {"aurora_speed", l_weather_aurora_speed},
    {"aurora_low_color", l_weather_aurora_low_color},
    {"aurora_high_color", l_weather_aurora_high_color},
    {"aurora_night_only", l_weather_aurora_night_only},
    {"aurora_enabled", l_weather_aurora_enabled},
    {"aurora_gate", l_weather_aurora_gate},
    {"aurora_level", l_weather_aurora_level},
    {"set_aurora", l_weather_set_aurora},
    {"rainbow_strength", l_weather_rainbow_strength},
    {"rainbow_band_width", l_weather_rainbow_band_width},
    {"rainbow_secondary", l_weather_rainbow_secondary},
    {"rainbow_min_wetness", l_weather_rainbow_min_wetness},
    {"rainbow_max_sun_elevation", l_weather_rainbow_max_sun_elevation},
    {"rainbow_enabled", l_weather_rainbow_enabled},
    {"rainbow_level", l_weather_rainbow_level},
    {"set_rainbow", l_weather_set_rainbow},
    {"meteor_rate", l_weather_meteor_rate},
    {"meteor_brightness", l_weather_meteor_brightness},
    {"meteor_length", l_weather_meteor_length},
    {"meteor_speed", l_weather_meteor_speed},
    {"meteor_min_darkness", l_weather_meteor_min_darkness},
    {"meteor_night_only", l_weather_meteor_night_only},
    {"meteor_seed", l_weather_meteor_seed},
    {"meteor_enabled", l_weather_meteor_enabled},
    {"meteor_level", l_weather_meteor_level},
    {"set_meteor", l_weather_set_meteor},
    {"season_enabled", l_weather_season_enabled},
    {"season_year_days", l_weather_season_year_days},
    {"season_start_phase", l_weather_season_start_phase},
    {"season_winter_density", l_weather_season_winter_density},
    {"season_snow_cover_bias", l_weather_season_snow_cover_bias},
    {"season_cold_bias", l_weather_season_cold_bias},
    {"season_strength", l_weather_season_strength},
    {"season_spring_tint", l_weather_season_spring_tint},
    {"season_autumn_tint", l_weather_season_autumn_tint},
    {"season_winter_tint", l_weather_season_winter_tint},
    {"season_phase", l_weather_season_phase},
    {"set_season", l_weather_set_season},
    {"water_ice_enabled", l_weather_water_ice_enabled},
    {"water_ice_freeze_start", l_weather_water_ice_freeze_start},
    {"water_ice_freeze_full", l_weather_water_ice_freeze_full},
    {"water_ice_manual", l_weather_water_ice_manual},
    {"water_ice_ice_roughness", l_weather_water_ice_ice_roughness},
    {"water_ice_ice_reflectivity", l_weather_water_ice_ice_reflectivity},
    {"water_ice_ice_depth_m", l_weather_water_ice_ice_depth_m},
    {"water_ice_calm", l_weather_water_ice_calm},
    {"water_ice_strength", l_weather_water_ice_strength},
    {"water_ice_ice_color", l_weather_water_ice_ice_color},
    {"water_frozen", l_weather_water_frozen},
    {"set_water_ice", l_weather_set_water_ice},
    {"leaf_fall_enabled", l_weather_leaf_fall_enabled},
    {"leaf_fall_autumn_start", l_weather_leaf_fall_autumn_start},
    {"leaf_fall_autumn_full", l_weather_leaf_fall_autumn_full},
    {"leaf_fall_autumn_end_full", l_weather_leaf_fall_autumn_end_full},
    {"leaf_fall_autumn_end", l_weather_leaf_fall_autumn_end},
    {"leaf_fall_manual", l_weather_leaf_fall_manual},
    {"leaf_fall_rate", l_weather_leaf_fall_rate},
    {"leaf_fall_box_xz_m", l_weather_leaf_fall_box_xz_m},
    {"leaf_fall_height_m", l_weather_leaf_fall_height_m},
    {"leaf_fall_size_m", l_weather_leaf_fall_size_m},
    {"leaf_fall_lifetime_s", l_weather_leaf_fall_lifetime_s},
    {"leaf_fall_fall_speed", l_weather_leaf_fall_fall_speed},
    {"leaf_fall_wind_follow", l_weather_leaf_fall_wind_follow},
    {"leaf_fall_tint_follow", l_weather_leaf_fall_tint_follow},
    {"leaf_fall_max_particles", l_weather_leaf_fall_max_particles},
    {"leaf_fall_seed", l_weather_leaf_fall_seed},
    {"leaf_fall_strength", l_weather_leaf_fall_strength},
    {"leaf_fall_leaf_color", l_weather_leaf_fall_leaf_color},
    {"leaf_falling", l_weather_leaf_falling},
    {"set_leaf_fall", l_weather_set_leaf_fall},
    {"firefly_enabled", l_weather_firefly_enabled},
    {"firefly_summer_start", l_weather_firefly_summer_start},
    {"firefly_summer_full", l_weather_firefly_summer_full},
    {"firefly_summer_end_full", l_weather_firefly_summer_end_full},
    {"firefly_summer_end", l_weather_firefly_summer_end},
    {"firefly_manual", l_weather_firefly_manual},
    {"firefly_night_only", l_weather_firefly_night_only},
    {"firefly_min_darkness", l_weather_firefly_min_darkness},
    {"firefly_rate", l_weather_firefly_rate},
    {"firefly_box_xz_m", l_weather_firefly_box_xz_m},
    {"firefly_height_m", l_weather_firefly_height_m},
    {"firefly_size_m", l_weather_firefly_size_m},
    {"firefly_lifetime_s", l_weather_firefly_lifetime_s},
    {"firefly_drift_speed", l_weather_firefly_drift_speed},
    {"firefly_max_particles", l_weather_firefly_max_particles},
    {"firefly_seed", l_weather_firefly_seed},
    {"firefly_strength", l_weather_firefly_strength},
    {"firefly_color", l_weather_firefly_color},
    {"fireflies_glowing", l_weather_fireflies_glowing},
    {"set_firefly", l_weather_set_firefly},
    {"pollen_enabled", l_weather_pollen_enabled},
    {"pollen_spring_start", l_weather_pollen_spring_start},
    {"pollen_spring_full", l_weather_pollen_spring_full},
    {"pollen_spring_end_full", l_weather_pollen_spring_end_full},
    {"pollen_spring_end", l_weather_pollen_spring_end},
    {"pollen_manual", l_weather_pollen_manual},
    {"pollen_day_only", l_weather_pollen_day_only},
    {"pollen_min_daylight", l_weather_pollen_min_daylight},
    {"pollen_rate", l_weather_pollen_rate},
    {"pollen_box_xz_m", l_weather_pollen_box_xz_m},
    {"pollen_height_m", l_weather_pollen_height_m},
    {"pollen_size_m", l_weather_pollen_size_m},
    {"pollen_lifetime_s", l_weather_pollen_lifetime_s},
    {"pollen_fall_speed", l_weather_pollen_fall_speed},
    {"pollen_wind_follow", l_weather_pollen_wind_follow},
    {"pollen_max_particles", l_weather_pollen_max_particles},
    {"pollen_seed", l_weather_pollen_seed},
    {"pollen_strength", l_weather_pollen_strength},
    {"pollen_color", l_weather_pollen_color},
    {"pollen_drifting", l_weather_pollen_drifting},
    {"set_pollen", l_weather_set_pollen},
    {"dust_enabled", l_weather_dust_enabled},
    {"dust_wind_start", l_weather_dust_wind_start},
    {"dust_wind_full", l_weather_dust_wind_full},
    {"dust_manual", l_weather_dust_manual},
    {"dust_rate", l_weather_dust_rate},
    {"dust_box_xz_m", l_weather_dust_box_xz_m},
    {"dust_height_m", l_weather_dust_height_m},
    {"dust_size_m", l_weather_dust_size_m},
    {"dust_lifetime_s", l_weather_dust_lifetime_s},
    {"dust_speed", l_weather_dust_speed},
    {"dust_wind_follow", l_weather_dust_wind_follow},
    {"dust_max_particles", l_weather_dust_max_particles},
    {"dust_seed", l_weather_dust_seed},
    {"dust_strength", l_weather_dust_strength},
    {"dust_color", l_weather_dust_color},
    {"dust_blowing", l_weather_dust_blowing},
    {"set_dust", l_weather_set_dust},
    {nullptr, nullptr},
};

}  // namespace

void register_weather_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kWeatherLib);
    lua_setglobal(L, "weather");
    lua_newtable(L);
    luaL_register(L, nullptr, kWaterLib);
    lua_setglobal(L, "water");
}

}  // namespace mce::script::detail
