-- 09_openworld: the car radio. Q (Radio) cycles station 1, 2, off while the player sits in a car; the chosen station is
-- unmuted, the other stays muted (both keep playing, so a station resumes mid-loop; Music streams two clips at a time).
-- The dial lives in ScriptVars.v[1] of "Script: radio" (a script keeps nothing between ticks) and is saved with it.
local DIAL = mce.field("ScriptVars", "v", 1)
local GAIN = mce.field("AudioSource", "gain_db")
local SEATED = mce.field("Rider3D", "seated")
local MUTE, ON = -60, -8

function update()
    local player = mce.find("Player")
    local script = mce.find("Script: radio")
    if not player or not script then return end
    local seated = mce.get(player, SEATED) == 1
    local station = mce.get(script, DIAL)
    if seated and mce.pressed("Radio") then
        station = (station + 1) % 3
        mce.set(script, DIAL, station)
    end
    for i = 1, 2 do
        local r = mce.find("Radio " .. i)
        if r then mce.set(r, GAIN, (seated and station == i) and ON or MUTE) end
    end
end
