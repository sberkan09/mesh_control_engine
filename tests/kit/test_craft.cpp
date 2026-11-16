// Crafting (M-CRAFT 2): craft_check / craft_apply atomicity (missing input, no room, the weight limit, freed input slots,
// nothing changes on failure), CraftItem by hand and at a station, timed crafts, busy stations and stations whose inputs vanished.

#include <doctest/doctest.h>

#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/craft.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kCat{5, 6};

// items: 0 cloth (stack 10, 100), 1 herb (stack 10, 50), 2 bandage (stack 5, 200), 3 plank (stack 4, 1000)
// recipes: 0 bandage (2 cloth + 1 herb -> 1 bandage, by hand), 1 shelf (4 plank -> 1 cloth 3 bandage, station "workbench", 1 s),
//          2 mend (1 bandage -> 1 herb, instant, workbench)
asset::ItemCatalog make_catalog() {
    asset::ItemCatalog c;
    auto item = [&](const char* id, u32 stack, u32 weight) {
        asset::ItemDef d;
        d.id = id;
        d.name = id;
        d.stack_max = stack;
        d.weight_milli = weight;
        REQUIRE(c.add_item(d));
    };
    item("cloth", 10, 100);
    item("herb", 10, 50);
    item("bandage", 5, 200);
    item("plank", 4, 1000);
    auto recipe = [&](const char* id, const char* station, u32 ms, std::initializer_list<asset::RecipeIngredient> in,
                      std::initializer_list<asset::RecipeIngredient> out) {
        asset::Recipe r;
        r.id = id;
        r.name = id;
        r.station = station;
        r.time_ms = ms;
        Array<asset::RecipeIngredient> a, b;
        for (const auto& i : in) a.push_back(i);
        for (const auto& o : out) b.push_back(o);
        REQUIRE(c.add_recipe(r, Span<const asset::RecipeIngredient>(a.data(), a.size()), Span<const asset::RecipeIngredient>(b.data(), b.size())));
    };
    recipe("bandage", "", 0, {{0, 2}, {1, 1}}, {{2, 1}});
    recipe("shelf", "workbench", 1000, {{3, 4}}, {{0, 1}, {2, 3}});
    recipe("mend", "workbench", 0, {{2, 1}}, {{1, 1}});
    REQUIRE(asset::item_catalog_problem(c) == nullptr);
    return c;
}

Inventory empty_inv(u32 max_weight = 0) {
    Inventory inv;
    inv.catalog.guid = kCat;
    inv.max_weight_milli = max_weight;
    return inv;
}

bool same(const Inventory& a, const Inventory& b) {
    for (u32 s = 0; s < kInventorySlots; ++s)
        if (a.item[s] != b.item[s] || a.qty[s] != b.qty[s]) return false;
    return true;
}

struct Rig {
    asset::ItemCatalog cat = make_catalog();
    World w;
    u64 t = 0;
    Rig() {
        w.register_components<SaveKey>();
        install_crafting(w, nullptr);
        w.resource<ItemRegistry>().define(kCat, cat);
    }
    void tick(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Entity hero(u32 max_weight = 0) { return w.spawn_with(SaveKey{1}, empty_inv(max_weight)); }
    Entity bench(const char* tag = "workbench") {
        CraftingStation s;
        s.station = tag;
        return w.spawn_with(SaveKey{2}, s);
    }
    void give(Entity e, u32 item, u32 qty) { REQUIRE(inventory_add(*w.try_get_mut<Inventory>(e), cat, item, qty) == qty); }
    u32 count(Entity e, u32 item) { return inventory_count(*w.try_get_mut<Inventory>(e), item); }
    void craft(Entity who, const char* recipe, Entity station = Entity()) {
        CraftItem q;
        q.owner = who;
        q.recipe = recipe;
        q.station = station;
        EventAccess(w).push(q);
    }
    u32 crafted() { u32 n = 0; for (const ItemCrafted& e : EventAccess(w).read<ItemCrafted>()) { (void)e; ++n; } return n; }
    // Ticks until a craft finishes or fails (0: neither within two seconds); the event is readable on that tick.
    u32 until_done() {
        for (u32 i = 1; i <= 120; ++i) {
            tick();
            if (crafted() > 0 || failed() != 0xFFFF) return i;
        }
        return 0;
    }
    // The last CraftFailed reason, or 0xFFFF when none.
    u32 failed() {
        u32 r = 0xFFFF;
        for (const CraftFailed& e : EventAccess(w).read<CraftFailed>()) r = e.reason;
        return r;
    }
};

u32 R(CraftResult r) { return static_cast<u32>(r); }

}  // namespace

