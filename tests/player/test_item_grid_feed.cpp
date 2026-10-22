// M-INVENTORY 4b: the stock player fills a document's inventory Text widgets (slot_N, equip_<where>, inventory_weight)
// from the world. CPU only.

#include <doctest/doctest.h>

#include "item_grid_feed.h"
#include "mce/ecs/world.h"
#include "mce/kit/inventory.h"
#include "mce/kit/status.h"
#include "mce/render/sprite_batch.h"

using namespace mce;

namespace {

const asset::Guid kCat{7, 8};

struct SkinFixture {
    render::FontMetrics metrics;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    render::SpriteSets sets;
    render::Font font;
    ui::Skin skin;
    SkinFixture() {
        ui::builtin_font(metrics, glyphs, atlas);
        render::SpriteSetDesc fd;
        fd.coverage = true;
        font.face = render::FontFace(metrics, glyphs.span(), {});
        font.set = sets.create(fd);
        const render::SpriteRegion white{0, 0.25f, 0.25f, 0.75f, 0.75f, 1.0f, 1.0f};
        render::SpriteSetDesc wd;
        wd.regions = Span<const render::SpriteRegion>(&white, 1);
        skin.set = sets.create(wd);
        skin.white_region = 0;
        skin.font = &font;
    }
};

asset::ItemCatalog make_catalog() {
    asset::ItemCatalog c;
    auto item = [&](const char* id, const char* name, u32 stack, u32 weight, const char* tag) {
        asset::ItemDef d;
        d.id = id;
        d.name = name;
        d.stack_max = stack;
        d.weight_milli = weight;
        Array<FixedString<32>> t;
        if (tag) t.push_back(FixedString<32>(tag));
        REQUIRE(c.add_item(d, {}, Span<const FixedString<32>>(t.data(), t.size())));
    };
    item("potion", "Potion", 5, 500, nullptr);
    item("sword", "Iron Sword", 1, 3000, "equip.weapon");
    return c;
}

}  // namespace

TEST_CASE("item grid feed: slots, equipment and weight show as text") {
    SkinFixture f;
    ui::Document doc;
    doc.set_skin(f.skin);
    const ui::WidgetId s1 = doc.add(doc.root(), ui::WidgetKind::Text, "slot_1");
    const ui::WidgetId s2 = doc.add(doc.root(), ui::WidgetKind::Text, "slot_2");
    const ui::WidgetId s3 = doc.add(doc.root(), ui::WidgetKind::Text, "slot_3");
    const ui::WidgetId wpn = doc.add(doc.root(), ui::WidgetKind::Text, "equip_weapon");
    const ui::WidgetId wt = doc.add(doc.root(), ui::WidgetKind::Text, "inventory_weight");
    doc.add(doc.root(), ui::WidgetKind::Text, "slot_25");  // past the end: ignored
    doc.add(doc.root(), ui::WidgetKind::Panel, "slot_4");  // not a Text: ignored

    World w;
    kit::install_inventory(w, nullptr);
    const asset::ItemCatalog cat = make_catalog();
    w.resource<kit::ItemRegistry>().define(kCat, cat);
    kit::Inventory inv;
    inv.catalog.guid = kCat;
    inv.max_weight_milli = 20000;
    kit::Equipment eq;
    eq.catalog.guid = kCat;
    REQUIRE(kit::inventory_add(inv, cat, 0, 7) == 7);  // slots 1 and 2: 5 + 2 potions
    REQUIRE(kit::inventory_add(inv, cat, 1, 1) == 1);
    const Entity hero = w.spawn_with(inv, eq);

    player::ItemGridFeed feed;
    feed.bind(doc);
    CHECK_FALSE(feed.empty());
    feed.feed(w, doc, Entity{});  // no screen entity: the first Inventory in the world
    CHECK(doc.text(s1) == StringView("Potion x5"));
    CHECK(doc.text(s2) == StringView("Potion x2"));
    CHECK(doc.text(s3) == StringView("Iron Sword"));
    CHECK(doc.text(wpn) == StringView(""));
    CHECK(doc.text(wt) == StringView("6.5 / 20.0 kg"));

    kit::Inventory& live = *w.try_get_mut<kit::Inventory>(hero);
    kit::Equipment& le = *w.try_get_mut<kit::Equipment>(hero);
    REQUIRE(kit::equipment_equip(live, le, cat, 2, static_cast<u32>(kit::EquipSlot::Weapon)));
    feed.feed(w, doc, hero);
    CHECK(doc.text(s3) == StringView(""));
    CHECK(doc.text(wpn) == StringView("Iron Sword"));
    CHECK(doc.text(wt) == StringView("3.5 / 20.0 kg"));  // the cap counts the inventory only, not what is worn

    ui::Document plain;
    plain.set_skin(f.skin);
    plain.add(plain.root(), ui::WidgetKind::Text, "title");
    player::ItemGridFeed none;
    none.bind(plain);
    CHECK(none.empty());
}

TEST_CASE("item grid feed: status_N shows active effects with the time left (M-STATUS HUD)") {
    SkinFixture f;
    ui::Document doc;
    doc.set_skin(f.skin);
    const ui::WidgetId a = doc.add(doc.root(), ui::WidgetKind::Text, "status_1");
    const ui::WidgetId b = doc.add(doc.root(), ui::WidgetKind::Text, "status_2");
    doc.add(doc.root(), ui::WidgetKind::Text, "status_9");  // past the end: ignored
    World w;
    kit::install_status(w, nullptr);
    kit::StatusEffects se;
    se.key[0] = kit::status_key("elixir");
    se.id[0] = "elixir";
    se.left_us[0] = 3450000;
    se.key[1] = kit::status_key("cd:potion");
    se.id[1] = "cd:potion";
    se.left_us[1] = 100000;
    const Entity hero = w.spawn_with(se);
    player::ItemGridFeed feed;
    feed.bind(doc);
    REQUIRE_FALSE(feed.empty());
    feed.feed(w, doc, Entity{});  // no screen entity: the first with effects
    CHECK(doc.text(a) == StringView("elixir 3.5s"));
    CHECK(doc.text(b) == StringView("cd:potion 0.1s"));
    w.try_get_mut<kit::StatusEffects>(hero)->key[1] = 0;
    feed.feed(w, doc, hero);
    CHECK(doc.text(b) == StringView(""));
}
