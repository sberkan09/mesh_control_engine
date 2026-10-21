-- Ember Step in Luau: the player controller. Run, jump with coyote time and a jump buffer, a
-- shorter hop when Jump is let go early, a respawn after a fall or an enemy's touch.
-- A script keeps nothing between ticks, so what it remembers sits in the entity's ScriptVars:
--   v[1] vx   v[2] vy   v[3] coyote ticks   v[4] jump buffer ticks   v[16] kind (1 = player)
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")

local RUN = 6.0          -- m/s
local ACCEL = 45.0       -- m/s^2 on the ground
local AIR_ACCEL = 28.0
local GRAVITY = -32.0
local JUMP = 11.5
local CUT = 0.45         -- vy is scaled by this when Jump is let go while rising
local COYOTE = 6         -- ticks after leaving a ledge that a jump still works
local BUFFER = 6         -- ticks a jump press is remembered before landing
local FALL_LIMIT = -9.0
local SPAWN = vector.create(2, 0.6, 0)

local function approach(v, target, step)
    if v < target then return math.min(v + step, target) end
    return math.max(v - step, target)
end

local function respawn(e, v)
    mce.teleport2d(e, SPAWN)
    v[1] = 0
    v[2] = 0
    v[3] = 0
    v[4] = 0
    mce.set(e, VARS, v)
    mce.log("player respawned")
end

function update()
    mce.each("ScriptVars", function(e)
        local v = mce.get(e, VARS)
        if v[16] ~= 1 then return end

        local move = mce.axis2("Move").x
        local vx, vy = v[1], v[2]
        local coyote, buffer = v[3], v[4]

        local accel = coyote > 0 and ACCEL or AIR_ACCEL
        vx = approach(vx, move * RUN, accel * mce.dt)

        if mce.pressed("Jump") then buffer = BUFFER end
        if buffer > 0 and coyote > 0 then
            vy = JUMP
            coyote = 0
            buffer = 0
        end
        if vy > 0 and not mce.held("Jump") then vy = vy * CUT end

        vy = vy + GRAVITY * mce.dt
        local r = mce.move_and_collide(e, vector.create(vx * mce.dt, vy * mce.dt, 0))
        if r.grounded then
            vy = math.min(vy, 0)
            coyote = COYOTE
        else
            coyote = math.max(coyote - 1, 0)
        end
        if r.ceiling and vy > 0 then vy = 0 end
        if (r.wall_left and vx < 0) or (r.wall_right and vx > 0) then vx = 0 end
        buffer = math.max(buffer - 1, 0)

        v[1], v[2], v[3], v[4] = vx, vy, coyote, buffer
        mce.set(e, VARS, v)
        if r.position.y < FALL_LIMIT then respawn(e, v) end
    end)
end
