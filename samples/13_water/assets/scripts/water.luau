-- 13_water: a crate floats on the Water: every tick its y follows water.height_at(x, z) (a little above, so the box
-- rides the surface) and it tilts to water.normal_at. Scripts keep nothing between ticks: the water is the Water
-- component and the surface is a pure function of the scene time (tick * dt). Every 150 ticks it logs the height, the
-- crate and two submerged tests (a point 0.3 m under the surface at the crate, and one 0.3 m over it; submerged is about
-- the waves only, the island is geometry the water does not know). The
-- log lines ("water: height ... at tick 150") are what samples/13_water/water_test.cmake pins.
-- M-FREEZE: the Water entity also carries a WaterIce, a Season (a 30-day year) and a WorldClock whose 2 s day makes the year
-- 60 s (drive_environment 0, so the clock never moves the fixed sun); the sea freezes in winter and thaws in spring. The
-- "ice:" line logs weather.water_frozen(), the freeze amount the renderer draws, every 150 ticks. The waves the crate rides
-- are the authored ones: the freeze changes only how the water is drawn.
-- M-FLOAT: the Buoy and the Raft carry a Floater, so kit_floater (not this script) puts them on the surface and leans them
-- with it. The "floater:" line logs each one's y, the surface under it and its Floater offset (floater.offset), which
-- the headless gate checks: y is the surface + offset.
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
        for _, f in ipairs({ { "Buoy", -6, 2 }, { "Raft", -2, 10 } }) do
            local e = mce.find(f[1])
            local p = mce.get(e, "LocalTransform", "position")
            mce.log(string.format("floater: %s y %.3f surface %.3f offset %.3f at tick %d", f[1], p.y,
                water.height_at(f[2], f[3]), floater.offset(e), t))
        end
        mce.log(string.format("ice: frozen %.3f phase %.3f at tick %d", weather.water_frozen(), weather.season_phase(), t))
    end
end
