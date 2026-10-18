-- 12_weather: a fixed forecast, then a log line every 150 ticks. weather.set only writes Weather.target; the kit
-- does the rest (sky, fog, light, rain or snow, lightning). Scripts keep nothing between ticks: the weather is the
-- Weather component. The tick counts from 0. The log lines ("weather: rain intensity 0.900 ... at tick 600") are what
-- samples/12_weather/weather_test.cmake pins.
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
    end
end
