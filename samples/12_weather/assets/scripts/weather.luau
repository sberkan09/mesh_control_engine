-- 12_weather: a fixed forecast, then a log line every 150 ticks. weather.set only writes Weather.target; the kit
-- does the rest (sky, fog, light, rain or snow, lightning). Scripts keep nothing between ticks: the weather is the
-- Weather component. The tick counts from 0. The log lines ("weather: rain intensity 0.900 ... at tick 600") are what
-- samples/12_weather/weather_test.cmake pins (so are the "sway:" lines, the foliage wind scale per weather kind).
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
    end
end
