-- Pursuit in Luau: collect the five coins before the grunt catches you. A wall with a four metre gap splits the
-- arena, so everything that walks has to plan around it: the grunt chases the hero, and Space hands the hero
-- to an autopilot that walks to the nearest coin. Both plan on the baked navmesh with mce.find_path3d and walk with
-- the kit's CharacterController3D (a script sets its `move`, the kit's own system does the slopes and steps).
-- A script keeps nothing between ticks, so each entity's memory sits in its ScriptVars:
--   hero   v[1] coins   v[2] 1 once cleared   v[3] autopilot (1 = on)   v[4] times caught   v[5],v[6] waypoint x,z
--          v[7] ticks to the next plan
--   grunt  v[5],v[6] waypoint x,z   v[7] ticks to the next plan   v[8],v[9] where it stands guard (its start)
--   v[16]  kind: 1 hero, 2 grunt, 3 coin
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")

local COINS = 5
local HERO_SPEED = 5.0   -- m/s
local GRUNT_SPEED = 3.0
local REPLAN = 12        -- ticks between plans
local CAUGHT = 1.0       -- metres
local CORNER = 0.3       -- metres: nearer than this a corner counts as reached
local PICKUP = 0.9       -- metres: the hero's reach

local function flat_length(x, z) return math.sqrt(x * x + z * z) end

local function navmesh_ready()
    local ready = false
    mce.each("NavMeshSurface3D", function(s) ready = mce.get(s, "NavMeshSurface3D", "ready") == 1 end)
    return ready
end

-- Plans a walk from `pos` to `target` every REPLAN ticks (v[7] counts them down) and sets the character's `move`
-- towards the first corner of the plan. v[5], v[6] hold that corner between plans.
local function walk_to(e, v, pos, target, speed)
    v[7] = v[7] - 1
    if v[7] <= 0 then
        v[7] = REPLAN
        local path = mce.find_path3d(pos, target)   -- nil when the target is off the mesh
        if path and #path > 0 then
            -- The first corner that is not underfoot (the plan lists the corners after `pos`, but one can lie right on it).
            local corner = path[#path]
            for i = 1, #path do
                if flat_length(path[i].x - pos.x, path[i].z - pos.z) > CORNER then
                    corner = path[i]
                    break
                end
            end
            v[5], v[6] = corner.x, corner.z
        end
    end
    local dx, dz = v[5] - pos.x, v[6] - pos.z
    local n = flat_length(dx, dz)
    local move = vector.create(0, 0, 0)
    if n > 0.1 then move = vector.create(dx / n * speed, 0, dz / n * speed) end
    mce.set(e, "CharacterController3D", "move", move)
end

local function hero_tick(p, v, pos)
    if mce.pressed("Jump") then v[3] = 1 - v[3] end
    mce.set(p, "CharacterController3D", "use_input", v[3] == 0)   -- the autopilot walks instead of the keys

    if v[3] == 1 and v[2] == 0 then
        -- The nearest coin by straight distance, then the plan to it.
        local best, best_d = nil, 1e9
        mce.each("ScriptVars", function(c)
            if mce.get(c, VARS)[16] ~= 3 then return end
            local d = pos - mce.get(c, POS)
            local len = flat_length(d.x, d.z)
            if len < best_d then best, best_d = mce.get(c, POS), len end
        end)
        if best then
            walk_to(p, v, pos, best, HERO_SPEED)
        end
    elseif v[3] == 1 then
        mce.set(p, "CharacterController3D", "move", vector.create(0, 0, 0))
    end

    -- Coins within reach are collected.
    for _, thing in ipairs(mce.overlap_sphere3d(pos, PICKUP, p)) do
        if mce.has(thing, "ScriptVars") and mce.get(thing, VARS)[16] == 3 then
            mce.destroy(thing)
            v[1] = v[1] + 1
            mce.shake(0.15)
            mce.log("coin " .. v[1])
        end
    end
    if v[1] >= COINS and v[2] == 0 then
        v[2] = 1
        mce.log("arena cleared, caught " .. v[4] .. " times")
    end
end

local function grunt_tick(g, v, pos, hero_pos)
    if v[2] == 0 then v[8], v[9], v[2] = pos.x, pos.z, 1 end   -- remember where it started
    walk_to(g, v, pos, hero_pos, GRUNT_SPEED)
    return flat_length(pos.x - hero_pos.x, pos.z - hero_pos.z) < CAUGHT
end

function update()
    if not navmesh_ready() then return end

    local hero, hero_pos = nil, nil
    mce.each("ScriptVars", function(p)
        if mce.get(p, VARS)[16] == 1 then hero, hero_pos = p, mce.get(p, POS) end
    end)
    if not hero then return end

    local hv = mce.get(hero, VARS)
    hero_tick(hero, hv, hero_pos)

    mce.each("ScriptVars", function(g)
        local v = mce.get(g, VARS)
        if v[16] ~= 2 then return end
        if grunt_tick(g, v, mce.get(g, POS), hero_pos) then
            hv[4] = hv[4] + 1
            mce.shake(0.5)
            mce.log("caught " .. hv[4])
            mce.teleport3d(g, vector.create(v[8], 0.9, v[9]))   -- back to its post; being caught is not lethal
            v[7] = 0
        end
        mce.set(g, VARS, v)
    end)
    mce.set(hero, VARS, hv)
end
