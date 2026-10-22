#include "mce/kit/inventory.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/asset/asset_system.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/status.h"
#include "mce/log.h"

namespace mce::kit {
namespace {

struct InventoryInstalled {};

constexpr const char* kEquipTags[kEquipSlots] = {"equip.head",   "equip.body",    "equip.hands",      "equip.feet",
                                                  "equip.weapon", "equip.offhand", "equip.accessory1", "equip.accessory2"};

bool valid_item(const asset::ItemCatalog& c, u32 item) { return item < c.items().size(); }
bool slot_known(const Inventory& inv, const asset::ItemCatalog& c, u32 s) {
    return inv.item[s] != 0 && inv.qty[s] != 0 && valid_item(c, inv.item[s] - 1u);
}

}  // namespace

const char* equip_slot_tag(u32 equip_slot) { return equip_slot < kEquipSlots ? kEquipTags[equip_slot] : nullptr; }

u32 inventory_count(const Inventory& inv, u32 item) {
    u64 n = 0;
    for (u32 s = 0; s < kInventorySlots; ++s)
        if (inv.item[s] == item + 1u) n += inv.qty[s];
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<u32>(n);
}

u64 inventory_weight_milli(const Inventory& inv, const asset::ItemCatalog& c) {
    u64 w = 0;
    for (u32 s = 0; s < kInventorySlots; ++s)
        if (slot_known(inv, c, s)) w += u64(c.items()[inv.item[s] - 1u].weight_milli) * (inv.qty[s] < asset::kItemMaxStack ? inv.qty[s] : asset::kItemMaxStack);  // a hostile save cannot wrap the sum
    return w;
}

u32 inventory_add(Inventory& inv, const asset::ItemCatalog& c, u32 item, u32 qty) {
    if (!valid_item(c, item) || qty == 0) return 0;
    const asset::ItemDef& d = c.items()[item];
    u32 want = qty;
    if (inv.max_weight_milli != 0 && d.weight_milli != 0) {
        const u64 now = inventory_weight_milli(inv, c);
        const u64 room = now >= inv.max_weight_milli ? 0 : (inv.max_weight_milli - now) / d.weight_milli;
        if (room < want) want = static_cast<u32>(room);
    }
    u32 added = 0;
    for (u32 s = 0; s < kInventorySlots && added < want; ++s) {  // the stacks already holding it
        if (inv.item[s] != item + 1u || inv.qty[s] >= d.stack_max) continue;
        const u32 take = d.stack_max - inv.qty[s] < want - added ? d.stack_max - inv.qty[s] : want - added;
        inv.qty[s] += take;
        added += take;
    }
    for (u32 s = 0; s < kInventorySlots && added < want; ++s) {  // then empty slots
        if (inv.item[s] != 0) continue;
        const u32 take = d.stack_max < want - added ? d.stack_max : want - added;
        inv.item[s] = static_cast<u16>(item + 1u);
        inv.qty[s] = take;
        added += take;
    }
    return added;
}

u32 inventory_remove(Inventory& inv, const asset::ItemCatalog& c, u32 item, u32 qty) {
    if (!valid_item(c, item) || qty == 0) return 0;
    u32 removed = 0;
    for (u32 s = 0; s < kInventorySlots && removed < qty; ++s) {
        if (inv.item[s] != item + 1u) continue;
        const u32 take = inv.qty[s] < qty - removed ? inv.qty[s] : qty - removed;
        inv.qty[s] -= take;
        removed += take;
        if (inv.qty[s] == 0) inv.item[s] = 0;
    }
    return removed;
}

bool inventory_move(Inventory& inv, const asset::ItemCatalog& c, u32 from, u32 to) {
    if (from >= kInventorySlots || to >= kInventorySlots || from == to || inv.item[from] == 0) return false;
    if (inv.item[to] == 0) {
        inv.item[to] = inv.item[from];
        inv.qty[to] = inv.qty[from];
        inv.item[from] = 0;
        inv.qty[from] = 0;
        return true;
    }
    if (inv.item[to] == inv.item[from] && slot_known(inv, c, to)) {
        const u32 stack = c.items()[inv.item[to] - 1u].stack_max;
        const u32 room = inv.qty[to] >= stack ? 0 : stack - inv.qty[to];
        const u32 take = room < inv.qty[from] ? room : inv.qty[from];
        if (take == 0) return false;  // both full: nothing to merge, and swapping equals changes nothing
        inv.qty[to] += take;
        inv.qty[from] -= take;
        if (inv.qty[from] == 0) inv.item[from] = 0;
        return true;
    }
    const u16 i = inv.item[to];
    const u32 q = inv.qty[to];
    inv.item[to] = inv.item[from];
    inv.qty[to] = inv.qty[from];
    inv.item[from] = i;
    inv.qty[from] = q;
    return true;
}

bool inventory_split(Inventory& inv, const asset::ItemCatalog& c, u32 from, u32 qty) {
    (void)c;
    if (from >= kInventorySlots || inv.item[from] == 0 || qty == 0 || qty >= inv.qty[from]) return false;
    for (u32 s = 0; s < kInventorySlots; ++s) {
        if (inv.item[s] != 0) continue;
        inv.item[s] = inv.item[from];
        inv.qty[s] = qty;
        inv.qty[from] -= qty;
        return true;
    }
    return false;
}

bool equipment_equip(Inventory& inv, Equipment& eq, const asset::ItemCatalog& c, u32 slot, u32 equip_slot, u32* used_slot) {
    if (slot >= kInventorySlots || !slot_known(inv, c, slot)) return false;
    const u32 item = inv.item[slot] - 1u;
    const asset::ItemDef& d = c.items()[item];
    u32 target = equip_slot;
    if (target == kEquipSlots) {
        for (u32 e = 0; e < kEquipSlots; ++e) {  // the first matching slot, an empty one before an occupied one
            if (!c.has_tag(d, kEquipTags[e])) continue;
            if (target == kEquipSlots || (eq.item[target] != 0 && eq.item[e] == 0)) target = e;
        }
    }
    if (target >= kEquipSlots || !c.has_tag(d, kEquipTags[target])) return false;
    // The old unit goes back first; if it has no room the swap does not happen. Work on copies so a refusal changes nothing.
    Inventory trial = inv;
    if (eq.item[target] != 0 && valid_item(c, eq.item[target] - 1u)) {  // an unknown old item (the catalog shrank) is dropped
        trial.qty[slot] -= 1;
        if (trial.qty[slot] == 0) trial.item[slot] = 0;
        if (inventory_add(trial, c, eq.item[target] - 1u, 1) != 1) return false;
    } else {
        trial.qty[slot] -= 1;
        if (trial.qty[slot] == 0) trial.item[slot] = 0;
    }
    inv = trial;
    eq.item[target] = static_cast<u16>(item + 1u);
    if (used_slot) *used_slot = target;
    return true;
}

bool equipment_unequip(Inventory& inv, Equipment& eq, const asset::ItemCatalog& c, u32 equip_slot) {
    if (equip_slot >= kEquipSlots || eq.item[equip_slot] == 0) return false;
    if (!valid_item(c, eq.item[equip_slot] - 1u)) {  // an item the catalog no longer has: clear the slot
        eq.item[equip_slot] = 0;
        return true;
    }
    if (inventory_add(inv, c, eq.item[equip_slot] - 1u, 1) != 1) return false;
    eq.item[equip_slot] = 0;
    return true;
}

// ---- registry

void ItemRegistry::define(asset::Guid guid, const asset::ItemCatalog& catalog) {
    u32 index = 0;
    if (const char* problem = asset::item_catalog_problem(catalog, &index)) {
        MCE_LOG_WARN("kit: item catalog {} not defined: {}", asset::to_string(guid).view(), problem);
        return;
    }
    for (Entry& e : entries_)
        if (e.guid == guid) {
            e.catalog = catalog;
            e.defined = true;
            return;
        }
    Entry e;
    e.guid = guid;
    e.defined = true;
    e.catalog = catalog;
    entries_.push_back(static_cast<Entry&&>(e));
}

const asset::ItemCatalog* ItemRegistry::resolve(const asset::AssetRef<asset::ItemCatalogChunk>& ref) {
    const asset::Guid guid = ref.guid;
    if (guid == asset::Guid{}) return nullptr;
    Entry* cached = nullptr;
    for (Entry& e : entries_)
        if (e.guid == guid) cached = &e;
    if (cached && (cached->defined || !assets_ || cached->key == asset::kInvalidAssetKey || assets_->generation(cached->key) == cached->generation))
        return &cached->catalog;
    if (!assets_) return nullptr;
    const asset::AssetKey key = assets_->find(guid);
    if (key != asset::kInvalidAssetKey) {
        if (Result<asset::AssetHandle<asset::ItemCatalogChunk>> h = assets_->load_sync<asset::ItemCatalogChunk>(key)) {
            asset::ItemCatalog fresh;
            const bool ok = static_cast<bool>(asset::decode_item_catalog(assets_->resolve(h.value()), fresh));
            const u32 gen = h.value().generation;
            assets_->release(asset::AnyHandle(h.value()));
            if (ok) {
                if (!cached) {
                    entries_.push_back(Entry());
                    cached = &entries_.back();
                    cached->guid = guid;
                }
                cached->catalog = static_cast<asset::ItemCatalog&&>(fresh);
                cached->key = key;
                cached->generation = gen;
                return &cached->catalog;
            }
            if (cached) {  // a reload that does not decode keeps the catalog in use, and is not retried until the next reload
                cached->generation = gen;
                return &cached->catalog;
            }
        }
    }
    bool warned = false;
    for (const asset::Guid& g : warned_) warned |= g == guid;
    if (!warned) {
        MCE_LOG_WARN("kit: item catalog {} cannot be loaded; its inventories stay as they are", asset::to_string(guid).view());
        warned_.push_back(guid);
    }
    return nullptr;
}

// ---- system

namespace {

void apply_equipment_stats(World& w, ItemRegistry& reg);

Inventory* inventory_of(World& w, Entity e) { return e.is_null() || !w.alive(e) ? nullptr : w.try_get_mut<Inventory>(e); }

void kit_inventory(Frame& f) {
    World& w = f.world();
    ItemRegistry& reg = w.resource<ItemRegistry>();
    EventAccess ev = f.events();

    // Pickups: whatever enters a pickup's trigger and has an inventory takes it.
    for (const TriggerEnter3D& t : ev.read<TriggerEnter3D>()) {
        if (t.trigger.is_null() || !w.alive(t.trigger)) continue;
        ItemPickup3D* p = w.try_get_mut<ItemPickup3D>(t.trigger);
        Inventory* inv = inventory_of(w, t.other);
        if (!p || !inv || p->qty == 0) continue;
        const asset::ItemCatalog* c = reg.resolve(inv->catalog);
        if (!c) continue;
        const u16 item = c->find_item(p->item.view());
        if (item == asset::kNoItem) continue;
        const u32 added = inventory_add(*inv, *c, item, p->qty);
        if (added == 0) continue;
        p->qty -= added;
        ev.push(ItemAdded{t.other, p->item, added, p->qty});
        if (p->qty == 0) f.commands().destroy(t.trigger);
    }

    for (const GiveItem& g : ev.read<GiveItem>()) {
        Inventory* inv = inventory_of(w, g.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (!c) continue;
        const u16 item = c->find_item(g.item.view());
        if (item == asset::kNoItem) continue;
        const u32 added = inventory_add(*inv, *c, item, g.qty);
        ev.push(ItemAdded{g.owner, g.item, added, g.qty - added});
    }
    for (const TakeItem& t : ev.read<TakeItem>()) {
        Inventory* inv = inventory_of(w, t.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (!c) continue;
        const u16 item = c->find_item(t.item.view());
        if (item == asset::kNoItem) continue;
        const u32 removed = inventory_remove(*inv, *c, item, t.qty);
        if (removed) ev.push(ItemRemoved{t.owner, t.item, removed});
    }
    for (const MoveItem& m : ev.read<MoveItem>()) {
        Inventory* inv = inventory_of(w, m.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (c) inventory_move(*inv, *c, m.from, m.to);
    }
    for (const SplitItem& s : ev.read<SplitItem>()) {
        Inventory* inv = inventory_of(w, s.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (c) inventory_split(*inv, *c, s.from, s.qty);
    }
    for (const EquipItem& q : ev.read<EquipItem>()) {
        Inventory* inv = inventory_of(w, q.owner);
        Equipment* eq = q.owner.is_null() || !w.alive(q.owner) ? nullptr : w.try_get_mut<Equipment>(q.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (!c || !eq || q.slot >= kInventorySlots || inv->item[q.slot] == 0) continue;
        const u16 item = inv->item[q.slot];
        u32 used = 0;
        if (equipment_equip(*inv, *eq, *c, q.slot, q.equip_slot, &used)) ev.push(ItemEquipped{q.owner, c->items()[item - 1u].id, used});
    }
    for (const UnequipItem& q : ev.read<UnequipItem>()) {
        Inventory* inv = inventory_of(w, q.owner);
        Equipment* eq = q.owner.is_null() || !w.alive(q.owner) ? nullptr : w.try_get_mut<Equipment>(q.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (!c || !eq || q.equip_slot >= kEquipSlots || eq->item[q.equip_slot] == 0) continue;
        const u16 item = eq->item[q.equip_slot];
        const bool known = valid_item(*c, item - 1u);
        if (equipment_unequip(*inv, *eq, *c, q.equip_slot) && known) ev.push(ItemUnequipped{q.owner, c->items()[item - 1u].id, q.equip_slot});
    }
    apply_equipment_stats(w, reg);  // heals are judged against the max the worn items give right now
    for (const UseItem& u : ev.read<UseItem>()) {
        Inventory* inv = inventory_of(w, u.owner);
        const asset::ItemCatalog* c = inv ? reg.resolve(inv->catalog) : nullptr;
        if (!c || u.slot >= kInventorySlots || !slot_known(*inv, *c, u.slot)) continue;
        const u32 item = inv->item[u.slot] - 1u;
        const asset::ItemDef& d = c->items()[item];
        u32 consumed = 0;
        if (c->has_tag(d, "consumable")) {
            bool refused = false;
            f32 healed = 0.0f;
            // cooldown (M-ITEMCD): a `cooldown` stat (seconds) keeps the same item from being used again until a Cooldown effect named
            // "cd:<item>" ends; it needs StatusEffects on the user and is skipped without it
            const f32 cooldown = c->stat(d, "cooldown", 0.0f);
            FixedString<32> cd_id;
            cd_id.assign("cd:");
            cd_id.append(d.id.view());
            const StatusEffects* cds = cooldown > 0.0f && !u.owner.is_null() && w.alive(u.owner) ? w.try_get<StatusEffects>(u.owner) : nullptr;
            const bool cd_on = cds && w.event_queue<ApplyStatus>() != nullptr;
            if (cd_on && status_find(*cds, status_key(cd_id.view())) >= 0) refused = true;  // still cooling down: not wasted
            const f32 heal = c->stat(d, "heal", 0.0f);
            Health3D* hp = heal > 0.0f && !u.owner.is_null() && w.alive(u.owner) ? w.try_get_mut<Health3D>(u.owner) : nullptr;
            if (hp && !refused) {
                const f32 room = hp->max - hp->current;
                if (hp->dead || !(room > 0.0f)) {
                    refused = true;  // a drink at full health (or on a corpse) is not wasted
                } else {
                    healed = heal < room ? heal : room;
                    hp->current += healed;
                }
            }
            // timed effects (M-STATUS 2): `regen` hp/s for `regen_seconds`, `armor_boost` for `boost_seconds`
            const f32 regen = c->stat(d, "regen", 0.0f), regen_s = c->stat(d, "regen_seconds", 0.0f);
            const f32 boost = c->stat(d, "armor_boost", 0.0f), boost_s = c->stat(d, "boost_seconds", 0.0f);
            const bool fx_regen = regen > 0.0f && regen_s > 0.0f, fx_boost = boost != 0.0f && boost_s > 0.0f;
            const bool wants_fx = (fx_regen || fx_boost) && w.event_queue<ApplyStatus>() != nullptr;
            bool can_fx = false;  // the user needs Health3D (alive) and StatusEffects to carry it
            if (wants_fx && !u.owner.is_null() && w.alive(u.owner)) {
                const Health3D* th = w.try_get<Health3D>(u.owner);
                can_fx = th && !th->dead && w.try_get<StatusEffects>(u.owner) != nullptr;
            }
            if (wants_fx && heal <= 0.0f && !can_fx) refused = true;  // nothing to affect: not wasted
            if (!refused) {
                if (can_fx) {
                    ApplyStatus a;
                    a.target = u.owner;
                    a.source = u.owner;
                    if (fx_regen) {
                        a.id.assign(d.id.view());
                        a.kind = static_cast<u32>(StatusKind::Regen);
                        a.amount = regen;
                        a.seconds = regen_s;
                        ev.push(a);
                    }
                    if (fx_boost) {
                        a.id.assign(d.id.view());
                        a.id.append(":armor");
                        a.kind = static_cast<u32>(StatusKind::Armor);
                        a.amount = boost;
                        a.seconds = boost_s;
                        ev.push(a);
                    }
                }
                if (cd_on) {
                    ApplyStatus a;
                    a.target = u.owner;
                    a.source = u.owner;
                    a.id = cd_id;
                    a.kind = static_cast<u32>(StatusKind::Cooldown);
                    a.amount = 1.0f;
                    a.seconds = cooldown < kStatusMaxSeconds ? (cooldown > 0.01f ? cooldown : 0.01f) : kStatusMaxSeconds;
                    ev.push(a);
                }
                inv->qty[u.slot] -= 1;
                if (inv->qty[u.slot] == 0) inv->item[u.slot] = 0;
                consumed = 1;
                if (healed > 0.0f) ev.push(ItemHealed{u.owner, d.id, healed});
            }
        }
        ev.push(ItemUsed{u.owner, d.id, u.slot, consumed});
    }
    apply_equipment_stats(w, reg);
}

// Brings Health3D in line with what the worn items' stats say, applying only the change since last tick.
void apply_equipment_stats(World& w, ItemRegistry& reg) {
    // M-WEAPONSTAT: worn items' "damage" stats add to a Weapon3D on the same entity, as a remembered change like armor below
    w.query<Equipment, Weapon3D>().each([&](Entity, Equipment& eq, Weapon3D& wp) {
        f32 damage = 0.0f;
        const asset::ItemCatalog* c = reg.resolve(eq.catalog);
        if (!c && eq.damage_applied != 0.0f) return;  // catalog not loaded: keep what is applied
        if (c) {
            for (u32 s = 0; s < kEquipSlots; ++s) {
                if (eq.item[s] == 0 || !valid_item(*c, eq.item[s] - 1u)) continue;
                damage += c->stat(c->items()[eq.item[s] - 1u], "damage", 0.0f);
            }
        }
        if (!std::isfinite(damage) || (damage == 0.0f && eq.damage_applied == 0.0f)) return;
        const f32 now = wp.damage + (damage - eq.damage_applied);
        const f32 next = now < 0.0f ? 0.0f : now;
        eq.damage_applied += next - wp.damage;  // what really changed, so the clamp at 0 is not remembered as added
        wp.damage = next;
    });
    w.query<Equipment, Health3D>().each([&](Entity, Equipment& eq, Health3D& hp) {
        f32 armor = 0.0f;
        f32 max_health = 0.0f;
        bool any = false;
        const asset::ItemCatalog* c = reg.resolve(eq.catalog);
        if (!c && (eq.armor_applied != 0.0f || eq.max_applied != 0.0f)) return;  // catalog not loaded: keep what is applied
        if (c) {
            for (u32 s = 0; s < kEquipSlots; ++s) {
                if (eq.item[s] == 0 || !valid_item(*c, eq.item[s] - 1u)) continue;
                const asset::ItemDef& d = c->items()[eq.item[s] - 1u];
                armor += c->stat(d, "armor", 0.0f);
                max_health += c->stat(d, "max_health", 0.0f);
                any = true;
            }
        }
        if (!std::isfinite(armor) || !std::isfinite(max_health)) return;  // a catalog built in code with a hostile stat
        if (!any && eq.armor_applied == 0.0f && eq.max_applied == 0.0f) return;
        const f32 armor_now = hp.armor + (armor - eq.armor_applied);
        const f32 armor_new = armor_now < 0.0f ? 0.0f : (armor_now > 1.0f ? 1.0f : armor_now);
        eq.armor_applied += armor_new - hp.armor;  // what actually changed, so a clamp is not remembered as added
        hp.armor = armor_new;
        const f32 max_now = hp.max + (max_health - eq.max_applied);
        const f32 max_new = max_now < 1.0f ? 1.0f : max_now;
        eq.max_applied += max_new - hp.max;
        hp.max = max_new;
        if (hp.current > hp.max) hp.current = hp.max;
    });
}

}  // namespace

void install_inventory(World& world, asset::AssetSystem* assets) {
    install_combat3d(world, assets);
    if (ItemRegistry* r = world.try_resource<ItemRegistry>()) {
        r->set_assets(assets);
        return;
    }
    world.set_resource(ItemRegistry(assets));
    world.register_components<Inventory, Equipment, ItemPickup3D>();
    world.register_event<GiveItem>(Phase::FixedUpdate_Pre);
    world.register_event<TakeItem>(Phase::FixedUpdate_Pre);
    world.register_event<MoveItem>(Phase::FixedUpdate_Pre);
    world.register_event<SplitItem>(Phase::FixedUpdate_Pre);
    world.register_event<EquipItem>(Phase::FixedUpdate_Pre);
    world.register_event<UnequipItem>(Phase::FixedUpdate_Pre);
    world.register_event<UseItem>(Phase::FixedUpdate_Pre);
    world.register_event<ItemAdded>(Phase::FixedUpdate_Pre);
    world.register_event<ItemRemoved>(Phase::FixedUpdate_Pre);
    world.register_event<ItemEquipped>(Phase::FixedUpdate_Pre);
    world.register_event<ItemUnequipped>(Phase::FixedUpdate_Pre);
    world.register_event<ItemUsed>(Phase::FixedUpdate_Pre);
    world.register_event<ItemHealed>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_inventory", &kit_inventory, "kit3d_health");
}

}  // namespace mce::kit
