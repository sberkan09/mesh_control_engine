// M-INVENTORY 4: the `inventory` Luau global over the kit's Inventory, Equipment and Container. Exact numbers: a script
// gives, counts, equips, removes and drops at pinned ticks; a chest opens; bad arguments raise a Luau error that names
// the call and disables only that script.

#include <doctest/doctest.h>

#include <string>

#include "mce/ecs/transform.h"
#include "mce/kit/craft.h"
#include "mce/kit/inventory.h"
#include "mce/kit/loot.h"
#include "mce/script/compiler.h"

using namespace mce;
using script::ScriptHost;
using script::ScriptState;

namespace {

const asset::Guid kCat{5, 6};

asset::ItemCatalog make_catalog() {
    asset::ItemCatalog c;
    auto item = [&](const char* id, u32 stack, u32 weight, std::initializer_list<const char*> tags) {
        asset::ItemDef d;
        d.id = id;
        d.name = id;
        d.stack_max = stack;
        d.weight_milli = weight;
        Array<FixedString<32>> t;
        for (const char* s : tags) t.push_back(FixedString<32>(s));
        REQUIRE(c.add_item(d, {}, Span<const FixedString<32>>(t.data(), t.size())));
    };
    item("coin", 100, 0, {});
    item("potion", 5, 500, {"consumable"});
    item("sword", 1, 3000, {"equip.weapon"});
    asset::LootTable t;
    t.id = "chest";
    t.rolls_min = 1;
    t.rolls_max = 1;
    asset::LootEntry e;
    e.ref = 0;
    e.weight = 1;
    e.qty_min = e.qty_max = 9;
    REQUIRE(c.add_table(t, Span<const asset::LootEntry>(&e, 1)));
    auto recipe = [&](const char* id, const char* station, u32 ms, asset::RecipeIngredient in, asset::RecipeIngredient out) {
        asset::Recipe r;
        r.id = id;
        r.name = id;
        r.station = station;
        r.time_ms = ms;
        REQUIRE(c.add_recipe(r, Span<const asset::RecipeIngredient>(&in, 1), Span<const asset::RecipeIngredient>(&out, 1)));
    };
    recipe("brew", "", 0, {0, 2}, {1, 1});         // 2 coin -> 1 potion, by hand
    recipe("forge", "anvil", 200, {0, 3}, {2, 1});  // 3 coin -> 1 sword at an anvil, 0.2 s
    return c;
}

struct IRig {
    World w;
    ScriptHost host;
    asset::ItemCatalog cat = make_catalog();
    Entity hero, box, anvil;
    u64 tick = 0;

    explicit IRig(const char* source, bool install_kit = true) {
        w.register_components<LocalTransform>();
        if (install_kit) {
            kit::install_crafting(w, nullptr);
            w.resource<kit::ItemRegistry>().define(kCat, cat);
        } else {
            w.register_components<kit::Inventory>();
        }
        INFO(source);
        Result<Array<u8>> bc = script::compile_unchecked("i", source);
        REQUIRE(bc);
        REQUIRE(host.add_bytecode("i", bc.value().span()));
        host.install(w);
        kit::Inventory inv;
        inv.catalog.guid = kCat;
        kit::Equipment eq;
        eq.catalog.guid = kCat;
        hero = install_kit ? w.spawn_with(inv, eq, LocalTransform{}) : w.spawn_with(inv);
        if (install_kit) {
            kit::Container c;
            c.catalog.guid = kCat;
            c.table = "chest";
            c.seed = 3;
            box = w.spawn_with(inv, c);
            kit::CraftingStation st;
            st.station = "anvil";
            anvil = w.spawn_with(st);
        }
    }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(1.0 / 60, ++tick);
    }
    kit::Inventory& inv(Entity e) { return *w.try_get_mut<kit::Inventory>(e); }
    u32 count(const char* id) { return kit::inventory_count(inv(hero), cat.find_item(id)); }
    std::string err() const { return host.scripts()[0].error.c_str(); }
};

void expect_error(const char* body, const char* needle, bool install_kit = true) {
    const std::string src = std::string("function update()\n    mce.each('Inventory', function(e)\n        ") + body + "\n    end)\nend\n";
    IRig r(src.c_str(), install_kit);
    r.step(2);
    INFO(body);
    REQUIRE(r.host.scripts()[0].state == ScriptState::Failed);
    INFO(r.err());
    CHECK(r.err().find(needle) != std::string::npos);
}

}  // namespace

