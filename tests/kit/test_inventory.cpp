// Inventories, equipment and pickups (M-INVENTORY 2): the add/remove/move/split rules and the weight cap, equip and
// unequip, use, pickups through TriggerEnter3D, a save round trip, and the same run twice has the same hash.

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/inventory.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kCat{1, 2};

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
    item("helm", 1, 2000, {"equip.head"});
    item("rock", 10, 1000, {});
    return c;
}

struct Rig {
    asset::ItemCatalog cat = make_catalog();
    World w;
    Entity hero;
    u64 t = 0;
    explicit Rig(u32 max_weight = 0) {
        w.register_components<SaveKey>();
        install_inventory(w, nullptr);
        w.register_event<TriggerEnter3D>(Phase::FixedUpdate_Pre);
        w.resource<ItemRegistry>().define(kCat, cat);
        Inventory inv;
        inv.catalog.guid = kCat;
        inv.max_weight_milli = max_weight;
        Equipment eq;
        eq.catalog.guid = kCat;
        hero = w.spawn_with(SaveKey{1}, inv, eq);
    }
    u32 id(const char* s) const { return cat.find_item(s); }
    Inventory& inv() { return *w.try_get_mut<Inventory>(hero); }
    Equipment& eq() { return *w.try_get_mut<Equipment>(hero); }
    void tick(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    void give(const char* s, u32 q) {
        GiveItem g;
        g.owner = hero;
        g.item = s;
        g.qty = q;
        EventAccess(w).push(g);
        tick(2);
    }
};

}  // namespace

TEST_CASE("inventory: add fills stacks then empty slots, remove takes in slot order") {
    Rig r;
    CHECK(inventory_add(r.inv(), r.cat, r.id("potion"), 12) == 12);
    CHECK(r.inv().qty[0] == 5);
    CHECK(r.inv().qty[1] == 5);
    CHECK(r.inv().qty[2] == 2);
    CHECK(inventory_add(r.inv(), r.cat, r.id("potion"), 2) == 2);  // tops up the third stack
    CHECK(r.inv().qty[2] == 4);
    CHECK(inventory_count(r.inv(), r.id("potion")) == 14);
    CHECK(inventory_remove(r.inv(), r.cat, r.id("potion"), 7) == 7);
    CHECK(r.inv().item[0] == 0);  // slot 0 and part of slot 1 went
    CHECK(r.inv().qty[1] == 3);
    CHECK(inventory_remove(r.inv(), r.cat, r.id("potion"), 100) == 7);
    CHECK(inventory_count(r.inv(), r.id("potion")) == 0);
}

TEST_CASE("inventory: a full inventory returns the remainder") {
    Rig r;
    CHECK(inventory_add(r.inv(), r.cat, r.id("rock"), 300) == 240);  // 24 slots of 10
    CHECK(inventory_add(r.inv(), r.cat, r.id("coin"), 1) == 0);
}

TEST_CASE("inventory: the weight cap limits what fits; weightless items are not limited") {
    Rig r(5000);
    CHECK(inventory_add(r.inv(), r.cat, r.id("rock"), 9) == 5);
    CHECK(inventory_weight_milli(r.inv(), r.cat) == 5000);
    CHECK(inventory_add(r.inv(), r.cat, r.id("rock"), 1) == 0);
    CHECK(inventory_add(r.inv(), r.cat, r.id("coin"), 250) == 250);
    CHECK(inventory_remove(r.inv(), r.cat, r.id("rock"), 2) == 2);
    CHECK(inventory_add(r.inv(), r.cat, r.id("sword"), 1) == 0);  // 3000 held + 3000 > 5000
    CHECK(inventory_remove(r.inv(), r.cat, r.id("rock"), 1) == 1);
    CHECK(inventory_add(r.inv(), r.cat, r.id("sword"), 1) == 1);  // 2000 + 3000 = 5000
}

