-- 14_seasons: one whole year in 60 s. The scene's WorldClock has a 2 s day and its Season a 30-day year, so the year is 3600
-- ticks at 60 ticks a second; the script only moves the camera and logs. The camera is a pure function of mce.tick(): a circle
-- of radius 16 m at 7 m height, one lap per year, looking at a point just above the lake (a yaw about +y composed with a fixed
-- downward pitch). Every 150 ticks it logs the season phase, the lake's freeze amount and the leaf-fall amount, read from the
-- authored Season, WaterIce and LeafFall through the clock: nothing is written but the camera, so every leaf and every bit of
-- ice comes from the season. samples/14_seasons/seasons_test.cmake pins the lines at the four season centres.
local R, H, LOOK_Y = 16, 7, 1
local PITCH = -math.atan2(H - LOOK_Y, R)
function update()
    local t = mce.tick()
    local a = (t % 3600) / 3600 * 2 * math.pi
    local sy, cy = math.sin(a / 2), math.cos(a / 2)
    local sx, cx = math.sin(PITCH / 2), math.cos(PITCH / 2)
    local cam = mce.find("Camera")
    mce.set(cam, "LocalTransform", "position", vector.create(R * math.sin(a), H, R * math.cos(a)))
    mce.set(cam, "LocalTransform", "rotation", cy * sx, sy * cx, -sy * sx, cy * cx)
    if t % 150 == 0 then
        mce.log(string.format("season: phase %.3f frozen %.3f leaves %.3f at tick %d",
            weather.season_phase(), weather.water_frozen(), weather.leaf_falling(), t))
    end
end
