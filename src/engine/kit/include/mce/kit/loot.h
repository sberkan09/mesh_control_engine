#pragma once
// Loot tables rolled on a seeded stream (M-INVENTORY 3). The tables themselves are a project's .mcitems catalog
// (mce/asset/items_format.h: weighted entries, nested tables, guaranteed entries, quantity and roll ranges); this adds
// the roller and the two components that use it. kit::install_stock installs it.
//
//   roll_loot   a pure function of (catalog, table, Rng): the same table and the same stream give the same list on
//               every machine. A table does its guaranteed entries first (in entry order), then rolls between
//               rolls_min and rolls_max times, each roll picking one weighted entry (cumulative integer weights, a
//               64-bit draw: no float anywhere). An entry that names a table rolls that table. The result merges
//               equal items in first-seen order and saturates a quantity at 4,294,967,295.
//   LootDrop    on an entity with Health3D: when it dies (Died3D), the table is rolled and every result is spawned as
//               an ItemPickup3D with a sphere Trigger3D around the corpse, on a fixed ring so a pile never overlaps.
//   Container   a chest. On OpenContainer the first open rolls the table into the container's own Inventory (put one
//               on the entity; pre-filled slots stay), then the opener takes as much as fits; the rest stays and a
//               later open takes more. ContainerOpened says how much moved and how much is left.
//
// Seeds: a LootDrop or Container with seed 0 uses the hash of its table id and its entity index; a non-zero seed is
// used as given (set it for loot that must be the same on every run and every load). `rolls` counts the draws taken
// so far and is saved, so a game that loads and carries on rolls what it would have rolled.

#include "mce/asset/handle.h"
#include "mce/asset/items_format.h"
#include "mce/containers/array.h"
#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/kit/procgen.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::asset {
class AssetSystem;
}

namespace mce::kit {

struct LootRoll {
    u16 item = 0;  // catalog index
    u32 qty = 0;
};

// Rolls `table` (an index into the catalog) with `rng`, appending merged results to `out`. False (nothing appended)
// when the table index is out of range.
bool roll_loot(const asset::ItemCatalog& c, u32 table, procgen::Rng& rng, Array<LootRoll>& out);

struct LootDrop {
    asset::AssetRef<asset::ItemCatalogChunk> catalog;
    FixedString<64> table;  // the table's id in the catalog
    u64 seed = 0;           // 0: derived from the table id and the entity
    u32 rolls = 0;          // runtime, saved: how many times this has been rolled
    u8 dropped = 0;         // runtime, saved: the drop already happened
    u8 pad_[3] = {};
    f32 radius = 1.2f;      // pickup trigger radius, metres
    f32 scatter = 0.9f;     // ring radius the pickups spread on, metres
};
MCE_COMPONENT(LootDrop, (catalog, Save, Help{"the .mcitems catalog the table is in"}), (table, Save, Help{"the loot table id"}),
              (seed, Save, Help{"0: derived; set it for loot that must not change between runs"}), (rolls, Save, ReadOnly),
              (dropped, Save, ReadOnly), (radius, Save, Range{0.05f, 100}), (scatter, Save, Range{0, 100}))

struct Container {
    asset::AssetRef<asset::ItemCatalogChunk> catalog;
    FixedString<64> table;  // empty: nothing to roll, the Inventory is all there is
    u64 seed = 0;
    u32 rolls = 0;          // runtime, saved
    u8 filled = 0;          // runtime, saved: the table was rolled into the Inventory
    u8 pad_[3] = {};
};
MCE_COMPONENT(Container, (catalog, Save, Help{"the .mcitems catalog the table is in"}), (table, Save, Help{"the loot table id; empty for a fixed chest"}),
              (seed, Save), (rolls, Save, ReadOnly), (filled, Save, ReadOnly))

struct OpenContainer {  // input
    Entity container;
    Entity opener;
};
struct ContainerOpened {  // output
    Entity container;
    Entity opener;
    u32 taken = 0;  // items that moved to the opener
    u32 left = 0;   // items still in the container
};
MCE_EVENT(OpenContainer, container, opener)
MCE_EVENT(ContainerOpened, container, opener, taken, left)

// Registers the components and events and adds the systems (after install_inventory and install_bodies3d, which it calls).
// Idempotent.
void install_loot(World& world, asset::AssetSystem* assets);

}  // namespace mce::kit