TEST_CASE("craft rules: apply takes the inputs and adds the outputs") {
    const asset::ItemCatalog c = make_catalog();
    Inventory inv = empty_inv();
    inventory_add(inv, c, 0, 5);
    inventory_add(inv, c, 1, 2);
    CHECK(craft_check(inv, c, 0) == CraftResult::Ok);
    CHECK(inventory_count(inv, 0) == 5);  // check changes nothing
    CHECK(craft_apply(inv, c, 0) == CraftResult::Ok);
    CHECK(inventory_count(inv, 0) == 3);
    CHECK(inventory_count(inv, 1) == 1);
    CHECK(inventory_count(inv, 2) == 1);
}

TEST_CASE("craft rules: a missing input changes nothing") {
    const asset::ItemCatalog c = make_catalog();
    Inventory inv = empty_inv();
    inventory_add(inv, c, 0, 1);
    inventory_add(inv, c, 1, 5);
    const Inventory before = inv;
    CHECK(craft_apply(inv, c, 0) == CraftResult::MissingInput);
    CHECK(same(inv, before));
    CHECK(craft_apply(inv, c, 99) == CraftResult::NoRecipe);
}

TEST_CASE("craft rules: no room for the outputs changes nothing, and the freed input slots count") {
    const asset::ItemCatalog c = make_catalog();
    // 4 planks (one slot) + 22 stacks of herb + a stack of cloth = all 24 slots used
    Inventory inv = empty_inv();
    inventory_add(inv, c, 3, 4);
    inventory_add(inv, c, 1, 10 * 22);
    inventory_add(inv, c, 0, 10);
    for (u32 s = 0; s < kInventorySlots; ++s) CHECK(inv.item[s] != 0);
    const Inventory snap = inv;
    // shelf frees one slot (the planks) but its outputs need two stacks (cloth tops up, bandages need one new slot): cloth is
    // full (10/10), so cloth needs a slot too
    CHECK(craft_apply(inv, c, 1) == CraftResult::NoRoom);
    CHECK(same(inv, snap));
    // one cloth less: the cloth output tops up its stack and the bandages take the slot the planks freed
    inventory_remove(inv, c, 0, 1);
    CHECK(craft_check(inv, c, 1) == CraftResult::Ok);
    CHECK(craft_apply(inv, c, 1) == CraftResult::Ok);
    CHECK(inventory_count(inv, 3) == 0);
    CHECK(inventory_count(inv, 2) == 3);
    CHECK(inventory_count(inv, 0) == 10);
}

TEST_CASE("craft rules: the weight limit is checked after the inputs come out") {
    const asset::ItemCatalog c = make_catalog();
    // mend: 1 bandage (200) -> 1 herb (50). A full inventory can still make it: the output is lighter than the input.
    Inventory inv = empty_inv(1000);
    inventory_add(inv, c, 2, 4);  // 800
    inventory_add(inv, c, 0, 2);  // 1000
    CHECK(inventory_weight_milli(inv, c) == 1000);
    CHECK(craft_apply(inv, c, 2) == CraftResult::Ok);
    CHECK(inventory_weight_milli(inv, c) == 850);
    // bandage: 2 cloth (200) + herb (50) -> bandage (200): 50 lighter, fits at the limit too
    Inventory at_limit = empty_inv(1000);
    inventory_add(at_limit, c, 0, 2);
    inventory_add(at_limit, c, 1, 1);
    inventory_add(at_limit, c, 2, 3);  // 200 + 50 + 600 = 850
    inventory_add(at_limit, c, 1, 3);  // 1000
    CHECK(inventory_weight_milli(at_limit, c) == 1000);
    CHECK(craft_apply(at_limit, c, 0) == CraftResult::Ok);
    CHECK(inventory_weight_milli(at_limit, c) <= 1000);
    // shelf: 4 planks (4000) -> cloth + 3 bandage (700); with a 600 limit the outputs do not fit, and nothing changes
    Inventory heavy = empty_inv(4000);
    inventory_add(heavy, c, 3, 4);
    heavy.max_weight_milli = 600;  // a limit lowered after the fact: the inputs come out, 700 of outputs is still over
    const Inventory snap = heavy;
    CHECK(craft_apply(heavy, c, 1) == CraftResult::NoRoom);
    CHECK(same(heavy, snap));
}

TEST_CASE("craft events: by hand") {
    Rig rig;
    const Entity h = rig.hero();
    rig.give(h, 0, 2);
    rig.give(h, 1, 1);
    rig.craft(h, "bandage");
    rig.tick(2);
    CHECK(rig.crafted() == 1);
    CHECK(rig.count(h, 2) == 1);
    rig.craft(h, "bandage");
    rig.tick(2);
    CHECK(rig.failed() == R(CraftResult::MissingInput));
    rig.craft(h, "nope");
    rig.tick(2);
    CHECK(rig.failed() == R(CraftResult::NoRecipe));
}

