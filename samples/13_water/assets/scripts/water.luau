-- 13_water: a crate floats on the Water: every tick its y follows water.height_at(x, z) (a little above, so the box
-- rides the surface) and it tilts to water.normal_at. Scripts keep nothing between ticks: the water is the Water
-- component and the surface is a pure function of the scene time (tick * dt). Every 150 ticks it logs the height, the
-- crate and two submerged tests (a point 0.3 m under the surface at the crate, and one 0.3 m over it; submerged is about
-- the waves only, the island is geometry the water does not know). The
-- log lines ("water: height ... at tick 150") are what samples/13_water/water_test.cmake pins.
local X, Z, LIFT = 8, 4, 0.1
function update()
    local crate = mce.find("Crate")
    local h = water.height_at(X, Z)
    local nx, ny, nz = water.normal_at(X, Z)
    -- the small-angle rotation taking +y to the normal: axis up x n = (nz, 0, -nx), half the angle
    local qx, qz = nz * 0.5, -nx * 0.5
    local len = math.sqrt(qx * qx + qz * qz + 1)
    mce.set(crate, "LocalTransform", "position", vector.create(X, h + LIFT, Z))
    mce.set(crate, "LocalTransform", "rotation", qx / len, 0, qz / len, 1 / len)
    local t = mce.tick()
    if t % 150 == 0 then
        mce.log(string.format("water: height %.3f crate %.3f tilt %.3f submerged %s above %s at tick %d",
            h, h + LIFT, ny, tostring(water.submerged(X, h - 0.3, Z)), tostring(water.submerged(X, h + 0.3, Z)), t))
    end
end
