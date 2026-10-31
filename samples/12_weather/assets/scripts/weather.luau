-- 12_weather: a fixed forecast, then a log line every 150 ticks. weather.set only writes Weather.target; the kit
-- does the rest (sky, fog, light, rain or snow, lightning). Scripts keep nothing between ticks: the weather is the
-- Weather component. The tick counts from 0. The log lines ("weather: rain intensity 0.900 ... at tick 600") are what
-- samples/12_weather/weather_test.cmake pins (so are the "sway:" lines, the foliage wind scale per weather kind, and the "snow:" lines,
-- the SnowCover's cover: it builds while the weather is snow and melts afterwards, the "mist:" lines, the GroundMist level, the "moon:" lines, the Moon's phase and level, the "aurora:" lines, the Aurora's latitude, gate and level, the "rainbow:" lines, the Rainbow's level, the "meteor:" lines, the Meteor's level, and the "season:" lines, the Season's phase and tint).
function update()
    local t = mce.tick()
    if t == 60 then
        weather.set("rain")
    elseif t == 600 then
        weather.set("storm")
    elseif t == 1500 then
        weather.set("snow")
    elseif t == 2100 then
        weather.set("clear")
    end
    if t % 150 == 0 then
        mce.log(string.format("weather: %s intensity %.3f wind %.3f wetness %.3f at tick %d",
            weather.kind(), weather.intensity(), weather.wind(), weather.wetness(), t))
        -- M-WINDSWAY: Kit3DExtract scales the foliage wind amplitude by 1 + (gain - 1) * wind; this is that factor.
        mce.log(string.format("sway: amplitude x%.3f frequency x%.3f at tick %d",
            1 + (weather.sway_amplitude() - 1) * weather.wind(),
            1 + (weather.sway_frequency() - 1) * weather.wind(), t))
        -- M-SNOWCOVER: kit_snow_cover writes the cover; the renderer whitens up-facing surfaces by cover * strength.
        mce.log(string.format("snow: cover %.3f at tick %d", weather.snow_cover(), t))
        -- M-RAINFX: the rain effects' authoring fields (ripples on the puddles, splashes where drops land; the
        -- ThunderDelay has no clip in this sample, so its strikes make no sound).
        mce.log(string.format("rain fx: ripples %.2f splash %.2f thunder %.0f at tick %d",
            weather.ripples_strength(), weather.splash_rate(), weather.thunder_speed(), t))
        -- M-WINDDIR: the scene wind turns 6 degrees a minute from 0 (+x); the foliage sway and the snow lean follow it.
        mce.log(string.format("wind dir: heading %.2f drift %.1f at tick %d",
            weather.wind_heading(), weather.wind_drift(), t))
        -- M-CLOUDS: the cloud layer's authoring fields; the renderer derives the cover from the weather kind.
        mce.log(string.format("clouds: strength %.2f fair %.2f overcast %.2f height %.0f at tick %d",
            weather.cloud_strength(), weather.cloud_fair_coverage(), weather.cloud_overcast_coverage(),
            weather.cloud_height(), t))
        -- M-WEATHERAUDIO: the rain and wind bed levels follow the weather (the two sources have no clip: silent).
        mce.log(string.format("audio: rain %.3f wind %.3f max %.0f/%.0f dB fade %.0f s at tick %d",
            weather.rain_level(), weather.wind_level(), weather.audio_rain_db(), weather.audio_wind_db(),
            weather.audio_fade(), t))
        -- M-LIGHTNINGBOLT: the last strike's ground position and distance from the camera (the bolt is drawn for the
        -- flash ticks; the light is off). Before the first strike they are 0.
        local bx, bz = weather.bolt_pos()
        mce.log(string.format("bolt: strike at %.0f, %.0f dist %.0f m range %.0f-%.0f m height %.0f m at tick %d",
            bx, bz, weather.bolt_distance(), weather.bolt_min_range(), weather.bolt_max_range(),
            weather.bolt_height(), t))
        -- M-WINDGUST: the seeded gust level (0 in the calm gaps) and the gusted wind that sway, rain and the wind bed read
        -- (Weather.wind itself stays smooth).
        mce.log(string.format("gust: level %.3f wind %.3f effective %.3f strength %.2f at tick %d",
            weather.gust_level(), weather.wind(), weather.effective_wind(), weather.gust_strength(), t))
        -- M-GROUNDMIST: the clock starts at 5:00 (a 600 s day: 1 h is 1500 ticks) so the mist is building toward its
        -- 6:00 peak; the level is the hour and the weather's wetness through kit mist_level (0 without a clock).
        mce.log(string.format("mist: level %.3f hour %.2f strength %.2f peak %.1f h at tick %d",
            weather.mist_level(), clock.hour(), weather.mist_strength(), weather.mist_peak_hour(), t))
        -- M-NIGHTSKY: the Moon on the Weather entity. phase and illumination follow the clock (day 0, hour 5 to 6.6: a
        -- full moon, 0.5 is the default offset); the level is that light times the moon's height and the weather's damping.
        local mx, my, mz = weather.moon_direction()
        mce.log(string.format("moon: phase %.3f illumination %.3f level %.3f dir %.2f %.2f %.2f at tick %d",
            weather.moon_phase(), weather.moon_illumination(), weather.moon_level(), mx, my, mz, t))
        -- M-AURORA: the Aurora on the Weather entity. The scene has no globe, so the latitude is authored: this walks it from
        -- 45 degrees (gate closed) up by 1 degree per 45 ticks (ticks 450 to 900 cross the gate, 55 to 65), so the level
        -- rises with the gate while the night fades (the clock starts at 5:00, dawn at about 6:00) and the weather's cloud
        -- cover damps it. aurora_level is the kit helper on the night fade of the hour and the cloud coverage.
        weather.set_aurora{latitude = 45 + t / 45}
        mce.log(string.format("aurora: latitude %.2f gate %.3f level %.3f at tick %d",
            weather.aurora_latitude(), weather.aurora_gate(), weather.aurora_level(), t))
        -- M-RAINBOW: the Rainbow on the Weather entity. rainbow_level is the kit helper on the sun elevation of the hour, the
        -- ground wetness, the rain intensity and the cloud cover: 0 by night (the clock starts at 5:00, sunrise at about
        -- 6:00 = tick 1500), 0 while it rains or storms, and it rises once the rain is gone and the sun is up while the
        -- ground is still wet (the snow gives way to clear at tick 2100); the bow follows the drying of the ground.
        mce.log(string.format("rainbow: level %.3f wetness %.3f at tick %d",
            weather.rainbow_level(), weather.wetness(), t))
        -- M-METEOR: the Meteor on the Weather entity. meteor_level is the kit helper on the night fade of the hour, the cloud
        -- cover and the rain intensity: non-zero through the dark hours (the clock starts at 5:00, sunrise at about tick 1500), damped by the
        -- cloud cover and the rain and the storm, and exactly 0 by day (night_only is on); read only, the script writes nothing.
        mce.log(string.format("meteor: level %.3f at tick %d", weather.meteor_level(), t))
        -- M-SEASONS: the Season on the Weather entity. The WorldClock day is 600 s (36000 ticks), so the sample sets a one-day
        -- year (year_days 1, the shortest) to move the phase visibly: it starts at start_phase 0.1 plus the 5:00 start
        -- (5 / 24 of a year) and climbs by 1 / 36000 per tick, from late spring to the centre of summer (0.375) at tick 2400; the tint logged is the authored
        -- spring multiplier. Read only, the script writes nothing.
        local sr, sg, sb = weather.season_spring_tint()
        mce.log(string.format("season: phase %.3f spring tint %.2f %.2f %.2f at tick %d", weather.season_phase(), sr, sg, sb, t))
        -- M-LEAFFALL: the LeafFall on the Weather entity. The Season never reaches autumn in this run (the phase climbs from 0.308
        -- to 0.375), so the scene sets manual 0.6, the floor of the amount: leaf_falling is 0.600 on every line and the camera-
        -- following leaf emitter is drawn all run. Read only, the script writes nothing.
        mce.log(string.format("leaves: %.3f at tick %d", weather.leaf_falling(), t))
    end
end
