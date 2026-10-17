-- Dungeon in Luau: a top-down crawl over levels the engine generates from a seed (ProcLevel, user guide 28).
-- Walk to the Exit marker; the next seed builds the next level. Clear LEVELS of them and the next scene
-- (another generator) loads. Press Jump (Space) to hand the walk to the autopilot, which follows the level's
-- Path markers; press it again to take over.
-- A script keeps nothing between ticks, so what it remembers sits in the player's ScriptVars:
--   v[1] seed of the level the player stands in   v[2] levels cleared in this scene
--   v[3] autopilot (1 = on)   v[4] next Path marker   v[5] times caught   v[16] kind (1 = player, 2 = chaser)
-- Chasers (two per level) have no body: they plan a walk to the player around the walls with mce.find_path every
-- REPLAN ticks and follow it at CHASE_SPEED. Being caught is not lethal: the chaser goes back to where it started.
-- A chaser keeps v[1],v[2] the waypoint it is walking to, v[3] ticks to the next plan, v[4],v[5] where it started,
-- v[6] whether it has planned yet.
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")
local SEED = mce.field("ProcLevel", "seed")
local KIND = mce.field("ProcLevel", "kind")

local SPEED = 7.0     -- m/s
local LEVELS = 10     -- levels per scene
local REACH = 0.45    -- metres from the Exit that count as arriving
local CHASERS = 2     -- per level
local CHASE_SPEED = 3.5
local REPLAN = 20     -- ticks between plans
local CAUGHT = 0.5    -- metres
local CHASER_TEXTURE = "guid:f438fc22-8393-4c6b-887d-4f907e4c4153"
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

-- Two chasers, standing on Path markers a third and two thirds of the way along.
local function spawn_chasers()
    mce.each("ScriptVars", function(c)
        if mce.get(c, VARS)[16] == 2 then mce.destroy(c) end
    end)
    local last = 0
    mce.each("ProcMarker", function(m)
        if mce.get(m, "ProcMarker", "kind") == "Path" then last = math.max(last, mce.get(m, "ProcMarker", "index")) end
    end)
    if last < 3 then return end
    for i = 1, CHASERS do
        local at = marker("Path", math.max(2, math.floor(last * i / (CHASERS + 1))))
        if at then
            local p = mce.get(at, POS)
            local v = {}
            for k = 1, 16 do v[k] = 0 end
            v[1], v[2], v[4], v[5], v[16] = p.x, p.y, p.x, p.y, 2
            mce.log("chaser spawned")
            mce.spawn({
                LocalTransform = { position = p },
                Sprite = { texture = CHASER_TEXTURE, size = vector.create(0.5, 0.5, 0), tint = { 0.9, 0.2, 0.25, 1 }, layer = 1 },
                ScriptVars = { v = v },
            })
        end
    end
end

-- One chaser's tick: plan towards the player now and then, then walk the plan.
local function chase(c, player_pos)
    local v = mce.get(c, VARS)
    local pos = mce.get(c, POS)
    v[3] = v[3] - 1
    if v[3] <= 0 then
        v[3] = REPLAN
        local path = mce.find_path(pos, player_pos)
        if path and #path > 0 then
            local way = path[math.min(2, #path)]   -- the first corner after where it stands
            v[1], v[2] = way.x, way.y
            if v[6] == 0 then
                v[6] = 1
                mce.log("chaser planned " .. #path .. " corners")
            end
        end
    end
    local dx, dy = v[1] - pos.x, v[2] - pos.y
    local n = math.sqrt(dx * dx + dy * dy)
    if n > 0.05 then
        local step = math.min(CHASE_SPEED * mce.dt, n)
        mce.set(c, POS, vector.create(pos.x + dx / n * step, pos.y + dy / n * step, 0))
    end
    mce.set(c, VARS, v)
    return length(player_pos - pos) < CAUGHT
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
            spawn_chasers()
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

    -- The chasers (after the player has moved).
    local who, where = nil, nil
    mce.each("ScriptVars", function(p)
        if mce.get(p, VARS)[16] == 1 then who, where = p, mce.get(p, POS) end
    end)
    if who then
        mce.each("ScriptVars", function(c)
            if mce.get(c, VARS)[16] ~= 2 then return end
            if chase(c, where) then
                local pv = mce.get(who, VARS)
                pv[5] = pv[5] + 1
                mce.set(who, VARS, pv)
                local cv = mce.get(c, VARS)
                mce.set(c, POS, vector.create(cv[4], cv[5], 0))
                cv[3] = 0
                mce.set(c, VARS, cv)
                mce.log("caught " .. pv[5])
            end
        end)
    end

    -- The camera keeps the player in view.
    local focus = nil
    mce.each("ScriptVars", function(p)
        if mce.get(p, VARS)[16] == 1 then focus = mce.get(p, POS) end
    end)
    if focus then
        mce.each("Camera2DComponent", function(c) mce.set(c, POS, focus) end)
    end
end