TEST_CASE("craft events: a station recipe needs a matching station; an instant one applies at once") {
    Rig rig;
    const Entity h = rig.hero();
    const Entity wb = rig.bench();
    const Entity forge = rig.bench("forge");
    rig.give(h, 2, 1);
    rig.craft(h, "mend");
    rig.tick(2);
    CHECK(rig.failed() == R(CraftResult::NoStation));
    rig.craft(h, "mend", forge);
    rig.tick(2);
    CHECK(rig.failed() == R(CraftResult::NoStation));
    CHECK(rig.count(h, 2) == 1);
    rig.craft(h, "mend", wb);
    rig.tick(2);
    CHECK(rig.crafted() == 1);
    CHECK(rig.count(h, 2) == 0);
    CHECK(rig.count(h, 1) == 1);
}

TEST_CASE("craft events: a timed craft completes after its time and a busy station refuses another") {
    Rig rig;
    const Entity h = rig.hero();
    const Entity wb = rig.bench();
    rig.give(h, 3, 4);
    rig.craft(h, "shelf", wb);
    rig.tick(2);
    CHECK(rig.count(h, 3) == 4);  // inputs are taken at the end
    CHECK(rig.w.try_get<CraftingStation>(wb)->recipe.view() == "shelf");
    rig.craft(h, "shelf", wb);
    rig.tick(2);
    CHECK(rig.failed() == R(CraftResult::StationBusy));
    rig.tick(50);
    CHECK(rig.count(h, 3) == 4);
    CHECK(rig.until_done() > 0);  // past one second
    CHECK(rig.count(h, 3) == 0);
    CHECK(rig.count(h, 2) == 3);
    CHECK(rig.count(h, 0) == 1);
    CHECK(rig.w.try_get<CraftingStation>(wb)->recipe.empty());
}

TEST_CASE("craft events: a timed craft whose inputs are gone at the end fails with its reason") {
    Rig rig;
    const Entity h = rig.hero();
    const Entity wb = rig.bench();
    rig.give(h, 3, 4);
    rig.craft(h, "shelf", wb);
    rig.tick(2);
    TakeItem t;
    t.owner = h;
    t.item = "plank";
    t.qty = 4;
    EventAccess(rig.w).push(t);
    CHECK(rig.until_done() > 0);
    CHECK(rig.failed() == R(CraftResult::MissingInput));
    CHECK(rig.count(h, 2) == 0);
    CHECK(rig.w.try_get<CraftingStation>(wb)->recipe.empty());
}

TEST_CASE("craft: result names and the station keeps the queued craft") {
    CHECK(StringView(craft_result_name(CraftResult::NoRoom)) == "no_room");
    Rig rig;
    const Entity h = rig.hero();
    const Entity wb = rig.bench();
    rig.give(h, 3, 4);
    rig.craft(h, "shelf", wb);
    rig.tick(2);
    const CraftingStation* s = rig.w.try_get<CraftingStation>(wb);
    REQUIRE(s);
    CHECK(s->left_us > 0);
    CHECK(s->station.view() == "workbench");
}

TEST_CASE("craft events: a hand recipe is not queued at a station, and a retagged station fails the queued craft") {
    Rig rig;
    const Entity h = rig.hero();
    const Entity wb = rig.bench();
    rig.give(h, 3, 5);
    // hand recipe with a time, named with a station: applied at once and the station stays free
    asset::ItemCatalog c2 = make_catalog();
    asset::Recipe slow;
    slow.id = "slowbandage";
    slow.name = "slowbandage";
    slow.time_ms = 500;
    const asset::RecipeIngredient in{3, 1}, out{0, 1};
    REQUIRE(c2.add_recipe(slow, Span<const asset::RecipeIngredient>(&in, 1), Span<const asset::RecipeIngredient>(&out, 1)));
    rig.w.resource<ItemRegistry>().define(kCat, c2);
    rig.craft(h, "slowbandage", wb);
    rig.tick(2);
    CHECK(rig.crafted() == 1);
    CHECK(rig.w.try_get<CraftingStation>(wb)->recipe.empty());
    // retag while queued
    rig.craft(h, "shelf", wb);
    rig.tick(2);
    rig.w.try_get_mut<CraftingStation>(wb)->station = "forge";
    CHECK(rig.until_done() > 0);
    CHECK(rig.failed() == R(CraftResult::NoStation));
}
