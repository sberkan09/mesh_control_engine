-- 12_loot: M-INVENTORY's demo. The hero gets items, equips the sword, drinks a potion, opens the chest (the table rolls
-- into the chest on the first open, the hero takes what fits), the goblin dies (a scripted Died3D; a real game's
-- Weapon3D does the same) and drops its table as pickups, and the hero drops two potions. The script keeps nothing:
-- everything is the Inventory, Equipment, Container and LootDrop components. Events land a few ticks after the call, so
-- each step waits for the previous one. The "inv:" lines are what samples/12_loot/loot_test.cmake pins.
local function report(label, hero)
    mce.log(string.format("inv: %s coin %d potion %d sword %d gem %d weight %d", label, inventory.count(hero, "coin"),
        inventory.count(hero, "potion"), inventory.count(hero, "sword"), inventory.count(hero, "gem"), inventory.weight(hero)))
end

function update()
    local t = mce.tick()
    local hero, chest, goblin = mce.find("Hero"), mce.find("Chest"), mce.find("Goblin")
    if t == 1 then
        inventory.add(hero, "potion", 7)
        inventory.add(hero, "sword")
        inventory.add(hero, "coin", 30)
    elseif t == 6 then
        report("given", hero)
        local id, qty = inventory.slot(hero, 1)
        mce.log(string.format("inv: slot 1 is %s x%d", id, qty))
        inventory.equip(hero, 3, "weapon")
        inventory.use(hero, 1)
    elseif t == 12 then
        report("equipped and drank", hero)
        inventory.open(chest, hero)
    elseif t == 18 then
        report("opened chest", hero)
        mce.emit("Died3D", { entity = goblin })
    elseif t == 24 then
        local pickups = 0
        mce.each("ItemPickup3D", function(e) pickups = pickups + 1 end)
        mce.log(string.format("inv: goblin dropped %d pickups", pickups))
        mce.log(string.format("inv: dropped %d potions", inventory.drop(hero, 1, 2)))
        report("dropped", hero)
    end
end
