-- 10_missions: starts mission 1 and chains the next one when the one before it completes.
-- Scripts keep nothing between ticks, so what it has seen lives in ScriptVars on this script's entity:
--   v[1], v[2], v[3] = the last state of mission 1, 2, 3 (0 idle, 1 active, 2 complete, 3 failed)
-- The log lines ("mission 2 complete at tick 98") are what tests/sample_missions pins.
local VARS = mce.field("ScriptVars", "v")
local CODE = { idle = 0, active = 1, complete = 2, failed = 3 }
local NAME = { [1] = "active", [2] = "complete", [3] = "failed" }

function update()
    local m1, m2, m3 = mce.find("Mission 1"), mce.find("Mission 2"), mce.find("Mission 3")
    local home = mce.find("Start Pad")
    local s1, s2, s3 = (mission.status(m1)), (mission.status(m2)), (mission.status(m3))

    if mce.tick() == 10 then mission.start(m1) end             -- a short briefing, then go
    if s1 == "complete" and s2 == "idle" then mission.start(m2) end
    if s2 == "complete" and s3 == "idle" then mission.start(m3) end

    -- The start pad is on the map from the briefing until the delivery is made or lost.
    if s1 ~= "idle" and s3 ~= "complete" and s3 ~= "failed" then
        if not mce.has(home, "MapMarker") then marker.add(home, { icon = 1, color = { 0.4, 0.8, 1 }, label = "Start" }) end
    elseif mce.has(home, "MapMarker") then
        marker.remove(home)
    end

    mce.each("ScriptVars", function(e)
        local v = mce.get(e, VARS)
        local seen = { CODE[s1], CODE[s2], CODE[s3] }
        for i = 1, 3 do
            if seen[i] ~= v[i] then
                v[i] = seen[i]
                if seen[i] > 0 then mce.log("mission " .. i .. " " .. NAME[seen[i]] .. " at tick " .. mce.tick()) end
            end
        end
        mce.set(e, VARS, v)
    end)
end
