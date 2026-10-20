// mce_script (M-INVENTORY step 4): the `inventory` global, thin bindings over the kit's inventory components and
// events (mce/kit/inventory.h, loot.h). Like `mission` it keeps no state: changes go in as the kit's input events
// (GiveItem, TakeItem, EquipItem, ...) and take effect on the next tick; reads look straight at the Inventory.
// Slots are 1-based in Luau (1..24). A world without the kit pieces gets a named error, never a crash.
//
//   inventory.add(e, id, qty?)          queue GiveItem (qty 1..1000000, default 1)
//   inventory.remove(e, id, qty?)       queue TakeItem
//   inventory.count(e, id) -> number    units of the item across all slots (0 for an unknown id)
//   inventory.has(e, id, qty?) -> boolean
//   inventory.weight(e) -> number       carried weight in thousandths of a kilogram
//   inventory.slot(e, i) -> id, qty     the item in slot i (nil, 0 when empty)
//   inventory.equip(e, i, where?)       queue EquipItem from slot i; `where` is "head", "body", "hands", "feet", "weapon",
//                                       "offhand", "accessory1", "accessory2" (nil: the first the item's tags allow)
//   inventory.unequip(e, where)         queue UnequipItem
//   inventory.use(e, i)                 queue UseItem on slot i
//   inventory.drop(e, i, qty?)          takes qty (default the whole stack) out of slot i now and spawns an ItemPickup3D
//                                       1.5 m along +x of the owner; returns how many were dropped
//   inventory.open(container, opener)   queue OpenContainer
//
// Every function may raise (LUA_USE_LONGJMP): nothing with a destructor lives across a luaL_* call.

#include <cmath>
#include <cstring>

#include "api.h"
#include "mce/ecs/transform.h"
#include "mce/fixed_string.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/inventory.h"
#include "mce/kit/loot.h"

