#include "mce/kit/loot.h"

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/hash.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/inventory.h"
#include "mce/log.h"

namespace mce::kit {

namespace {
struct LootInstalled {};

constexpr u32 kMaxResults = 4096;
constexpr u32 kMaxWork = 16384;  // draws and entries visited in one roll: a hostile chain of tables cannot hang a frame

void add_roll(Array<LootRoll>& out, u16 item, u32 qty) {
    if (qty == 0) return;
    for (LootRoll& r : out)
        if (r.item == item) {
            const u64 sum = u64(r.qty) + qty;
            r.qty = sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(sum);
            return;
        }
    if (out.size() < kMaxResults) out.push_back(LootRoll{item, qty});
}

u32 qty_of(const asset::LootEntry& e, procgen::Rng& rng) { return static_cast<u32>(rng.range(static_cast<i32>(e.qty_min > 0x7FFFFFFFu ? 0x7FFFFFFFu : e.qty_min), static_cast<i32>(e.qty_max > 0x7FFFFFFFu ? 0x7FFFFFFFu : e.qty_max))); }

void roll_table(const asset::ItemCatalog& c, u32 table, procgen::Rng& rng, Array<LootRoll>& out, u32 depth, u32& work) {
    if (depth > asset::kItemMaxTableDepth || table >= c.tables().size() || work == 0) return;
    const asset::LootTable& t = c.tables()[table];
    const Span<const asset::LootEntry> entries = c.entries_of(t);
    for (const asset::LootEntry& e : entries) {  // guaranteed first, in entry order
        if (!e.guaranteed || work == 0) continue;
        --work;
        if (e.kind == asset::LootKind::Item) add_roll(out, e.ref, qty_of(e, rng));
        else roll_table(c, e.ref, rng, out, depth + 1, work);
    }
    u64 total = 0;
    for (const asset::LootEntry& e : entries)
        if (!e.guaranteed) total += e.weight;
    if (total == 0) return;
    const u32 rolls = static_cast<u32>(rng.range(t.rolls_min, t.rolls_max));
    for (u32 i = 0; i < rolls && work > 0; ++i) {
        --work;
        u64 pick = rng.next_u64() % total;
        for (const asset::LootEntry& e : entries) {
            if (e.guaranteed) continue;
            if (pick < e.weight) {
                if (e.kind == asset::LootKind::Item) add_roll(out, e.ref, qty_of(e, rng));
                else roll_table(c, e.ref, rng, out, depth + 1, work);
                break;
            }
            pick -= e.weight;
        }
    }
}

u64 seed_of(u64 seed, const FixedString<64>& table, Entity e) {
    return seed != 0 ? seed : hash_combine(fnv1a64(table.view()), e.index);
}

// The fixed ring a drop's pickups spread on: eight spots, then a second ring twice as far.
void ring_offset(u32 i, f32 r, f32& x, f32& z) {
    static constexpr f32 kX[8] = {1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f, 0.0f, 0.70710678f};
    static constexpr f32 kZ[8] = {0.0f, 0.70710678f, 1.0f, 0.70710678f, 0.0f, -0.70710678f, -1.0f, -0.70710678f};
    const u32 ring = i / 8u + 1u;
    const f32 k = static_cast<f32>(ring);
    x = kX[i % 8u] * r * k;
    z = kZ[i % 8u] * r * k;
}

bool resolve_table(ItemRegistry& reg, const asset::AssetRef<asset::ItemCatalogChunk>& ref, const FixedString<64>& table, const asset::ItemCatalog*& c, u32& index) {
    c = reg.resolve(ref);
    if (!c) return false;
    const u16 t = c->find_table(table.view());
    if (t == asset::kNoItem) return false;
    index = t;
    return true;
}

void kit_loot(Frame& f) {
    World& w = f.world();
    ItemRegistry& reg = w.resource<ItemRegistry>();
    EventAccess ev = f.events();

    for (const Died3D& d : ev.read<Died3D>()) {  // drops
        if (d.entity.is_null() || !w.alive(d.entity)) continue;
        LootDrop* drop = w.try_get_mut<LootDrop>(d.entity);
        if (!drop || drop->dropped) continue;
        const asset::ItemCatalog* c = nullptr;
        u32 table = 0;
        if (!resolve_table(reg, drop->catalog, drop->table, c, table)) {
            MCE_LOG_WARN("loot: drop table '{}' did not resolve when its owner died; no drop", drop->table.view());
            drop->dropped = 1;  // Died3D fires once: say so, do not leave a half-state
            continue;
        }
        procgen::Rng rng = procgen::Rng::child(seed_of(drop->seed, drop->table, d.entity), "loot", drop->rolls);
        ++drop->rolls;
        drop->dropped = 1;
        Array<LootRoll> rolled;
        u32 work = kMaxWork;
        roll_table(*c, table, rng, rolled, 0, work);
        const LocalTransform* at = w.try_get<LocalTransform>(d.entity);
        const math::vec3 origin = at ? at->position : math::vec3{};
        u32 i = 0;
        for (const LootRoll& r : rolled) {
            ItemPickup3D p;
            p.item = c->items()[r.item].id;
            p.qty = r.qty;
            Trigger3D trig;
            trig.shape = TriggerShape3D::Sphere;
            trig.radius = drop->radius;
            f32 ox = 0, oz = 0;
            ring_offset(i++, drop->scatter, ox, oz);
            f.commands().spawn(LocalTransform{{origin.x + ox, origin.y, origin.z + oz}, math::quat::identity(), {1, 1, 1}}, p, trig);
        }
    }

    for (const OpenContainer& o : ev.read<OpenContainer>()) {  // chests
        if (o.container.is_null() || !w.alive(o.container) || o.opener.is_null() || !w.alive(o.opener)) continue;
        Container* box = w.try_get_mut<Container>(o.container);
        Inventory* from = w.try_get_mut<Inventory>(o.container);
        Inventory* to = w.try_get_mut<Inventory>(o.opener);
        if (!box || !from || !to || o.container == o.opener) continue;
        const asset::ItemCatalog* c = reg.resolve(from->catalog);
        const asset::ItemCatalog* cto = reg.resolve(to->catalog);
        if (!c || c != cto) continue;  // both sides must index the same catalog
        if (!box->filled && !box->table.empty()) {
            const asset::ItemCatalog* cbox = nullptr;
            u32 table = 0;
            if (!resolve_table(reg, box->catalog, box->table, cbox, table) || cbox != c) {
                MCE_LOG_WARN("container: table '{}' is not in the chest's catalog; nothing rolled", box->table.view());
                continue;  // not marked filled: a later open can still roll once the catalog is right
            }
            box->filled = 1;
            procgen::Rng rng = procgen::Rng::child(seed_of(box->seed, box->table, o.container), "loot", box->rolls);
            ++box->rolls;
            Array<LootRoll> rolled;
            u32 work = kMaxWork;
            roll_table(*c, table, rng, rolled, 0, work);
            for (const LootRoll& r : rolled) {
                const u32 put = inventory_add(*from, *c, r.item, r.qty);
                if (put < r.qty) MCE_LOG_WARN("container: chest inventory full, {} of '{}' lost", r.qty - put, c->items()[r.item].id.view());
            }
        } else if (!box->filled) {
            box->filled = 1;
        }
        u32 taken = 0, left = 0;
        for (u32 s = 0; s < kInventorySlots; ++s) {
            if (from->item[s] == 0 || from->qty[s] == 0 || from->item[s] - 1u >= c->items().size()) continue;
            const u32 item = from->item[s] - 1u;
            const u32 moved = inventory_add(*to, *c, item, from->qty[s]);
            from->qty[s] -= moved;
            if (from->qty[s] == 0) from->item[s] = 0;
            taken += moved;
            left += from->qty[s];
            if (moved > 0) ev.push(ItemAdded{o.opener, c->items()[item].id, moved, from->qty[s]});
        }
        ev.push(ContainerOpened{o.container, o.opener, taken, left});
    }
}

}  // namespace

bool roll_loot(const asset::ItemCatalog& c, u32 table, procgen::Rng& rng, Array<LootRoll>& out) {
    if (table >= c.tables().size()) return false;
    u32 work = kMaxWork;
    roll_table(c, table, rng, out, 0, work);
    return true;
}

void install_loot(World& world, asset::AssetSystem* assets) {
    install_inventory(world, assets);
    install_bodies3d(world, assets);
    if (world.try_resource<LootInstalled>()) return;
    world.set_resource(LootInstalled{});
    world.register_components<LootDrop, Container>();
    world.register_event<OpenContainer>(Phase::FixedUpdate_Pre);
    world.register_event<ContainerOpened>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_loot", &kit_loot, "kit_inventory");
}

}  // namespace mce::kit
