// Feeding inventory widgets in the stock player (M-INVENTORY step 4b; item_grid_feed.h).

#include "item_grid_feed.h"

#include <cstdio>
#include <cstring>

#include "mce/kit/inventory.h"

namespace mce::player {

namespace {

bool slot_index(StringView name, u32& out) {
    constexpr usize kPrefix = 5;  // "slot_"
    if (name.size() <= kPrefix || std::strncmp(name.data(), "slot_", kPrefix) != 0) return false;
    u32 n = 0;
    for (usize i = kPrefix; i < name.size(); ++i) {
        const char c = name.data()[i];
        if (c < '0' || c > '9' || n > 100) return false;
        n = n * 10u + static_cast<u32>(c - '0');
    }
    if (n < 1 || n > kit::kInventorySlots) return false;
    out = n - 1u;
    return true;
}

bool equip_index(StringView name, u32& out) {
    constexpr usize kPrefix = 6;  // "equip_"
    if (name.size() <= kPrefix || std::strncmp(name.data(), "equip_", kPrefix) != 0) return false;
    for (u32 i = 0; i < kit::kEquipSlots; ++i) {
        const char* tag = kit::equip_slot_tag(i);  // "equip.head"
        if (tag && std::strlen(tag + 6) == name.size() - kPrefix && std::strncmp(tag + 6, name.data() + kPrefix, name.size() - kPrefix) == 0) {
            out = i;
            return true;
        }
    }
    return false;
}

void set_if_changed(ui::Document& doc, ui::WidgetId w, const char* text) {
    if (doc.text(w) != StringView(text)) doc.set_text(w, text);
}

}  // namespace

void ItemGridFeed::bind(const ui::Document& doc) {
    slots_.clear();
    equips_.clear();
    weight_ = ui::WidgetId{};
    has_weight_ = false;
    Array<ui::WidgetId> stack;
    stack.push_back(doc.root());
    while (!stack.empty()) {
        const ui::WidgetId w = stack.back();
        stack.pop_back();
        u32 i = 0;
        const StringView name = doc.name(w);
        if (doc.kind(w) == ui::WidgetKind::Text) {
            if (slot_index(name, i)) slots_.push_back(Bound{w, i});
            else if (equip_index(name, i)) equips_.push_back(Bound{w, i});
            else if (name == StringView("inventory_weight")) {
                weight_ = w;
                has_weight_ = true;
            }
        }
        for (ui::WidgetId c = doc.first_child(w); doc.valid(c); c = doc.next_sibling(c)) stack.push_back(c);
    }
}

void ItemGridFeed::feed(World& world, ui::Document& doc, Entity self) {
    if (empty()) return;
    const kit::Inventory* inv = !self.is_null() && world.alive(self) ? world.try_get<kit::Inventory>(self) : nullptr;
    Entity owner = inv ? self : Entity{};
    if (!inv) {
        world.query<const kit::Inventory>().each([&](Entity e, const kit::Inventory& i) {
            if (!inv) {
                inv = &i;
                owner = e;
            }
        });
    }
    kit::ItemRegistry* reg = world.try_resource<kit::ItemRegistry>();
    const asset::ItemCatalog* cat = inv && reg ? reg->resolve(inv->catalog) : nullptr;
    char buf[160];
    for (const Bound& b : slots_) {
        if (!doc.valid(b.widget)) continue;
        buf[0] = '\0';
        if (cat && inv->item[b.index] != 0 && inv->item[b.index] - 1u < cat->items().size() && inv->qty[b.index] > 0) {
            const StringView name = cat->items()[inv->item[b.index] - 1u].name.view();
            if (inv->qty[b.index] > 1) std::snprintf(buf, sizeof buf, "%.*s x%u", static_cast<int>(name.size()), name.data(), inv->qty[b.index]);
            else std::snprintf(buf, sizeof buf, "%.*s", static_cast<int>(name.size()), name.data());
        }
        set_if_changed(doc, b.widget, buf);
    }
    if (!equips_.empty()) {
        const kit::Equipment* eq = !owner.is_null() ? world.try_get<kit::Equipment>(owner) : nullptr;
        // Equipment has its own catalog reference; the inventory's is what the item numbers index in practice, but resolve it
        // for the equipment so a differing one is shown as empty rather than as the wrong item.
        const asset::ItemCatalog* ecat = nullptr;
        if (eq && reg) ecat = reg->resolve(eq->catalog);
        for (const Bound& b : equips_) {
            if (!doc.valid(b.widget)) continue;
            buf[0] = '\0';
            if (ecat && eq->item[b.index] != 0 && eq->item[b.index] - 1u < ecat->items().size()) {
                const StringView name = ecat->items()[eq->item[b.index] - 1u].name.view();
                std::snprintf(buf, sizeof buf, "%.*s", static_cast<int>(name.size()), name.data());
            }
            set_if_changed(doc, b.widget, buf);
        }
    }
    if (has_weight_ && doc.valid(weight_)) {
        buf[0] = '\0';
        if (cat) {
            // Whole grams, printed as kilograms with one decimal: integer arithmetic, so the text is the same everywhere.
            const u64 w = kit::inventory_weight_milli(*inv, *cat);
            const u64 tenths = (w + 50) / 100;
            if (inv->max_weight_milli > 0) {
                const u64 mt = (static_cast<u64>(inv->max_weight_milli) + 50) / 100;
                std::snprintf(buf, sizeof buf, "%llu.%llu / %llu.%llu kg", static_cast<unsigned long long>(tenths / 10), static_cast<unsigned long long>(tenths % 10),
                              static_cast<unsigned long long>(mt / 10), static_cast<unsigned long long>(mt % 10));
            } else {
                std::snprintf(buf, sizeof buf, "%llu.%llu kg", static_cast<unsigned long long>(tenths / 10), static_cast<unsigned long long>(tenths % 10));
            }
        }
        set_if_changed(doc, weight_, buf);
    }
}

}  // namespace mce::player