TEST_CASE("inventory api: give, read, equip, remove and drop at pinned ticks") {
    IRig r(R"(
function update()
    local t = mce.tick()
    mce.each("Equipment", function(e)
        if t == 1 then
            inventory.add(e, "potion", 7)
            inventory.add(e, "sword")
        elseif t == 6 then
            assert(inventory.count(e, "potion") == 7)
            assert(inventory.has(e, "potion", 7) and not inventory.has(e, "potion", 8))
            assert(inventory.count(e, "nothing") == 0)
            local id, qty = inventory.slot(e, 1)
            assert(id == "potion" and qty == 5)
            local id3, qty3 = inventory.slot(e, 3)
            assert(id3 == "sword" and qty3 == 1)
            assert(inventory.slot(e, 9) == nil)
            assert(inventory.weight(e) == 7 * 500 + 3000)
            inventory.equip(e, 3, "weapon")
            inventory.remove(e, "potion", 2)
        elseif t == 12 then
            assert(inventory.count(e, "potion") == 5)
            assert(inventory.count(e, "sword") == 0) -- equipped: out of the slots
            assert(inventory.drop(e, 1, 2) == 2)
            assert(inventory.count(e, "potion") == 3)
            assert(inventory.drop(e, 20) == 0)
        end
    end)
end
)");
    r.step(14);
    INFO(r.err());
    REQUIRE(r.host.scripts()[0].state == ScriptState::Running);
    CHECK(r.w.try_get<kit::Equipment>(r.hero)->item[static_cast<u32>(kit::EquipSlot::Weapon)] == r.cat.find_item("sword") + 1u);
    u32 pickups = 0, qty = 0;
    r.w.query<const kit::ItemPickup3D, const LocalTransform>().each([&](Entity, const kit::ItemPickup3D& p, const LocalTransform& t) {
        ++pickups;
        qty += p.qty;
        CHECK(t.position.x == doctest::Approx(1.5f));
    });
    CHECK(pickups == 1);
    CHECK(qty == 2);
}

TEST_CASE("inventory api: use takes a consumable, open moves a chest's loot") {
    IRig r(R"(
function update()
    local t = mce.tick()
    mce.each("Equipment", function(e)
        if t == 1 then inventory.add(e, "potion", 2)
        elseif t == 6 then
            inventory.use(e, 1)
            mce.each("Container", function(box) inventory.open(box, e) end)
        end
    end)
end
)");
    r.step(14);
    REQUIRE(r.host.scripts()[0].state == ScriptState::Running);
    CHECK(r.count("potion") == 1);
    CHECK(r.count("coin") == 9);
}

TEST_CASE("inventory api: bad arguments raise an error that names the call") {
    expect_error("inventory.add(e, '', 1)", "inventory.add: the item id is empty");
    expect_error("inventory.add(e, 'coin', 0)", "inventory.add: 0 is not a whole number from 1 to 1e+06");
    expect_error("inventory.remove(e, 'coin', 1.5)", "inventory.remove");
    expect_error("inventory.slot(e, 25)", "inventory.slot: 25 is not a whole number from 1 to 24");
    expect_error("inventory.equip(e, 1, 'elbow')", "inventory.equip: unknown equipment slot 'elbow'");
    expect_error("inventory.use(e, 0)", "inventory.use");
    expect_error("inventory.count(e, 'coin')", "this world has no inventories", false);
    expect_error("inventory.open(e, e)", "inventory.open: the first entity has no Container component");
}

TEST_CASE("inventory api: can_craft reads now, craft queues by hand and at a station") {
    IRig r(R"(
function update()
    local t = mce.tick()
    mce.each("Equipment", function(e)
        mce.each("CraftingStation", function(anvil)
            if t == 1 then
                inventory.add(e, "coin", 5)
            elseif t == 6 then
                mce.log(string.format("c: brew %s forge %s forge_at_anvil %s nope %s", tostring(inventory.can_craft(e, "brew")),
                    tostring(inventory.can_craft(e, "forge")), tostring(inventory.can_craft(e, "forge", anvil)), tostring(inventory.can_craft(e, "nope"))))
                inventory.craft(e, "brew")
            elseif t == 12 then
                mce.log(string.format("c: after brew potion %d coin %d forge_at_anvil %s", inventory.count(e, "potion"), inventory.count(e, "coin"),
                    tostring(inventory.can_craft(e, "forge", anvil))))
                inventory.craft(e, "forge", anvil)
            elseif t == 40 then
                mce.log(string.format("c: after forge sword %d coin %d", inventory.count(e, "sword"), inventory.count(e, "coin")))
            end
        end)
    end)
end
)");
    r.step(45);
    REQUIRE(r.host.scripts()[0].state == ScriptState::Running);
    CHECK(r.count("potion") == 1);
    CHECK(r.count("sword") == 1);
    CHECK(r.count("coin") == 0);
}

TEST_CASE("inventory api: craft argument errors name the call") {
    expect_error("inventory.craft(e, '')", "inventory.craft: the item id is empty");
    expect_error("inventory.craft(e, 'forge', e)", "inventory.craft: the station entity has no CraftingStation component");
    expect_error("inventory.can_craft(e, 'brew', e)", "inventory.can_craft: the station entity has no CraftingStation component");
    expect_error("inventory.can_craft(e, 'brew')", "this world has no inventories", false);
}