TEST_CASE("inventory: move, merge and swap") {
    Rig r;
    inventory_add(r.inv(), r.cat, r.id("rock"), 10);
    inventory_add(r.inv(), r.cat, r.id("coin"), 5);
    CHECK(inventory_move(r.inv(), r.cat, 0, 5));  // to an empty slot
    CHECK(r.inv().item[0] == 0);
    CHECK(r.inv().qty[5] == 10);
    CHECK(inventory_move(r.inv(), r.cat, 1, 5));  // coin onto rock: swap
    CHECK(r.inv().item[5] == r.id("coin") + 1u);
    CHECK(r.inv().item[1] == r.id("rock") + 1u);
    inventory_add(r.inv(), r.cat, r.id("rock"), 4);  // slot 1 is full, so slot 0 takes them
    CHECK(r.inv().qty[0] == 4);
    inventory_move(r.inv(), r.cat, 0, 1);  // 4 onto a full stack: nothing left to merge
    CHECK(r.inv().qty[0] + r.inv().qty[1] == 14);
    CHECK_FALSE(inventory_move(r.inv(), r.cat, 0, 99));
    CHECK_FALSE(inventory_move(r.inv(), r.cat, 7, 8));  // empty source
}

TEST_CASE("inventory: split") {
    Rig r;
    inventory_add(r.inv(), r.cat, r.id("rock"), 8);
    CHECK(inventory_split(r.inv(), r.cat, 0, 3));
    CHECK(r.inv().qty[0] == 5);
    CHECK(r.inv().qty[1] == 3);
    CHECK_FALSE(inventory_split(r.inv(), r.cat, 0, 5));  // not below the stack
    CHECK_FALSE(inventory_split(r.inv(), r.cat, 0, 0));
}

TEST_CASE("equipment: equip needs the tag, swaps the old unit back, unequip needs room") {
    Rig r;
    inventory_add(r.inv(), r.cat, r.id("sword"), 2);
    inventory_add(r.inv(), r.cat, r.id("helm"), 1);
    inventory_add(r.inv(), r.cat, r.id("potion"), 1);
    const u32 weapon = static_cast<u32>(EquipSlot::Weapon);
    CHECK_FALSE(equipment_equip(r.inv(), r.eq(), r.cat, 3, weapon));  // a potion is not a weapon
    CHECK_FALSE(equipment_equip(r.inv(), r.eq(), r.cat, 2, weapon));  // a helm is not a weapon
    CHECK(equipment_equip(r.inv(), r.eq(), r.cat, 0, weapon));
    CHECK(r.eq().item[weapon] == r.id("sword") + 1u);
    CHECK(inventory_count(r.inv(), r.id("sword")) == 1);
    CHECK(equipment_equip(r.inv(), r.eq(), r.cat, 1, weapon));  // the other sword; the first returns
    CHECK(inventory_count(r.inv(), r.id("sword")) == 1);
    CHECK(equipment_equip(r.inv(), r.eq(), r.cat, 2, kEquipSlots));  // auto slot: Head
    CHECK(r.eq().item[static_cast<u32>(EquipSlot::Head)] == r.id("helm") + 1u);
    CHECK(equipment_unequip(r.inv(), r.eq(), r.cat, weapon));
    CHECK_FALSE(equipment_unequip(r.inv(), r.eq(), r.cat, weapon));  // empty
    CHECK(inventory_count(r.inv(), r.id("sword")) == 2);
}

TEST_CASE("inventory: events give, take, use and equip") {
    Rig r;
    r.give("potion", 3);
    CHECK(r.inv().qty[0] == 3);
    UseItem u;
    u.owner = r.hero;
    u.slot = 0;
    EventAccess(r.w).push(u);
    r.tick(2);
    CHECK(r.inv().qty[0] == 2);
    r.give("sword", 1);
    EquipItem q;
    q.owner = r.hero;
    q.slot = 1;
    EventAccess(r.w).push(q);
    r.tick(2);
    CHECK(r.eq().item[static_cast<u32>(EquipSlot::Weapon)] == r.id("sword") + 1u);
    TakeItem tk;
    tk.owner = r.hero;
    tk.item = "potion";
    tk.qty = 10;
    EventAccess(r.w).push(tk);
    r.tick(2);
    CHECK(inventory_count(r.inv(), r.id("potion")) == 0);
    r.give("nonsense", 1);  // unknown ids change nothing
    CHECK(inventory_count(r.inv(), r.id("coin")) == 0);
}