namespace mce::script::detail {

namespace {

constexpr f64 kMaxQty = 1000000.0;

u32 check_whole(lua_State* L, int idx, const char* fn, f64 lo, f64 hi, f64 def) {
    const f64 v = luaL_optnumber(L, idx, def);
    if (!(v >= lo && v <= hi && v == std::floor(v))) luaL_error(L, "%s: %g is not a whole number from %g to %g", fn, v, lo, hi);
    return static_cast<u32>(v);
}

kit::Inventory& check_inventory(lua_State* L, World& w, Entity e, const char* fn) {
    if (!w.alive(e)) luaL_error(L, "%s: the entity is not alive", fn);
    kit::Inventory* inv = w.try_get_mut<kit::Inventory>(e);
    if (!inv) luaL_error(L, "%s: the entity has no Inventory component", fn);
    return *inv;
}

void need_kit(lua_State* L, World& w, const char* fn) {
    if (!w.try_resource<kit::ItemRegistry>()) luaL_error(L, "%s: this world has no inventories (install_inventory)", fn);
}

const asset::ItemCatalog& check_catalog(lua_State* L, World& w, const kit::Inventory& inv, const char* fn) {
    need_kit(L, w, fn);
    const asset::ItemCatalog* c = w.resource<kit::ItemRegistry>().resolve(inv.catalog);
    if (!c) luaL_error(L, "%s: the inventory's catalog is not loaded", fn);
    return *c;
}

FixedString<64> check_id(lua_State* L, int idx, const char* fn) {
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    if (len == 0) luaL_error(L, "%s: the item id is empty", fn);
    if (len > FixedString<64>::capacity()) luaL_error(L, "%s: the item id is longer than %d characters", fn, static_cast<int>(FixedString<64>::capacity()));
    FixedString<64> id;
    id.assign(StringView(s, len));
    return id;
}

u32 check_slot(lua_State* L, int idx, const char* fn) { return check_whole(L, idx, fn, 1, kit::kInventorySlots, 1) - 1u; }

// "head" -> EquipSlot index; raises for an unknown name.
u32 check_where(lua_State* L, int idx, const char* fn) {
    const char* name = luaL_checkstring(L, idx);
    for (u32 i = 0; i < kit::kEquipSlots; ++i) {
        const char* tag = kit::equip_slot_tag(i);  // "equip.head"
        if (tag && std::strcmp(tag + 6, name) == 0) return i;
    }
    luaL_error(L, "%s: unknown equipment slot '%s' (head, body, hands, feet, weapon, offhand, accessory1, accessory2)", fn, name);
    return 0;
}

int l_add(lua_State* L) {
    Frame& f = frame_of(L);
    const Entity e = check_entity(L, 1);
    check_inventory(L, f.world(), e, "inventory.add");
    kit::GiveItem g;
    g.owner = e;
    g.item = check_id(L, 2, "inventory.add");
    g.qty = check_whole(L, 3, "inventory.add", 1, kMaxQty, 1);
    f.events().push(g);
    return 0;
}

int l_remove(lua_State* L) {
    Frame& f = frame_of(L);
    const Entity e = check_entity(L, 1);
    check_inventory(L, f.world(), e, "inventory.remove");
    kit::TakeItem t;
    t.owner = e;
    t.item = check_id(L, 2, "inventory.remove");
    t.qty = check_whole(L, 3, "inventory.remove", 1, kMaxQty, 1);
    f.events().push(t);
    return 0;
}

u32 count_of(lua_State* L, World& w, Entity e, const FixedString<64>& id, const char* fn) {
    const kit::Inventory& inv = check_inventory(L, w, e, fn);
    const asset::ItemCatalog& c = check_catalog(L, w, inv, fn);
    const u16 item = c.find_item(id.view());
    return item == asset::kNoItem ? 0u : kit::inventory_count(inv, item);
}

int l_count(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const FixedString<64> id = check_id(L, 2, "inventory.count");
    lua_pushnumber(L, static_cast<double>(count_of(L, w, e, id, "inventory.count")));
    return 1;
}

int l_has(lua_State* L) {
    World& w = world_of(L);
    const Entity e = check_entity(L, 1);
    const FixedString<64> id = check_id(L, 2, "inventory.has");
    const u32 want = check_whole(L, 3, "inventory.has", 1, kMaxQty, 1);
    lua_pushboolean(L, count_of(L, w, e, id, "inventory.has") >= want ? 1 : 0);
    return 1;
}

int l_weight(lua_State* L) {
    World& w = world_of(L);
    const kit::Inventory& inv = check_inventory(L, w, check_entity(L, 1), "inventory.weight");
    const asset::ItemCatalog& c = check_catalog(L, w, inv, "inventory.weight");
    lua_pushnumber(L, static_cast<double>(kit::inventory_weight_milli(inv, c)));
    return 1;
}

int l_slot(lua_State* L) {
    World& w = world_of(L);
    const kit::Inventory& inv = check_inventory(L, w, check_entity(L, 1), "inventory.slot");
    const u32 s = check_slot(L, 2, "inventory.slot");
    const asset::ItemCatalog& c = check_catalog(L, w, inv, "inventory.slot");
    if (inv.item[s] == 0 || inv.item[s] - 1u >= c.items().size()) {
        lua_pushnil(L);
        lua_pushnumber(L, 0);
        return 2;
    }
    const FixedString<64>& id = c.items()[inv.item[s] - 1u].id;
    const StringView sv = id.view();
    lua_pushlstring(L, sv.data(), sv.size());
    lua_pushnumber(L, static_cast<double>(inv.qty[s]));
    return 2;
}

int l_equip(lua_State* L) {
    Frame& f = frame_of(L);
    const Entity e = check_entity(L, 1);
    check_inventory(L, f.world(), e, "inventory.equip");
    kit::EquipItem q;
    q.owner = e;
    q.slot = check_slot(L, 2, "inventory.equip");
    q.equip_slot = lua_isnoneornil(L, 3) ? kit::kEquipSlots : check_where(L, 3, "inventory.equip");
    f.events().push(q);
    return 0;
}

int l_unequip(lua_State* L) {
    Frame& f = frame_of(L);
    const Entity e = check_entity(L, 1);
    check_inventory(L, f.world(), e, "inventory.unequip");
    kit::UnequipItem q;
    q.owner = e;
    q.equip_slot = check_where(L, 2, "inventory.unequip");
    f.events().push(q);
    return 0;
}

int l_use(lua_State* L) {
    Frame& f = frame_of(L);
    const Entity e = check_entity(L, 1);
    check_inventory(L, f.world(), e, "inventory.use");
    kit::UseItem q;
    q.owner = e;
    q.slot = check_slot(L, 2, "inventory.use");
    f.events().push(q);
    return 0;
}

int l_drop(lua_State* L) {
    Frame& f = frame_of(L);
    World& w = f.world();
    const Entity e = check_entity(L, 1);
    kit::Inventory& inv = check_inventory(L, w, e, "inventory.drop");
    const u32 s = check_slot(L, 2, "inventory.drop");
    const u32 want = check_whole(L, 3, "inventory.drop", 0, kMaxQty, 0);  // 0: the whole stack
    const asset::ItemCatalog& c = check_catalog(L, w, inv, "inventory.drop");
    if (inv.item[s] == 0 || inv.qty[s] == 0 || inv.item[s] - 1u >= c.items().size()) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const u32 item = inv.item[s] - 1u;
    const u32 n = want == 0 || want > inv.qty[s] ? inv.qty[s] : want;
    inv.qty[s] -= n;
    if (inv.qty[s] == 0) inv.item[s] = 0;
    kit::ItemPickup3D p;
    p.item = c.items()[item].id;
    p.qty = n;
    kit::Trigger3D trig;
    trig.shape = kit::TriggerShape3D::Sphere;
    trig.radius = 0.6f;
    const LocalTransform* at = w.try_get<LocalTransform>(e);
    const math::vec3 o = at ? at->position : math::vec3{};
    w.commands().spawn(LocalTransform{{o.x + 1.5f, o.y, o.z}, math::quat::identity(), {1, 1, 1}}, p, trig);
    lua_pushnumber(L, static_cast<double>(n));
    return 1;
}

int l_open(lua_State* L) {
    Frame& f = frame_of(L);
    World& w = f.world();
    const Entity box = check_entity(L, 1);
    const Entity who = check_entity(L, 2);
    if (!w.alive(box) || !w.try_get<kit::Container>(box)) luaL_error(L, "inventory.open: the first entity has no Container component");
    check_inventory(L, w, who, "inventory.open");
    if (!w.event_queue<kit::OpenContainer>()) luaL_error(L, "inventory.open: this world has no containers (install_loot)");
    f.events().push(kit::OpenContainer{box, who});
    return 0;
}

const luaL_Reg kInventoryLib[] = {
    {"add", l_add},       {"remove", l_remove}, {"count", l_count}, {"has", l_has},       {"weight", l_weight}, {"slot", l_slot},
    {"equip", l_equip},   {"unequip", l_unequip}, {"use", l_use},   {"drop", l_drop},     {"open", l_open},     {nullptr, nullptr},
};

}  // namespace

void register_inventory_api(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kInventoryLib);
    lua_setglobal(L, "inventory");
}

}  // namespace mce::script::detail
