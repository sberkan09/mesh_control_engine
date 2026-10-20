#pragma once
// Inventories, equipment and pickups as data (M-INVENTORY 2). The items themselves are a project's .mcitems catalog
// (mce/asset/items_format.h); an Inventory holds slots of (item, quantity) and an Equipment one item per body slot.
// kit::install_stock installs it. Scenes without these components are unchanged.
//
//   Inventory     on an entity: kInventorySlots slots, an optional weight limit, the catalog its item numbers
//                 index. A slot holds `item` = catalog index + 1 (0: empty) and `qty` (never above the item's
//                 stack size). Everything is whole numbers; weights are thousandths, so no sum depends on float
//                 order and the same inputs give the same state hash.
//   Equipment     on the same entity: one catalog index + 1 per EquipSlot. Equipping moves ONE unit out of an
//                 inventory slot; the item must carry the tag "equip.<slot>" ("equip.weapon", "equip.head", ...).
//   ItemPickup3D  on an entity with a Trigger3D: when something with an Inventory enters, the pickup's item goes
//                 into that inventory; it is destroyed when all of it fits, otherwise `qty` shrinks to what is left.
//
// The rules (inventory_add, remove, count, move, split, weight) are plain functions of an Inventory and an
// ItemCatalog, so scripts, tools and tests run them without a World.
//   add     fills the stacks that already hold the item (slot order), then empty slots (slot order); it stops at the
//           weight limit (an item that weighs nothing is never limited by weight) and returns how many fit.
//   remove  takes from the first slot holding the item onward and returns how many it took.
//   move    onto an empty slot moves the stack; onto the same item merges up to the stack size and leaves the rest;
//           onto another item swaps.
//   split   moves part of a stack to the first empty slot.
// A slot whose item index is past the end of the catalog (the catalog changed under a save) is left alone: it counts
// for no weight and nothing adds to, removes from or merges with it.
//
// Input events (push them; processed in the system, FixedUpdate_Pre): GiveItem, TakeItem, MoveItem, SplitItem,
// EquipItem, UnequipItem, UseItem. Output events: ItemAdded (with what did not fit), ItemRemoved, ItemEquipped,
// ItemUnequipped, ItemUsed. UseItem on an item tagged "consumable" takes one unit and sends ItemUsed; any other
// item only sends ItemUsed (the game decides what it does).
//
// Saves hold catalog indices, not ids: edit a shipped catalog by appending items only (reordering or inserting changes what old
// saves hold). A slot with quantity 0 or an index past the catalog is treated as unusable, never as stock.
//
// System: "kit_inventory" (FixedUpdate_Pre, after "kit3d_health").

#include "mce/asset/handle.h"
#include "mce/asset/items_format.h"
#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

inline constexpr u32 kInventorySlots = 24;

enum class EquipSlot : u8 { Head, Body, Hands, Feet, Weapon, Offhand, Accessory1, Accessory2 };
MCE_ENUM(EquipSlot, Head, Body, Hands, Feet, Weapon, Offhand, Accessory1, Accessory2)
inline constexpr u32 kEquipSlots = 8;

struct Inventory {
    asset::AssetRef<asset::ItemCatalogChunk> catalog;
    u32 qty[kInventorySlots] = {};
    u16 item[kInventorySlots] = {};  // catalog index + 1, 0: empty
    u32 max_weight_milli = 0;        // 0: no limit
    u32 pad_ = 0;
};
MCE_COMPONENT(Inventory, (catalog, Save, Help{"the .mcitems catalog the item numbers index"}), (qty, Save, ReadOnly),
              (item, Save, ReadOnly), (max_weight_milli, Save, Range{0, 1000000000}, Help{"carry limit in thousandths of a kilogram; 0: none"}))

struct Equipment {
    asset::AssetRef<asset::ItemCatalogChunk> catalog;
    u16 item[kEquipSlots] = {};  // by EquipSlot: catalog index + 1, 0: empty
};
MCE_COMPONENT(Equipment, (catalog, Save, Help{"the .mcitems catalog the item numbers index (the inventory's)"}), (item, Save, ReadOnly))

struct ItemPickup3D {
    FixedString<64> item;  // the item's id in the collector's catalog
    u32 qty = 1;
};
MCE_COMPONENT(ItemPickup3D, (item, Save, Help{"the item id in the catalog of whoever collects it"}), (qty, Save, Range{1, 1000000}, Help{"how many"}))

