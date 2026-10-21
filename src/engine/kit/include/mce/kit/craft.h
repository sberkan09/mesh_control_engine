#pragma once
// Crafting as data (M-CRAFT 2). The recipes are a project's .mcitems catalog (mce/asset/items_format.h); this adds the
// atomic rules, the events and the station component. kit::install_stock installs it.
//
//   craft_check / craft_apply   pure functions of an Inventory and an ItemCatalog. A craft is atomic: it takes every
//                               input and adds every output, or changes nothing. The room for the outputs is checked
//                               after the inputs are taken out (an emptied slot, or the weight of the inputs, makes
//                               room), against stacks, slots and the weight limit exactly as inventory_add does.
//                               Whole numbers only, so the same inputs give the same state hash.
//   CraftingStation             on an entity: `station` is the tag it offers ("workbench"). A recipe with a station tag
//                               needs a CraftingStation of that tag; a recipe without one is made by hand.
//
// Events (FixedUpdate_Pre): CraftItem in; ItemCrafted and CraftFailed out (the reason is a CraftResult; craft_result_name
// spells it). An instant craft (a recipe with time 0, or one made by hand) is applied in the tick the event is read.
// A timed craft at a station checks the recipe when it starts and queues it (one at a time; a second request while the
// station is busy fails with StationBusy); the inputs are taken and the outputs added when the time is up, again as one
// atomic step, so the inventory can be used meanwhile and a craft that no longer fits fails with its reason.
//
// Destroying a station drops its queued craft without an event (nothing was taken). The queued craft is saved with the
// station; the crafter is remapped by the save like any entity reference.
//
// System: "kit_craft" (FixedUpdate_Pre, after "kit_loot").

#include "mce/asset/items_format.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/kit/inventory.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

enum class CraftResult : u8 { Ok, NoRecipe, MissingInput, NoRoom, NoStation, StationBusy, NoInventory };

const char* craft_result_name(CraftResult r);  // "ok", "no_recipe", "missing_input", "no_room", "no_station", "station_busy", "no_inventory"

// Whether `recipe` (an index into the catalog) could be made from `inv` now. Never changes `inv`.
CraftResult craft_check(const Inventory& inv, const asset::ItemCatalog& c, u32 recipe);
// Makes it: Ok after taking the inputs and adding the outputs; anything else leaves `inv` exactly as it was.
CraftResult craft_apply(Inventory& inv, const asset::ItemCatalog& c, u32 recipe);

struct CraftingStation {
    FixedString<32> station;   // the tag it offers
    FixedString<64> recipe;    // engine-written: the queued recipe id, empty when idle
    Entity crafter;            // engine-written: whose inventory the queued craft uses
    u32 left_us = 0;           // engine-written: microseconds until the queued craft completes
};
MCE_COMPONENT(CraftingStation, (station, Save, Help{"the tag recipes name as their station, e.g. workbench"}), (recipe, Save, ReadOnly),
              (crafter, Save, ReadOnly), (left_us, Save, ReadOnly))

// ---- input event
struct CraftItem {
    Entity owner;               // whose Inventory
    FixedString<64> recipe;     // the recipe id in the owner's catalog
    Entity station;             // a CraftingStation entity, or null to craft by hand
};
MCE_EVENT(CraftItem, owner, recipe, station)

// ---- output events
struct ItemCrafted {
    Entity owner;
    FixedString<64> recipe;
    Entity station;
};
struct CraftFailed {
    Entity owner;
    FixedString<64> recipe;
    Entity station;
    u32 reason = 0;  // a CraftResult
};
MCE_EVENT(ItemCrafted, owner, recipe, station)
MCE_EVENT(CraftFailed, owner, recipe, station, reason)

// Registers the component and events and adds the system (after install_loot, which it calls). Idempotent.
void install_crafting(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
