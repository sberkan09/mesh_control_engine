-- The Hero swings left and right. A script keeps no state between ticks: where it is comes from
-- the tick, and anything it must remember lives in a component.
local POSITION = mce.field("LocalTransform", "position")

function update()
    local x = math.sin(mce.tick() * mce.dt) * 4
    mce.each("Sprite", function(e)
        mce.set(e, POSITION, vector.create(x, 0, 0))
    end)
    if mce.tick() % 60 == 0 then
        mce.log("hero at x=" .. string.format("%.2f", x))
    end
end
