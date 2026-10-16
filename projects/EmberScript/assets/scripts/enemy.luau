-- Ember Step in Luau: the walker enemy, a three-state machine (Patrol, Chase, Return) and the
-- touch rules (stomp it from above, or it sends the player back to the start).
--   v[1] state (0 patrol, 1 chase, 2 return)   v[2] direction (-1 / 1)   v[3] home x
--   v[4] patrol half-width   v[5] sight range   v[16] kind (2 = enemy, 1 = player, 3 = goal)
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")

local PATROL, CHASE, RETURN = 0, 1, 2
local PATROL_SPEED = 1.6
local CHASE_SPEED = 3.4
local GRAVITY = -30.0

local function player_position()
    local found
    mce.each("ScriptVars", function(e)
        local v = mce.get(e, VARS)
        if v[16] == 1 then found = {e = e, p = mce.get(e, POS), vy = v[2]} end
    end)
    return found
end

function update()
    local player = player_position()
    mce.each("ScriptVars", function(e)
        local v = mce.get(e, VARS)
        if v[16] ~= 2 then return end
        local p = mce.get(e, POS)
        local state, dir, home, half, sight = v[1], v[2], v[3], v[4], v[5]

        local dx = player and (player.p.x - p.x) or 1e9
        local dy = player and (player.p.y - p.y) or 1e9
        local sees = math.abs(dx) < sight and math.abs(dy) < 1.5

        local speed = PATROL_SPEED
        if state == PATROL then
            if sees then state = CHASE end
            if p.x > home + half then dir = -1 elseif p.x < home - half then dir = 1 end
        elseif state == CHASE then
            speed = CHASE_SPEED
            dir = dx >= 0 and 1 or -1
            if not sees then state = RETURN end
        else
            dir = home >= p.x and 1 or -1
            if sees then state = CHASE
            elseif math.abs(p.x - home) < 0.2 then state = PATROL end
        end

        local r = mce.move_and_collide(e, vector.create(dir * speed * mce.dt, GRAVITY * mce.dt * mce.dt, 0))
        if r.wall_left or r.wall_right then dir = -dir end

        -- Touch: from above it is stomped, otherwise the player goes back to the start.
        if player and math.abs(dx) < 0.75 and math.abs(dy) < 0.9 then
            if player.vy < 0 and dy > 0.3 then
                mce.destroy(e)
                mce.log("enemy stomped")
                return
            end
            mce.set(player.e, POS, vector.create(2, 0.6, 0))
            local pv = mce.get(player.e, VARS)
            pv[1], pv[2], pv[3], pv[4] = 0, 0, 0, 0
            mce.set(player.e, VARS, pv)
            mce.log("player hurt")
        end

        v[1], v[2] = state, dir
        mce.set(e, VARS, v)
    end)
end