// ---- input events
struct GiveItem {
    Entity owner;
    FixedString<64> item;
    u32 qty = 1;
};
struct TakeItem {
    Entity owner;
    FixedString<64> item;
    u32 qty = 1;
};
struct MoveItem {
    Entity owner;
    u32 from = 0;
    u32 to = 0;
};
struct SplitItem {
    Entity owner;
    u32 from = 0;
    u32 qty = 1;
};
struct EquipItem {
    Entity owner;
    u32 slot = 0;                        // the inventory slot to equip from
    u32 equip_slot = kEquipSlots;        // an EquipSlot, or kEquipSlots to pick the first one the item's "equip.*" tags allow
};
struct UnequipItem {
    Entity owner;
    u32 equip_slot = 0;
};
struct UseItem {
    Entity owner;
    u32 slot = 0;
};
MCE_EVENT(GiveItem, owner, item, qty)
MCE_EVENT(TakeItem, owner, item, qty)
MCE_EVENT(MoveItem, owner, from, to)
MCE_EVENT(SplitItem, owner, from, qty)
MCE_EVENT(EquipItem, owner, slot, equip_slot)
MCE_EVENT(UnequipItem, owner, equip_slot)
MCE_EVENT(UseItem, owner, slot)

// ---- output events (producer FixedUpdate_Pre)
struct ItemAdded {
    Entity owner;
    FixedString<64> item;
    u32 qty = 0;        // how many went in
    u32 remainder = 0;  // how many did not fit
};
struct ItemRemoved {
    Entity owner;
    FixedString<64> item;
    u32 qty = 0;
};
struct ItemEquipped {
    Entity owner;
    FixedString<64> item;
    u32 equip_slot = 0;
};
struct ItemUnequipped {
    Entity owner;
    FixedString<64> item;
    u32 equip_slot = 0;
};
struct ItemUsed {
    Entity owner;
    FixedString<64> item;
    u32 slot = 0;
    u32 consumed = 0;  // 1 when the unit was taken
};
MCE_EVENT(ItemAdded, owner, item, qty, remainder)
MCE_EVENT(ItemRemoved, owner, item, qty)
MCE_EVENT(ItemEquipped, owner, item, equip_slot)
MCE_EVENT(ItemUnequipped, owner, item, equip_slot)
MCE_EVENT(ItemUsed, owner, item, slot, consumed)

// ---- the rules
u32 inventory_add(Inventory& inv, const asset::ItemCatalog& c, u32 item, u32 qty);     // returns how many went in
u32 inventory_remove(Inventory& inv, const asset::ItemCatalog& c, u32 item, u32 qty);  // returns how many came out
u32 inventory_count(const Inventory& inv, u32 item);
u64 inventory_weight_milli(const Inventory& inv, const asset::ItemCatalog& c);
bool inventory_move(Inventory& inv, const asset::ItemCatalog& c, u32 from, u32 to);
bool inventory_split(Inventory& inv, const asset::ItemCatalog& c, u32 from, u32 qty);  // false: no empty slot, or qty is not below the stack

// Equip one unit from inventory slot `slot` into `equip_slot` (kEquipSlots: the first slot the item's tags allow). A unit
// already there goes back to the inventory first; false (nothing changes) when the item does not fit that slot or the
// old one has nowhere to go.
bool equipment_equip(Inventory& inv, Equipment& eq, const asset::ItemCatalog& c, u32 slot, u32 equip_slot, u32* used_slot = nullptr);
bool equipment_unequip(Inventory& inv, Equipment& eq, const asset::ItemCatalog& c, u32 equip_slot);  // false: empty slot or no room

const char* equip_slot_tag(u32 equip_slot);  // "equip.head", ... nullptr past the end

// The decoded catalogs the systems use, one per catalog asset (a resource). Tools and tests register one in code with
// define; otherwise it is loaded from the asset system and re-read on hot reload.
class ItemRegistry {
public:
    ItemRegistry() = default;
    explicit ItemRegistry(asset::AssetSystem* assets) : assets_(assets) {}
    void set_assets(asset::AssetSystem* assets) { assets_ = assets; }
    // The pointer is valid until the next resolve or define call.
    const asset::ItemCatalog* resolve(const asset::AssetRef<asset::ItemCatalogChunk>& ref);
    void define(asset::Guid guid, const asset::ItemCatalog& catalog);

private:
    struct Entry {
        asset::Guid guid;
        asset::AssetKey key = asset::kInvalidAssetKey;
        u32 generation = 0;
        bool defined = false;  // registered in code: never re-read
        asset::ItemCatalog catalog;
    };
    Array<Entry> entries_;
    Array<asset::Guid> warned_;
    asset::AssetSystem* assets_ = nullptr;
};

// Registers the components, events and the registry and adds the system (after install_combat3d, which it calls).
// Idempotent.
void install_inventory(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
