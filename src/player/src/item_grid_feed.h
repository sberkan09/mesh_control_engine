#pragma once
// Feeds a UI document's inventory widgets from the world (M-INVENTORY step 4b). No new widget kind and no chunk
// change: a document shows an inventory with Text widgets that carry the right names, anywhere in the tree.
//
//   slot_1 .. slot_24      slot N of the entity's Inventory: "Potion x5", "Sword", or "" when it is empty
//   equip_<where>          the Equipment slot (equip_head, equip_body, equip_hands, equip_feet, equip_weapon,
//                          equip_offhand, equip_accessory1, equip_accessory2): the item's name, or ""
//   inventory_weight       "12.5 / 20 kg" with a weight limit, "12.5 kg" without
//
// The entity is the screen's own when it has an Inventory, else the first entity in the world that has one (the
// player's). Text is set only when it changed. A document without such widgets costs nothing: bind() finds them once
// when the document is built, and feed() returns at once when there are none. A localized game can set the catalog's
// item names to localization keys and map the Text through its own table; this feed shows the catalog's names as they are.

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/ui/ui.h"

namespace mce::player {

class ItemGridFeed {
public:
    // Finds the document's widgets by name (call once after it is built).
    void bind(const ui::Document& doc);
    bool empty() const { return slots_.empty() && equips_.empty() && !has_weight_; }
    // Writes the texts for `self`'s Inventory and Equipment (or the first Inventory in the world).
    void feed(World& world, ui::Document& doc, Entity self);

private:
    struct Bound {
        ui::WidgetId widget;
        u32 index = 0;
    };
    Array<Bound> slots_;   // index: inventory slot
    Array<Bound> equips_;  // index: EquipSlot
    ui::WidgetId weight_;
    bool has_weight_ = false;
};

}  // namespace mce::player
