// Loot tables (M-INVENTORY 3): the roller (guaranteed entries, weights, nested tables, quantity ranges, depth, merge),
// the same seed twice has the same golden hash, LootDrop on Died3D, Container opens, and a save round trip.

#include <doctest/doctest.h>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/inventory.h"
#include "mce/kit/loot.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kCat{3, 4};

asset::LootEntry item_entry(u16 ref, u32 weight, u32 qmin = 1, u32 qmax = 1, bool guaranteed = false) {
    asset::LootEntry e;
    e.kind = asset::LootKind::Item;
    e.guaranteed = guaranteed;
    e.ref = ref;
    e.weight = weight;
    e.qty_min = qmin;
    e.qty_max = qmax;
    return e;
}

asset::LootEntry table_entry(u16 ref, u32 weight) {
    asset::LootEntry e;
    e.kind = asset::LootKind::Table;
    e.ref = ref;
    e.weight = weight;
    return e;
}

void add_table(asset::ItemCatalog& c, const char* id, u16 rmin, u16 rmax, std::initializer_list<asset::LootEntry> es) {
    asset::LootTable t;
    t.id = id;
    t.rolls_min = rmin;
    t.rolls_max = rmax;
    Array<asset::LootEntry> a;
    for (const asset::LootEntry& e : es) a.push_back(e);
    REQUIRE(c.add_table(t, Span<const asset::LootEntry>(a.data(), a.size())));
}

// items: 0 coin, 1 gem, 2 rock; tables: 0 "chest", 1 "trash", 2 "dragon"
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
    item("coin", 100, 0);
    item("gem", 5, 100);
    item("rock", 10, 1000);
    add_table(c, "chest", 2, 4, {item_entry(0, 70, 5, 20), item_entry(1, 25, 1, 2), item_entry(2, 5, 1, 3), item_entry(0, 0, 10, 10, true)});
    add_table(c, "trash", 1, 1, {item_entry(2, 1, 1, 1)});
    add_table(c, "dragon", 1, 2, {table_entry(0, 1), table_entry(1, 1), item_entry(1, 0, 3, 3, true)});
    REQUIRE(asset::item_catalog_problem(c) == nullptr);
    return c;
}

u64 hash_rolls(const Array<LootRoll>& r) {
    u64 h = 1469598103934665603ull;
    for (const LootRoll& x : r) {
        h = (h ^ x.item) * 1099511628211ull;
        h = (h ^ x.qty) * 1099511628211ull;
    }
    return h;
}

Array<LootRoll> roll(const asset::ItemCatalog& c, const char* table, u64 seed, u32 stream = 0) {
    Array<LootRoll> out;
    procgen::Rng rng = procgen::Rng::child(seed, "loot", stream);
    REQUIRE(roll_loot(c, c.find_table(table), rng, out));
    return out;
}

u32 total_of(const Array<LootRoll>& r, u16 item) {
    u32 n = 0;
    for (const LootRoll& x : r)
        if (x.item == item) n += x.qty;
    return n;
}

struct Rig {
    asset::ItemCatalog cat = make_catalog();
    World w;
    u64 t = 0;
    Rig() {
        w.register_components<SaveKey>();
        install_loot(w, nullptr);
        w.resource<ItemRegistry>().define(kCat, cat);
    }
    void tick(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Entity hero(u32 max_weight = 0) {
        Inventory inv;
        inv.catalog.guid = kCat;
        inv.max_weight_milli = max_weight;
        return w.spawn_with(SaveKey{1}, inv);
    }
    Entity chest(const char* table, u64 seed) {
        Inventory inv;
        inv.catalog.guid = kCat;
        Container c;
        c.catalog.guid = kCat;
        c.table = table;
        c.seed = seed;
        return w.spawn_with(SaveKey{2}, inv, c);
    }
    ContainerOpened open(Entity box, Entity who) {
        OpenContainer o;
        o.container = box;
        o.opener = who;
        EventAccess(w).push(o);
        tick(2);
        ContainerOpened got;
        bool seen = false;
        for (const ContainerOpened& e : EventAccess(w).read<ContainerOpened>()) {
            got = e;
            seen = true;
        }
        REQUIRE(seen);
        return got;
    }
};

}  // namespace

