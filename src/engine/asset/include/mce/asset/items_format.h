#pragma once
// CHNK_ITEMS: a project's item definitions and loot tables (M-INVENTORY 1). The .mcitems
// importer (cook/src/items_importer.cpp) writes it; the kit's inventory systems read it through ItemCatalog.
// Kept out of formats.h so the many TUs that include that header do not rebuild for the item kit.
//
//   {
//     "format": "mce-items", "version": 1, "guid": "<uuid>",
//     "items": [ { "id": "medkit", "name": "Medkit", "stack": 5, "weight": 0.4, "tags": ["consumable"],
//                  "stats": { "heal": 25 }, "icon": "guid:<texture>", "prefab": "guid:<prefab>" } ],
//     "tables": [ { "id": "chest_common", "rolls": [1, 3],
//                   "entries": [ { "item": "medkit", "weight": 5, "qty": [1, 2] },
//                                { "table": "chest_rare", "weight": 1 },
//                                { "item": "key", "guaranteed": true } ] } ]
//   }
//
// An item is named by its string id in game code and by its index here; quantities are whole numbers, weights are
// fixed-point thousandths (a kilogram is 1000), so no sum depends on float order. A loot table makes `rolls`
// weighted picks among its non-guaranteed entries (an entry that names a table picks from that table in turn) and
// always adds its guaranteed entries; tables may nest to kItemMaxTableDepth and may not loop.
//
// Chunk layout v1 (little-endian, no padding; every string is a u16 length and the bytes):
//   u32 item_count | u32 table_count
//   item:  str id | str name | u32 stack_max | u32 weight_milli | Guid icon (hi, lo as u64) | Guid prefab
//          | u8 stat_count | { str key | f32 value }* | u8 tag_count | str tag*
//   table: str id | u16 rolls_min | u16 rolls_max | u16 entry_count
//          | { u8 kind (0 item, 1 table) | u8 guaranteed | u16 ref | u32 weight | u32 qty_min | u32 qty_max }*

#include "mce/asset/guid.h"
#include "mce/containers/array.h"
#include "mce/fixed_string.h"
#include "mce/result.h"

namespace mce::asset {

inline constexpr u8 ITEM_CATALOG_VERSION = 1;  // CHNK_ITEMS: item definitions and loot tables (M-INVENTORY 1)

inline constexpr u32 kItemMaxItems = 4096;
inline constexpr u32 kItemMaxTables = 1024;
inline constexpr u32 kItemMaxEntries = 256;       // per table
inline constexpr u32 kItemMaxStats = 32;          // per item
inline constexpr u32 kItemMaxTags = 16;           // per item
inline constexpr u32 kItemMaxStack = 1000000;     // also the largest quantity
inline constexpr u32 kItemMaxWeightMilli = 1000000000u;
inline constexpr u32 kItemMaxEntryWeight = 1000000;
inline constexpr u32 kItemMaxRolls = 64;
inline constexpr u32 kItemMaxTableDepth = 8;      // tables naming tables

inline constexpr u16 kNoItem = 0xFFFF;

struct ItemStat {
    FixedString<32> key;
    f32 value = 0.0f;
};

struct ItemDef {
    FixedString<64> id;  // what game code names it by
    FixedString<64> name;  // for the player; a localization key when the game localizes
    u32 stack_max = 1;     // a stack of this item holds up to this many
    u32 weight_milli = 0;  // one unit, in thousandths
    Guid icon;             // a Texture, or null
    Guid prefab;           // a Prefab spawned when the item is dropped in the world, or null
    u32 first_stat = 0;    // into ItemCatalog::stats()
    u16 stat_count = 0;
    u32 first_tag = 0;     // into ItemCatalog::tags()
    u16 tag_count = 0;
};

enum class LootKind : u8 { Item, Table };

struct LootEntry {
    LootKind kind = LootKind::Item;
    bool guaranteed = false;  // always added (weight unused), outside the rolls
    u16 ref = 0;              // an item index, or a table index
    u32 weight = 1;           // chance relative to the table's other weighted entries
    u32 qty_min = 1;
    u32 qty_max = 1;
};

struct LootTable {
    FixedString<64> id;
    u16 rolls_min = 1;
    u16 rolls_max = 1;
    u32 first_entry = 0;  // into ItemCatalog::entries()
    u16 entry_count = 0;
};

class ItemCatalog {
public:
    explicit ItemCatalog(mem::IAllocator& a = mem::heap()) : items_(a), stats_(a), tags_(a), tables_(a), entries_(a) {}

    Span<const ItemDef> items() const { return items_.span(); }
    Span<const ItemStat> stats() const { return stats_.span(); }
    Span<const FixedString<32>> tags() const { return tags_.span(); }
    Span<const LootTable> tables() const { return tables_.span(); }
    Span<const LootEntry> entries() const { return entries_.span(); }

    // Index by id, kNoItem when absent.
    u16 find_item(StringView id) const;
    u16 find_table(StringView id) const;
    Span<const ItemStat> stats_of(const ItemDef& d) const { return Span<const ItemStat>(stats_.data() + d.first_stat, d.stat_count); }
    Span<const FixedString<32>> tags_of(const ItemDef& d) const { return Span<const FixedString<32>>(tags_.data() + d.first_tag, d.tag_count); }
    Span<const LootEntry> entries_of(const LootTable& t) const { return Span<const LootEntry>(entries_.data() + t.first_entry, t.entry_count); }
    bool has_tag(const ItemDef& d, StringView tag) const;
    // A named stat's value, `def` when the item does not have it.
    f32 stat(const ItemDef& d, StringView key, f32 def = 0.0f) const;

    // Code-side authoring (tests, samples). add_item copies `stats` and `tags`; false when the id is empty, taken or
    // a limit is passed. add_table copies `entries`; its references are checked by item_catalog_problem, since a
    // table may name one added later.
    bool add_item(const ItemDef& d, Span<const ItemStat> stats = {}, Span<const FixedString<32>> tags = {});
    bool add_table(const LootTable& t, Span<const LootEntry> entries);
    void clear();

private:
    Array<ItemDef> items_;
    Array<ItemStat> stats_;
    Array<FixedString<32>> tags_;
    Array<LootTable> tables_;
    Array<LootEntry> entries_;
};

// What is wrong with a catalog, nullptr when nothing: ids that are empty, duplicated or use characters outside
// letters, digits, '_', '-' and '.'; limits; non-finite stats; references out of range; quantity and roll ranges;
// a table that rolls with nothing to roll; a table loop or nesting past kItemMaxTableDepth. `index` is the item
// or table at fault (the item when the text starts with "item", the table when it starts with "table").
const char* item_catalog_problem(const ItemCatalog& c, u32* index = nullptr);

// The ItemCatalog chunk: written by the cook (encode), read by the runtime (decode, which bounds-checks every
// length and runs item_catalog_problem).
void encode_item_catalog(const ItemCatalog& c, Array<byte>& out);
Result<void> decode_item_catalog(Span<const byte> payload, ItemCatalog& out);

}  // namespace mce::asset
