-- Ember Step in Luau: the goal flag. The player reaching it ends the level.
--   v[16] kind: 1 = player, 3 = goal
local VARS = mce.field("ScriptVars", "v")
local POS = mce.field("LocalTransform", "position")

function update()
    local player
    mce.each("ScriptVars", function(e)
        if mce.get(e, VARS)[16] == 1 then player = mce.get(e, POS) end
    end)
    if not player then return end
    mce.each("ScriptVars", function(e)
        if mce.get(e, VARS)[16] ~= 3 then return end
        local g = mce.get(e, POS)
        if math.abs(g.x - player.x) < 1.0 and math.abs(g.y - player.y) < 1.5 then
            mce.log("level complete")
            mce.destroy(e)
        end
    end)
end
