-- 12_weather: a fixed forecast, then a log line every 150 ticks. weather.set only writes Weather.target; the kit
-- does the rest (sky, fog, light, rain or snow, lightning). Scripts keep nothing between ticks: the weather is the
-- Weather component. The tick counts from 0. The log lines ("weather: rain intensity 0.900 ... at tick 600") are what
-- samples/12_weather/weather_test.cmake pins (so are the "sway:" lines, the foliage wind scale per weather kind, and the "snow:" lines,
-- the SnowCover's cover: it builds while the weather is snow and melts afterwards).
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
    end
end