TEST_CASE("loot: guaranteed entries come first, results merge, quantities stay in range") {
    const asset::ItemCatalog c = make_catalog();
    for (u64 seed = 1; seed < 200; ++seed) {
        const Array<LootRoll> r = roll(c, "chest", seed);
        REQUIRE(r.size() > 0);
        CHECK(r[0].item == 0);                // the guaranteed 10 coins lead
        CHECK(total_of(r, 0) >= 10);
        for (u32 i = 0; i < r.size(); ++i)
            for (u32 j = i + 1; j < r.size(); ++j) CHECK(r[i].item != r[j].item);  // merged
    }
    Array<LootRoll> none;
    procgen::Rng rng = procgen::Rng::child(1, "loot", 0);
    CHECK_FALSE(roll_loot(c, 99, rng, none));
    CHECK(none.size() == 0);
}

TEST_CASE("loot: weights decide the mix, a zero-weight entry never rolls by chance") {
    const asset::ItemCatalog c = make_catalog();
    u32 coins = 0, gems = 0, rocks = 0;
    for (u64 seed = 1; seed <= 2000; ++seed) {
        const Array<LootRoll> r = roll(c, "trash", seed);
        CHECK(r.size() == 1);
        rocks += total_of(r, 2);
        coins += total_of(r, 0);
        gems += total_of(r, 1);
    }
    CHECK(rocks == 2000);
    CHECK(coins == 0);
    CHECK(gems == 0);
    u32 dragon_gems = 0;
    for (u64 seed = 1; seed <= 500; ++seed) dragon_gems += total_of(roll(c, "dragon", seed), 1);
    CHECK(dragon_gems >= 500 * 3);  // the guaranteed three every time, plus what the nested chest adds
}

TEST_CASE("loot: nested tables roll their own entries; a depth past the limit stops") {
    asset::ItemCatalog c = make_catalog();
    // a loop: table 3 -> table 4 -> table 3 (the catalog check rejects it, the roller still must not recurse forever)
    add_table(c, "ping", 1, 1, {table_entry(4, 1), item_entry(0, 0, 1, 1, true)});
    add_table(c, "pong", 1, 1, {table_entry(3, 1)});
    CHECK(asset::item_catalog_problem(c) != nullptr);
    const Array<LootRoll> r = roll(c, "ping", 7);
    CHECK(r.size() <= 1);
    CHECK(total_of(r, 0) >= 1);
}

TEST_CASE("loot: the same seed rolls the same list (golden hash), another stream rolls another") {
    const asset::ItemCatalog c = make_catalog();
    const u64 a = hash_rolls(roll(c, "chest", 12345));
    CHECK(a == hash_rolls(roll(c, "chest", 12345)));
    CHECK(a != hash_rolls(roll(c, "chest", 12345, 1)));
    u64 all = 0;
    for (u64 s = 1; s <= 64; ++s) all = all * 31 + hash_rolls(roll(c, "dragon", s));
    // Pinned: a change here means every game's loot changed. Update only with a changelog line saying so.
    CHECK(all == 2496492206515761875ull);
}

TEST_CASE("loot: a LootDrop spawns one pickup per result when its owner dies, once") {
    Rig r;
    Health3D h;
    h.max = h.current = 10;
    LootDrop d;
    d.catalog.guid = kCat;
    d.table = "chest";
    d.seed = 99;
    const Entity mob = r.w.spawn_with(h, d, LocalTransform{{5, 0, 5}, math::quat::identity(), {1, 1, 1}});
    r.tick(2);
    DamageInfo di;
    di.amount = 50;
    REQUIRE(apply_damage(r.w, mob, di));
    r.tick(3);
    const Array<LootRoll> want = roll(r.cat, "chest", 99);
    u32 pickups = 0;
    u32 coins = 0;
    r.w.query<const ItemPickup3D>().each([&](Entity, const ItemPickup3D& p) {
        ++pickups;
        if (p.item.view() == "coin") coins += p.qty;
    });
    CHECK(pickups == want.size());
    CHECK(coins == total_of(want, 0));
    r.tick(5);
    u32 again = 0;
    r.w.query<const ItemPickup3D>().each([&](Entity, const ItemPickup3D&) { ++again; });
    CHECK(again == pickups);  // rolled once
    CHECK(r.w.try_get<LootDrop>(mob)->dropped == 1);
}