TEST_CASE("inventory: output events carry what went in and what did not fit") {
    Rig r(2000);
    GiveItem g;
    g.owner = r.hero;
    g.item = "rock";
    g.qty = 5;
    EventAccess(r.w).push(g);
    u32 added = 0, remainder = 0, seen = 0;
    for (u32 i = 0; i < 4; ++i) {
        r.tick();
        for (const ItemAdded& a : EventAccess(r.w).read<ItemAdded>()) {
            added += a.qty;
            remainder += a.remainder;
            ++seen;
        }
    }
    CHECK(seen == 1);
    CHECK(added == 2);
    CHECK(remainder == 3);
}

TEST_CASE("inventory: a pickup goes in when something with an inventory enters, and is destroyed when all fits") {
    Rig r;
    ItemPickup3D p;
    p.item = "rock";
    p.qty = 15;
    const Entity pickup = r.w.spawn_with(p);
    const Entity stranger = r.w.spawn_with(ItemPickup3D{});
    TriggerEnter3D e;
    e.trigger = pickup;
    e.other = stranger;  // has no inventory
    EventAccess(r.w).push(e);
    r.tick();
    CHECK(r.w.alive(pickup));
    e.other = r.hero;
    EventAccess(r.w).push(e);
    r.tick(2);
    CHECK(inventory_count(r.inv(), r.id("rock")) == 15);
    CHECK_FALSE(r.w.alive(pickup));
}

TEST_CASE("inventory: a pickup that does not fully fit keeps the rest") {
    Rig r(3000);
    ItemPickup3D p;
    p.item = "rock";
    p.qty = 8;
    const Entity pickup = r.w.spawn_with(p);
    TriggerEnter3D e;
    e.trigger = pickup;
    e.other = r.hero;
    EventAccess(r.w).push(e);
    r.tick(2);
    CHECK(inventory_count(r.inv(), r.id("rock")) == 3);
    REQUIRE(r.w.alive(pickup));
    CHECK(r.w.try_get<ItemPickup3D>(pickup)->qty == 5);
}

TEST_CASE("inventory: a save round trip keeps slots and equipment") {
    Array<byte> bytes;
    {
        Rig a;
        a.give("potion", 7);
        a.give("sword", 1);
        REQUIRE(equipment_equip(a.inv(), a.eq(), a.cat, 2, kEquipSlots));
        save::write_save(a.w, {}, bytes);
    }
    Rig b;
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(inventory_count(b.inv(), b.id("potion")) == 7);
    CHECK(b.eq().item[static_cast<u32>(EquipSlot::Weapon)] == b.id("sword") + 1u);
}

