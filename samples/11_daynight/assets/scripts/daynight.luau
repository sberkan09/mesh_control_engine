-- 11_daynight: Jump (Space) skips to noon with clock.set(12); every 150 ticks (2.5 s, one game hour at the
-- 60 s day) it logs the clock. Scripts keep nothing between ticks: the clock is the WorldClock component.
-- The log lines ("hour 6.000 day 0 at tick 150") are what samples/11_daynight/daynight_test.cmake pins.
function update()
    if mce.pressed("Jump") then
        clock.set(12)
        mce.log(string.format("jump to noon at tick %d", mce.tick()))
    end
    if mce.tick() % 150 == 0 then
        mce.log(string.format("hour %.3f day %d at tick %d", clock.hour(), clock.day(), mce.tick()))
    end
end