TEST_CASE("container: the first open rolls into the chest, the opener takes what fits, a later open takes more") {
    Rig r;
    const Entity box = r.chest("chest", 5);
    const Entity who = r.hero(1500);  // a gem weighs 100, a rock 1000: the cap stops the rocks
    const Array<LootRoll> want = roll(r.cat, "chest", 5);
    u32 total = 0;
    for (const LootRoll& x : want) total += x.qty;
    const ContainerOpened a = r.open(box, who);
    CHECK(a.taken + a.left == total);
    CHECK(a.taken > 0);
    CHECK(r.w.try_get<Container>(box)->filled == 1);
    CHECK(r.w.try_get<Container>(box)->rolls == 1);
    // a second open does not roll again
    const ContainerOpened b = r.open(box, who);
    CHECK(b.taken + b.left == a.left);
    CHECK(r.w.try_get<Container>(box)->rolls == 1);
    // a roomy opener takes the rest
    const Entity big = r.hero(0);
    const ContainerOpened c = r.open(box, big);
    CHECK(c.left <= b.left);
    CHECK(c.taken == b.left - c.left);
}

TEST_CASE("container: a chest and an opener on different catalogs move nothing") {
    Rig r;
    asset::ItemCatalog other = make_catalog();
    const asset::Guid g2{9, 9};
    r.w.resource<ItemRegistry>().define(g2, other);
    const Entity box = r.chest("chest", 5);
    Inventory inv;
    inv.catalog.guid = g2;
    const Entity who = r.w.spawn_with(inv);
    OpenContainer o;
    o.container = box;
    o.opener = who;
    EventAccess(r.w).push(o);
    r.tick(2);
    CHECK(r.w.try_get<Container>(box)->filled == 0);
}

TEST_CASE("container: a table missing from the chest's catalog rolls nothing and leaves the chest unfilled") {
    Rig r;
    const Entity box = r.chest("nope", 5);
    const Entity who = r.hero();
    OpenContainer o;
    o.container = box;
    o.opener = who;
    EventAccess(r.w).push(o);
    r.tick(2);
    CHECK(r.w.try_get<Container>(box)->filled == 0);
    // chest with an unloadable Container catalog but a loaded Inventory catalog: no crash, nothing rolled
    const Entity box2 = r.chest("chest", 5);
    r.w.try_get_mut<Container>(box2)->catalog = {};
    o.container = box2;
    o.opener = who;
    EventAccess(r.w).push(o);
    r.tick(2);
    CHECK(r.w.try_get<Container>(box2)->filled == 0);
    CHECK(inventory_count(*r.w.try_get<Inventory>(who), r.cat.find_item("coin")) == 0);
}

TEST_CASE("container: opening a chest with itself changes nothing; rolled loot that does not fit is dropped, not duplicated") {
    Rig r;
    const Entity box = r.chest("chest", 5);
    OpenContainer o;
    o.container = box;
    o.opener = box;
    EventAccess(r.w).push(o);
    r.tick(2);
    CHECK(r.w.try_get<Container>(box)->filled == 0);
    // a chest with a weight cap of 1 kg cannot hold what rolls; the opener never gets more than was rolled
    r.w.try_get_mut<Inventory>(box)->max_weight_milli = 1;
    const Entity who = r.hero();
    const Array<LootRoll> want = roll(r.cat, "chest", 5);
    u32 total = 0;
    for (const LootRoll& x : want) total += x.qty;
    const ContainerOpened a = r.open(box, who);
    CHECK(a.taken + a.left <= total);
}

TEST_CASE("loot: the flags and the roll count survive a save, and the same run twice has the same hash") {
    Array<byte> bytes;
    u32 left = 0;
    {
        Rig a;
        const Entity box = a.chest("dragon", 0);
        const Entity who = a.hero(1500);
        left = a.open(box, who).left;
        save::write_save(a.w, {}, bytes);
    }
    Rig b;
    const Entity box = b.chest("dragon", 0);
    b.hero(1500);
    REQUIRE(save::read_save(b.w, bytes.span()));
    CHECK(b.w.try_get<Container>(box)->filled == 1);
    CHECK(b.w.try_get<Container>(box)->rolls == 1);
    u32 in_box = 0;
    for (u32 i = 0; i < kInventorySlots; ++i) in_box += b.w.try_get<Inventory>(box)->qty[i];
    CHECK(in_box == left);
    auto run = [] {
        Rig r;
        const Entity box2 = r.chest("dragon", 0);
        const Entity who = r.hero(0);
        r.open(box2, who);
        return r.w.state_hash();
    };
    CHECK(run() == run());
}