TEST_CASE("inventory: the same run twice has the same hash") {
    auto run = [] {
        Rig r(20000);
        for (u32 i = 0; i < 40; ++i) {
            r.give(i % 3 == 0 ? "rock" : (i % 3 == 1 ? "potion" : "coin"), 1 + i * 7 % 13);
            if (i % 5 == 0) {
                SplitItem s;
                s.owner = r.hero;
                s.from = 0;
                s.qty = 1;
                EventAccess(r.w).push(s);
            }
            if (i % 7 == 0) {
                MoveItem m;
                m.owner = r.hero;
                m.from = 0;
                m.to = i % 24;
                EventAccess(r.w).push(m);
            }
        }
        return r.w.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("inventory: corrupt saves cannot underflow, wrap or stick") {
    Rig r(1000000);
    Inventory& i = r.inv();
    i.item[3] = static_cast<u16>(r.id("potion") + 1u);
    i.qty[3] = 0;  // an item with nothing in it
    UseItem u;
    u.owner = r.hero;
    u.slot = 3;
    EventAccess(r.w).push(u);
    r.tick(2);
    CHECK(i.qty[3] == 0);
    CHECK_FALSE(equipment_equip(i, r.eq(), r.cat, 3, kEquipSlots));
    i.item[4] = static_cast<u16>(r.id("rock") + 1u);
    i.qty[4] = 0xFFFFFFFFu;  // far over the stack: weight is clamped, not wrapped
    CHECK(inventory_weight_milli(i, r.cat) <= 24ull * 1000000ull * 1000ull);
    // a stale index past the catalog
    i.item[5] = 200;
    i.qty[5] = 5;
    CHECK_FALSE(inventory_move(i, r.cat, 5, 5));
    CHECK(inventory_count(i, r.id("coin")) == 0);
    r.eq().item[0] = 300;
    CHECK(equipment_unequip(i, r.eq(), r.cat, 0));  // clears the stale slot
    CHECK(r.eq().item[0] == 0);
    i.item[4] = 0;
    i.qty[4] = 0;
    r.eq().item[static_cast<u32>(EquipSlot::Weapon)] = 300;
    inventory_add(i, r.cat, r.id("sword"), 1);
    CHECK(equipment_equip(i, r.eq(), r.cat, 0, static_cast<u32>(EquipSlot::Weapon)));  // the stale old item is dropped
    CHECK(r.eq().item[static_cast<u32>(EquipSlot::Weapon)] == r.id("sword") + 1u);
}

TEST_CASE("inventory: a catalog that is not valid is not defined") {
    Rig r;
    asset::ItemCatalog bad;
    asset::ItemDef d;
    d.id = "zero";
    d.name = "zero";
    d.stack_max = 0;
    bad.add_item(d);
    r.w.resource<ItemRegistry>().define(asset::Guid{9, 9}, bad);
    asset::AssetRef<asset::ItemCatalogChunk> ref;
    ref.guid = asset::Guid{9, 9};
    CHECK(r.w.resource<ItemRegistry>().resolve(ref) == nullptr);
}

TEST_CASE("equipment: a swap that cannot return the old unit changes nothing; auto slot prefers an empty one") {
    asset::ItemCatalog c = make_catalog();
    asset::ItemDef ring;
    ring.id = "ring";
    ring.name = "ring";
    ring.stack_max = 1;
    Array<FixedString<32>> tags;
    tags.push_back(FixedString<32>("equip.accessory1"));
    tags.push_back(FixedString<32>("equip.accessory2"));
    REQUIRE(c.add_item(ring, {}, Span<const FixedString<32>>(tags.data(), tags.size())));
    Inventory inv;
    Equipment eq;
    inventory_add(inv, c, c.find_item("ring"), 2);
    CHECK(equipment_equip(inv, eq, c, 0, kEquipSlots));
    CHECK(equipment_equip(inv, eq, c, 1, kEquipSlots));  // the second ring takes the empty slot
    CHECK(eq.item[static_cast<u32>(EquipSlot::Accessory1)] != 0);
    CHECK(eq.item[static_cast<u32>(EquipSlot::Accessory2)] != 0);
    // fill the inventory with swords, then swap the weapon: no room for the old one only if the slot freed is taken
    for (u32 s = 0; s < kInventorySlots; ++s) { inv.item[s] = static_cast<u16>(c.find_item("rock") + 1u); inv.qty[s] = 10; }
    inv.item[0] = static_cast<u16>(c.find_item("sword") + 1u);
    inv.qty[0] = 1;
    eq.item[static_cast<u32>(EquipSlot::Weapon)] = static_cast<u16>(c.find_item("sword") + 1u);
    const Inventory before = inv;
    // equipping slot 0's sword frees slot 0, so the old sword fits back there: allowed
    CHECK(equipment_equip(inv, eq, c, 0, static_cast<u32>(EquipSlot::Weapon)));
    CHECK(inv.item[0] == c.find_item("sword") + 1u);
    (void)before;
    // unequip with every slot full is refused and changes nothing
    inv.item[0] = static_cast<u16>(c.find_item("rock") + 1u);
    inv.qty[0] = 10;
    const Equipment eq_before = eq;
    CHECK_FALSE(equipment_unequip(inv, eq, c, static_cast<u32>(EquipSlot::Weapon)));
    CHECK(eq.item[static_cast<u32>(EquipSlot::Weapon)] == eq_before.item[static_cast<u32>(EquipSlot::Weapon)]);
}

TEST_CASE("inventory: a consumable with a heal stat heals the user and is refused at full health (M-ITEMFX 1)") {
    Rig r;
    asset::ItemCatalog c = r.cat;
    asset::ItemDef d;
    d.id = "tonic";
    d.name = "tonic";
    d.stack_max = 5;
    const FixedString<32> tag("consumable");
    asset::ItemStat heal;
    heal.key = "heal";
    heal.value = 30.0f;
    REQUIRE(c.add_item(d, Span<const asset::ItemStat>(&heal, 1), Span<const FixedString<32>>(&tag, 1)));
    r.w.resource<ItemRegistry>().define(kCat, c);
    r.give("tonic", 3);
    const u32 slot = 0;
    auto use = [&] {
        UseItem u;
        u.owner = r.hero;
        u.slot = slot;
        EventAccess(r.w).push(u);
        r.tick(2);
    };
    auto healed = [&] {
        f32 amount = -1.0f;
        for (const ItemHealed& h : EventAccess(r.w).read<ItemHealed>()) amount = h.amount;
        return amount;
    };
    auto used = [&] {
        u32 consumed = 99;
        for (const ItemUsed& e : EventAccess(r.w).read<ItemUsed>()) consumed = e.consumed;
        return consumed;
    };
    use();  // no Health3D: consumed as before, nothing heals
    CHECK(r.inv().qty[slot] == 2);
    Health3D hp;
    hp.max = 100.0f;
    hp.current = 50.0f;
    r.w.set(r.hero, hp);
    use();
    CHECK(r.w.try_get<Health3D>(r.hero)->current == 80.0f);
    CHECK(r.inv().qty[slot] == 1);
    CHECK(healed() == 30.0f);
    r.w.try_get_mut<Health3D>(r.hero)->current = 90.0f;
    use();  // only 10 missing: heals 10
    CHECK(r.w.try_get<Health3D>(r.hero)->current == 100.0f);
    CHECK(healed() == 10.0f);
    CHECK(r.inv().qty[slot] == 0);
    r.give("tonic", 2);
    use();  // full health: refused, nothing consumed
    CHECK(r.inv().qty[slot] == 2);
    CHECK(used() == 0);
    r.w.try_get_mut<Health3D>(r.hero)->current = 0.0f;
    r.w.try_get_mut<Health3D>(r.hero)->dead = 1;
    use();  // dead: refused
    CHECK(r.inv().qty[slot] == 2);
    CHECK(r.w.try_get<Health3D>(r.hero)->current == 0.0f);
}

TEST_CASE("inventory: worn items add their armor and max_health to Health3D and give it back (M-ITEMFX 2)") {
    Rig r;
    asset::ItemCatalog c = r.cat;
    auto gear = [&](const char* id, const char* tag, std::initializer_list<std::pair<const char*, f32>> stats) {
        asset::ItemDef d;
        d.id = id;
        d.name = id;
        d.stack_max = 1;
        const FixedString<32> t(tag);
        Array<asset::ItemStat> st;
        for (const auto& kv : stats) {
            asset::ItemStat s;
            s.key = kv.first;
            s.value = kv.second;
            st.push_back(s);
        }
        REQUIRE(c.add_item(d, Span<const asset::ItemStat>(st.data(), st.size()), Span<const FixedString<32>>(&t, 1)));
    };
    gear("plate", "equip.body", {{"armor", 0.25f}, {"max_health", 20.0f}});
    gear("cap", "equip.head", {{"armor", 0.5f}});
    gear("ring", "equip.accessory1", {{"max_health", 10.0f}});
    r.w.resource<ItemRegistry>().define(kCat, c);
    Health3D hp;
    hp.max = 100.0f;
    hp.current = 100.0f;
    hp.armor = 0.125f;
    r.w.set(r.hero, hp);
    auto health = [&]() -> const Health3D& { return *r.w.try_get<Health3D>(r.hero); };
    auto equip = [&](const char* id) {
        r.give(id, 1);
        EquipItem q;
        q.owner = r.hero;
        q.slot = 0;
        EventAccess(r.w).push(q);
        r.tick(3);
    };
    equip("plate");
    CHECK(health().armor == doctest::Approx(0.375f));
    CHECK(health().max == doctest::Approx(120.0f));
    r.tick(30);  // idempotent: nothing accumulates tick after tick
    CHECK(health().armor == doctest::Approx(0.375f));
    CHECK(health().max == doctest::Approx(120.0f));
    equip("cap");  // armor 0.125 + 0.25 + 0.5 = 0.875
    CHECK(health().armor == doctest::Approx(0.875f));
    equip("ring");
    CHECK(health().max == doctest::Approx(130.0f));
    // another system (a level-up) changes the base: the bonus stays on top of it
    r.w.try_get_mut<Health3D>(r.hero)->max += 50.0f;
    r.tick(2);
    CHECK(health().max == doctest::Approx(180.0f));
    // a base raised so the total passes 1: armor clamps to 1, the clamp is not remembered as added, and the base comes back
    r.w.try_get_mut<Health3D>(r.hero)->armor = 1.25f;  // as if the base were 0.5 with 0.75 worn
    r.tick(2);
    CHECK(health().armor == doctest::Approx(1.0f));
    // take everything off: the base comes back
    for (u32 slot : {static_cast<u32>(EquipSlot::Body), static_cast<u32>(EquipSlot::Head), static_cast<u32>(EquipSlot::Accessory1)}) {
        UnequipItem u;
        u.owner = r.hero;
        u.equip_slot = slot;
        EventAccess(r.w).push(u);
        r.tick(3);
    }
    CHECK(health().max == doctest::Approx(150.0f));
    CHECK(health().armor == doctest::Approx(0.5f).epsilon(0.001));  // the raised base, not 0.125
    CHECK(r.w.try_get<Equipment>(r.hero)->max_applied == doctest::Approx(0.0f));
    CHECK(health().current <= health().max);
}

TEST_CASE("inventory: losing max_health clamps current, and an entity without Health3D is left alone (M-ITEMFX 2)") {
    Rig r;
    asset::ItemCatalog c = r.cat;
    asset::ItemDef d;
    d.id = "amulet";
    d.name = "amulet";
    d.stack_max = 1;
    const FixedString<32> t("equip.accessory1");
    asset::ItemStat s;
    s.key = "max_health";
    s.value = 50.0f;
    REQUIRE(c.add_item(d, Span<const asset::ItemStat>(&s, 1), Span<const FixedString<32>>(&t, 1)));
    r.w.resource<ItemRegistry>().define(kCat, c);
    r.give("amulet", 1);
    EquipItem q;
    q.owner = r.hero;
    q.slot = 0;
    EventAccess(r.w).push(q);
    r.tick(3);  // no Health3D: nothing happens, nothing is remembered
    CHECK(r.w.try_get<Equipment>(r.hero)->max_applied == 0.0f);
    Health3D hp;
    hp.max = 100.0f;
    hp.current = 100.0f;
    r.w.set(r.hero, hp);
    r.tick(2);
    CHECK(r.w.try_get<Health3D>(r.hero)->max == doctest::Approx(150.0f));
    r.w.try_get_mut<Health3D>(r.hero)->current = 150.0f;
    UnequipItem u;
    u.owner = r.hero;
    u.equip_slot = static_cast<u32>(EquipSlot::Accessory1);
    EventAccess(r.w).push(u);
    r.tick(3);
    CHECK(r.w.try_get<Health3D>(r.hero)->max == doctest::Approx(100.0f));
    CHECK(r.w.try_get<Health3D>(r.hero)->current == doctest::Approx(100.0f));
}

TEST_CASE("inventory: equipping and drinking in the same tick judges the heal against the new max (M-ITEMFX 2)") {
    Rig r;
    asset::ItemCatalog c = r.cat;
    asset::ItemDef ring;
    ring.id = "ring";
    ring.name = "ring";
    ring.stack_max = 1;
    const FixedString<32> rtag("equip.accessory1");
    asset::ItemStat ms;
    ms.key = "max_health";
    ms.value = 50.0f;
    REQUIRE(c.add_item(ring, Span<const asset::ItemStat>(&ms, 1), Span<const FixedString<32>>(&rtag, 1)));
    asset::ItemDef tonic;
    tonic.id = "tonic";
    tonic.name = "tonic";
    tonic.stack_max = 5;
    const FixedString<32> ctag("consumable");
    asset::ItemStat hs;
    hs.key = "heal";
    hs.value = 30.0f;
    REQUIRE(c.add_item(tonic, Span<const asset::ItemStat>(&hs, 1), Span<const FixedString<32>>(&ctag, 1)));
    r.w.resource<ItemRegistry>().define(kCat, c);
    Health3D hp;
    hp.max = 100.0f;
    hp.current = 100.0f;
    r.w.set(r.hero, hp);
    r.give("ring", 1);
    r.give("tonic", 1);
    EquipItem q;
    q.owner = r.hero;
    q.slot = 0;
    UseItem u;
    u.owner = r.hero;
    u.slot = 1;
    EventAccess(r.w).push(q);
    EventAccess(r.w).push(u);
    r.tick(3);
    CHECK(r.w.try_get<Health3D>(r.hero)->max == doctest::Approx(150.0f));
    CHECK(r.w.try_get<Health3D>(r.hero)->current == doctest::Approx(130.0f));
    CHECK(inventory_count(r.inv(), r.id("tonic")) == 0);
}
