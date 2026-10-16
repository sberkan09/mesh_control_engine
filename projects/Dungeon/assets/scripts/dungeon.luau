-- Dungeon in Luau: a top-down crawl over levels the engine generates from a seed (ProcLevel, user guide 28).
-- Walk to the Exit marker; the next seed builds the next level. Clear LEVELS of them and the next scene
-- (another generator) loads. Press Jump (Space) to hand the walk to the autopilot, which follows the level's
-- Path markers; press it again to take over.
-- A script keeps nothing between ticks, so what it remembers sits in the player's ScriptVars:
--   v[1] seed of the level the player stands in   v[2] levels cleared in this scene
--   v[3] autopilot (1 = on)   v[4] next Path marker   v[16] kind (1 = player)
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")
local SEED = mce.field("ProcLevel", "seed")
local KIND = mce.field("ProcLevel", "kind")

local SPEED = 7.0     -- m/s
local LEVELS = 10     -- levels per scene
local REACH = 0.45    -- metres from the Exit that count as arriving
local NEXT = { Rooms = "scenes/caves.mcscene", Caves = "scenes/walk.mcscene", Walk = "scenes/main.mcscene" }

local function length(d) return math.sqrt(d.x * d.x + d.y * d.y) end

local function marker(kind, index)
    local found = nil
    mce.each("ProcMarker", function(m)
        if mce.get(m, "ProcMarker", "kind") == kind and (index == nil or mce.get(m, "ProcMarker", "index") == index) then
            found = m
        end
    end)
    return found
end

function update()
    local level = nil
    mce.each("ProcLevel", function(e) level = e end)
    if not level then return end
    local seed, kind = mce.get(level, SEED, KIND)

    mce.each("ScriptVars", function(p)
        local v = mce.get(p, VARS)
        if v[16] ~= 1 then return end
        local pos = mce.get(p, POS)

        -- A new level has been built (the engine builds it a tick after the seed changes): stand on its Start.
        if v[1] ~= seed then
            local start = marker("Start")
            if not start then return end
            mce.teleport2d(p, mce.get(start, POS))
            v[1] = seed
            v[4] = 1
            mce.set(p, VARS, v)
            mce.log("level ready seed " .. seed)
            return
        end

        if mce.pressed("Jump") then v[3] = 1 - v[3] end

        local dir = mce.axis2("Move")
        local dx, dy = dir.x, dir.y
        if v[3] == 1 then
            local target = marker("Path", v[4])
            if target then
                local d = mce.get(target, POS) - pos
                local n = length(d)
                if n < 0.12 then
                    v[4] = v[4] + 1
                else
                    dx, dy = d.x / n, d.y / n
                end
            end
        end
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 1 then dx, dy = dx / len, dy / len end
        mce.move_and_collide(p, vector.create(dx * SPEED * mce.dt, dy * SPEED * mce.dt, 0))
        mce.set(p, VARS, v)

        -- Arrived?
        local exit = marker("Exit")
        if exit and length(mce.get(exit, POS) - pos) < REACH then
            mce.log("level complete seed " .. seed)
            v[2] = v[2] + 1
            mce.set(p, VARS, v)
            if v[2] >= LEVELS then
                mce.load_scene(NEXT[kind])
            else
                mce.set(level, "ProcLevel", "reroll", true)
            end
        end
    end)

    -- The camera keeps the player in view.
    local focus = nil
    mce.each("ScriptVars", function(p)
        if mce.get(p, VARS)[16] == 1 then focus = mce.get(p, POS) end
    end)
    if focus then
        mce.each("Camera2DComponent", function(c) mce.set(c, POS, focus) end)
    end
end
