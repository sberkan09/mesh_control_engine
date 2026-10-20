// M-INVENTORY 1: .mcitems cook to CHNK_ITEMS; the chunk reader refuses damage; authoring mistakes are cook errors
// naming the file.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/asset/items_format.h"
#include "mce/cook/cook.h"

using namespace mce;
using namespace cook_test;

namespace {

constexpr const char* kCatalogGuid = "17e30000-0000-4000-8000-000000000001";
constexpr const char* kIconGuid = "17e30000-0000-4000-8000-0000000000a1";

std::string diags_text(const cook::CookReport& r) {
    std::string s;
    for (const auto& d : r.diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += line.c_str();
        s += "\n";
    }
    return s;
}

std::string catalog_json(const std::string& items, const std::string& tables) {
    return std::string(R"({ "format": "mce-items", "version": 1, "guid": ")") + kCatalogGuid + R"(", "items": [)" + items + R"(], "tables": [)" +
           tables + "] }";
}

const char* kItems = R"(
    { "id": "medkit", "name": "Medkit", "stack": 5, "weight": 0.4, "tags": ["consumable", "heal"], "stats": { "heal": 25, "use_time": 1.5 },
      "icon": "guid:17e30000-0000-4000-8000-0000000000a1" },
    { "id": "key", "stack": 1 },
    { "id": "ammo", "stack": 99, "weight": 0.01 })";

const char* kTables = R"(
    { "id": "chest_common", "rolls": [1, 3], "entries": [
        { "item": "medkit", "weight": 5, "qty": [1, 2] },
        { "item": "ammo", "weight": 10, "qty": [10, 30] },
        { "table": "chest_rare", "weight": 1 },
        { "item": "key", "guaranteed": true } ] },
    { "id": "chest_rare", "rolls": 1, "entries": [ { "item": "medkit", "weight": 1, "qty": 5 } ] })";

// Cooks `text` as assets/items.mcitems beside an icon texture; true when it cooked.
bool cook_text(const std::string& name, const std::string& text, std::string* diags = nullptr, std::string* root_out = nullptr) {
    const std::string root = fresh_dir(name);
    write_file(root + "/assets/icon.png", make_gradient_png(8, 8));
    write_file(root + "/assets/icon.png.mceasset", sidecar(kIconGuid, "texture"));
    write_file(root + "/assets/items.mcitems", text);
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(root.c_str()), cook::CookOptions{});
    if (diags) *diags = diags_text(r);
    if (root_out) *root_out = root;
    return r.ok();
}

bool load_catalog(const std::string& root, asset::ItemCatalog& out) {
    asset::AssetSystem assets;
    const std::string cooked = root + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    if (!assets.mount(Span<const StringView>(roots, 1))) return false;
    const asset::AssetKey key = assets.find(asset::parse_guid(kCatalogGuid).value());
    if (key == asset::kInvalidAssetKey || assets.info(key).type != asset::ChunkType::ItemCatalog) return false;
    auto h = assets.load_sync<asset::ItemCatalogChunk>(key);
    if (!h) return false;
    return static_cast<bool>(asset::decode_item_catalog(assets.resolve(h.value()), out));
}

}  // namespace

TEST_CASE("items: .mcitems cooks to CHNK_ITEMS and the catalog reads back") {
    std::string diags, root;
    REQUIRE_MESSAGE(cook_text("items_cook", catalog_json(kItems, kTables), &diags, &root), diags);
    asset::ItemCatalog c;
    REQUIRE(load_catalog(root, c));
    REQUIRE(c.items().size() == 3);
    REQUIRE(c.tables().size() == 2);

    const u16 medkit = c.find_item("medkit");
    REQUIRE(medkit != asset::kNoItem);
    const asset::ItemDef& m = c.items()[medkit];
    CHECK(m.name.view() == "Medkit");
    CHECK(m.stack_max == 5);
    CHECK(m.weight_milli == 400);
    CHECK(m.icon == asset::parse_guid(kIconGuid).value());
    CHECK(m.prefab.is_null());
    CHECK(c.has_tag(m, "heal"));
    CHECK_FALSE(c.has_tag(m, "weapon"));
    CHECK(c.stat(m, "heal") == 25.0f);
    CHECK(c.stat(m, "use_time") == 1.5f);
    CHECK(c.stat(m, "damage", -1.0f) == -1.0f);

    const asset::ItemDef& key = c.items()[c.find_item("key")];
    CHECK(key.name.view() == "key");  // the name defaults to the id
    CHECK(key.stack_max == 1);
    CHECK(key.weight_milli == 0);

    const asset::LootTable& common = c.tables()[c.find_table("chest_common")];
    CHECK(common.rolls_min == 1);
    CHECK(common.rolls_max == 3);
    const Span<const asset::LootEntry> es = c.entries_of(common);
    REQUIRE(es.size() == 4);
    CHECK(es[0].kind == asset::LootKind::Item);
    CHECK(es[0].qty_min == 1);
    CHECK(es[0].qty_max == 2);
    CHECK(es[2].kind == asset::LootKind::Table);
    CHECK(es[2].ref == c.find_table("chest_rare"));  // a forward reference
    CHECK(es[3].guaranteed);
    const asset::LootTable& rare = c.tables()[c.find_table("chest_rare")];
    CHECK(rare.rolls_min == 1);
    CHECK(rare.rolls_max == 1);
    CHECK(c.entries_of(rare)[0].qty_min == 5);
    CHECK(c.entries_of(rare)[0].qty_max == 5);
}

TEST_CASE("items: cooking twice gives the same chunk bytes") {
    std::string a_root, b_root;
    REQUIRE(cook_text("items_det_a", catalog_json(kItems, kTables), nullptr, &a_root));
    REQUIRE(cook_text("items_det_b", catalog_json(kItems, kTables), nullptr, &b_root));
    asset::ItemCatalog a, b;
    REQUIRE(load_catalog(a_root, a));
    REQUIRE(load_catalog(b_root, b));
    Array<byte> ba, bb;
    asset::encode_item_catalog(a, ba);
    asset::encode_item_catalog(b, bb);
    REQUIRE(ba.size() == bb.size());
    CHECK(std::memcmp(ba.data(), bb.data(), ba.size()) == 0);
}

TEST_CASE("items: authoring mistakes are cook errors naming the file") {
    struct Bad {
        std::string text;
        const char* expect;  // part of the message
    };
    const std::string one = R"({ "id": "a" })";
    const Bad bad[] = {
        {catalog_json(one + "," + one, ""), "share this \"id\""},
        {catalog_json(R"({ "id": "bad id" })", ""), "id"},
        {catalog_json(R"({ "id": "" })", ""), "non-empty"},
        {catalog_json(R"({ "id": "a", "stack": 0 })", ""), "'stack'"},
        {catalog_json(R"({ "id": "a", "stack": 1.5 })", ""), "'stack'"},
        {catalog_json(R"({ "id": "a", "stack": 1000001 })", ""), "'stack'"},
        {catalog_json(R"({ "id": "a", "weight": -1 })", ""), "'weight'"},
        {catalog_json(R"({ "id": "a", "stats": { "x": "high" } })", ""), "stat"},
        {catalog_json(R"({ "id": "a", "stats": [1] })", ""), "'stats'"},
        {catalog_json(R"({ "id": "a", "tags": [1] })", ""), "tag"},
        {catalog_json(R"({ "id": "a", "tags": ["x", "x"] })", ""), "listed twice"},
        {catalog_json(R"({ "id": "a", "icon": "nope" })", ""), "guid:<uuid>"},
        {catalog_json(R"({ "id": "a", "icon": "guid:17e30000-0000-4000-8000-0000000000ff" })", ""), "17e30000"},
        {catalog_json(R"({ "id": "a", "colour": "red" })", ""), "colour"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "zzz" } ] })"), "not in \"items\""},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "table": "zzz" } ] })"), "not in \"tables\""},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a", "table": "t" } ] })"), "exactly one"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { } ] })"), "exactly one"},
        {catalog_json(one, R"({ "id": "t", "entries": [] })"), "entries"},
        {catalog_json(one, R"({ "id": "t", "rolls": [3, 1], "entries": [ { "item": "a" } ] })"), "'rolls'"},
        {catalog_json(one, R"({ "id": "t", "rolls": 65, "entries": [ { "item": "a" } ] })"), "'rolls'"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a", "qty": 0 } ] })"), "'qty'"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a", "qty": [5, 2] } ] })"), "'qty'"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a", "weight": 0 } ] })"), "'weight'"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a", "guaranteed": 1 } ] })"), "guaranteed"},
        // it rolls, but everything in it is guaranteed
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a", "guaranteed": true } ] })"), "no weighted entry"},
        {catalog_json(one, R"({ "id": "t", "entries": [ { "item": "a" } ] }, { "id": "t", "entries": [ { "item": "a" } ] })"), "share this \"id\""},
        // loops
        {catalog_json(one, R"({ "id": "t", "entries": [ { "table": "t" } ] })"), "itself"},
        {catalog_json(one, R"({ "id": "p", "entries": [ { "table": "q" } ] }, { "id": "q", "entries": [ { "table": "p" } ] })"), "loop"},
        // not a catalog
        {R"({ "format": "mce-store", "version": 1, "guid": "17e30000-0000-4000-8000-000000000001" })", "mce-items"},
        {R"({ "format": "mce-items", "version": 2, "guid": "17e30000-0000-4000-8000-000000000001" })", "version"},
    };
    int n = 0;
    for (const Bad& b : bad) {
        std::string diags;
        const bool ok = cook_text("items_bad_" + std::to_string(n++), b.text, &diags);
        INFO("case " << (n - 1) << ": " << b.text);
        CHECK_FALSE(ok);
        CHECK(diags.find("items.mcitems") != std::string::npos);
        CHECK_MESSAGE(diags.find(b.expect) != std::string::npos, "expected '" << b.expect << "' in: " << diags);
    }
}

TEST_CASE("items: loot tables nest eight deep, not nine") {
    auto chain = [](int tables) {
        std::string t;
        for (int i = 0; i < tables; ++i) {
            if (i) t += ",";
            t += R"({ "id": "t)" + std::to_string(i) + R"(", "entries": [ )";
            t += i + 1 < tables ? R"({ "table": "t)" + std::to_string(i + 1) + R"(" })" : std::string(R"({ "item": "a" })");
            t += " ] }";
        }
        return catalog_json(R"({ "id": "a" })", t);
    };
    std::string diags;
    CHECK_MESSAGE(cook_text("items_nest8", chain(8), &diags), diags);
    CHECK_FALSE(cook_text("items_nest9", chain(9), &diags));
    CHECK(diags.find("deeper than 8") != std::string::npos);
}

TEST_CASE("items: shared tables, deepest-first order and guaranteed-only tables") {
    auto chain_rev = [](int tables) {
        std::string t;
        for (int i = tables - 1; i >= 0; --i) {
            if (!t.empty()) t += ",";
            t += R"({ "id": "t)" + std::to_string(i) + R"(", "entries": [ )";
            t += i + 1 < tables ? R"({ "table": "t)" + std::to_string(i + 1) + R"(" })" : std::string(R"({ "item": "a" })");
            t += " ] }";
        }
        return catalog_json(R"({ "id": "a" })", t);
    };
    std::string diags;
    CHECK_MESSAGE(cook_text("items_rev8", chain_rev(8), &diags), diags);
    CHECK_FALSE(cook_text("items_rev9", chain_rev(9), &diags));
    CHECK(diags.find("deeper than 8") != std::string::npos);
    // A diamond, and one table shared by more than eight parents, are not loops or deep.
    std::string shared = R"({ "id": "d", "entries": [ { "item": "a" } ] })";
    for (int i = 0; i < 12; ++i) shared += R"(, { "id": "p)" + std::to_string(i) + R"(", "entries": [ { "table": "d" } ] })";
    shared += R"(, { "id": "top", "entries": [ { "table": "p0" }, { "table": "p1" } ] })";
    CHECK_MESSAGE(cook_text("items_shared", catalog_json(R"({ "id": "a" })", shared), &diags), diags);
    // rolls 0 with only guaranteed entries, and a guaranteed entry's weight 0.
    CHECK_MESSAGE(cook_text("items_guar", catalog_json(R"({ "id": "a" })",
        R"({ "id": "t", "rolls": 0, "entries": [ { "item": "a", "guaranteed": true, "weight": 0 } ] })"), &diags), diags);
    // Weight rounds to the nearest thousandth.
    std::string root;
    REQUIRE(cook_text("items_round", catalog_json(R"({ "id": "a", "weight": 0.0004 }, { "id": "b", "weight": 0.0005 })", ""), &diags, &root));
    asset::ItemCatalog c;
    REQUIRE(load_catalog(root, c));
    CHECK(c.items()[0].weight_milli == 0);
    CHECK(c.items()[1].weight_milli == 1);
}

TEST_CASE("items: the chunk reader refuses damage") {
    asset::ItemCatalog c;
    asset::ItemDef a;
    a.id = "sword";
    a.name = "Sword";
    a.stack_max = 1;
    asset::ItemStat st{"damage", 7.0f};
    REQUIRE(c.add_item(a, Span<const asset::ItemStat>(&st, 1)));
    asset::ItemDef b;
    b.id = "arrow";
    b.name = "Arrow";
    b.stack_max = 50;
    REQUIRE(c.add_item(b));
    asset::LootTable t;
    t.id = "drops";
    t.rolls_min = 1;
    t.rolls_max = 2;
    asset::LootEntry e0, e1;
    e0.ref = 0;
    e1.ref = 1;
    e1.qty_max = 5;
    const asset::LootEntry es[] = {e0, e1};
    REQUIRE(c.add_table(t, Span<const asset::LootEntry>(es, 2)));
    REQUIRE(asset::item_catalog_problem(c) == nullptr);

    Array<byte> bytes;
    asset::encode_item_catalog(c, bytes);
    asset::ItemCatalog back;
    REQUIRE(asset::decode_item_catalog(bytes.span(), back));
    Array<byte> again;
    asset::encode_item_catalog(back, again);
    REQUIRE(again.size() == bytes.size());
    CHECK(std::memcmp(again.data(), bytes.data(), bytes.size()) == 0);

    // Every shorter prefix is refused, and a trailing byte is too.
    for (usize n = 0; n < bytes.size(); ++n) {
        asset::ItemCatalog o;
        CHECK_FALSE(asset::decode_item_catalog(Span<const byte>(bytes.data(), n), o));
    }
    Array<byte> longer = bytes;
    longer.push_back(byte{0});
    asset::ItemCatalog o;
    CHECK_FALSE(asset::decode_item_catalog(longer.span(), o));

    // A chunk the catalog's own checks refuse leaves nothing behind.
    {
        Array<byte> dangling_ref = bytes;
        // the last entry's ref (u16) sits 13 bytes from the end: ref, weight, qty_min, qty_max = 2+4+4+4
        dangling_ref[dangling_ref.size() - 14] = byte{9};
        asset::ItemCatalog left;
        CHECK_FALSE(asset::decode_item_catalog(dangling_ref.span(), left));
        CHECK(left.items().size() == 0);
        CHECK(left.tables().size() == 0);
    }

    // Counts far past the limits are refused before anything is allocated for them.
    Array<byte> huge = bytes;
    huge[0] = byte{0xFF};
    huge[1] = byte{0xFF};
    huge[2] = byte{0xFF};
    huge[3] = byte{0x7F};
    CHECK_FALSE(asset::decode_item_catalog(huge.span(), o));

    // Single bytes damaged: whatever the reader accepts must pass the catalog's own checks.
    for (usize i = 0; i < bytes.size(); ++i)
        for (const unsigned v : {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu}) {
            Array<byte> t2 = bytes;
            t2[i] = static_cast<byte>(v);
            asset::ItemCatalog r;
            if (asset::decode_item_catalog(t2.span(), r)) CHECK(asset::item_catalog_problem(r) == nullptr);
        }

    // A table that names an item that does not exist, and two ids alike, built in code.
    asset::ItemCatalog dangling;
    asset::LootEntry bad_entry;
    bad_entry.ref = 3;
    REQUIRE(dangling.add_table(t, Span<const asset::LootEntry>(&bad_entry, 1)));
    CHECK(asset::item_catalog_problem(dangling) != nullptr);
    asset::ItemCatalog twice;
    CHECK(twice.add_item(a));
    CHECK_FALSE(twice.add_item(a));
}

// ---- recipes (M-CRAFT 1)

namespace {

std::string recipes_json(const std::string& items, const std::string& recipes) {
    return std::string(R"({ "format": "mce-items", "version": 1, "guid": ")") + kCatalogGuid + R"(", "items": [)" + items + R"(], "recipes": [)" +
           recipes + "] }";
}

const char* kCraftItems = R"(
    { "id": "cloth", "stack": 20 },
    { "id": "herb", "stack": 20 },
    { "id": "bandage", "name": "Bandage", "stack": 5 })";

const char* kRecipes = R"(
    { "id": "bandage", "name": "Bandage", "station": "workbench", "time": 1.5,
      "in": [ { "item": "cloth", "qty": 2 }, { "item": "herb" } ], "out": [ { "item": "bandage", "qty": 1 } ] },
    { "id": "unravel", "in": [ { "item": "bandage" } ], "out": [ { "item": "cloth", "qty": 2 } ] })";

}  // namespace

TEST_CASE("recipes: .mcitems recipes cook and read back; a catalog without them keeps the v1.0 bytes") {
    std::string diags, root;
    REQUIRE_MESSAGE(cook_text("recipes_cook", recipes_json(kCraftItems, kRecipes), &diags, &root), diags);
    asset::ItemCatalog c;
    REQUIRE(load_catalog(root, c));
    REQUIRE(c.recipes().size() == 2);
    const asset::Recipe& r = c.recipes()[c.find_recipe("bandage")];
    CHECK(r.name.view() == "Bandage");
    CHECK(r.station.view() == "workbench");
    CHECK(r.time_ms == 1500);
    REQUIRE(c.inputs_of(r).size() == 2);
    CHECK(c.inputs_of(r)[0].item == c.find_item("cloth"));
    CHECK(c.inputs_of(r)[0].qty == 2);
    CHECK(c.inputs_of(r)[1].qty == 1);  // qty defaults to 1
    REQUIRE(c.outputs_of(r).size() == 1);
    CHECK(c.outputs_of(r)[0].item == c.find_item("bandage"));
    const asset::Recipe& u = c.recipes()[c.find_recipe("unravel")];
    CHECK(u.name.view() == "unravel");  // the name defaults to the id
    CHECK(u.station.empty());
    CHECK(u.time_ms == 0);

    // The same catalog without its recipes is the v1.0 payload, byte for byte.
    asset::ItemCatalog plain;
    for (const asset::ItemDef& it : c.items()) REQUIRE(plain.add_item(it));
    Array<byte> with, without;
    asset::encode_item_catalog(c, with);
    asset::encode_item_catalog(plain, without);
    REQUIRE(with.size() > without.size());
    CHECK(std::memcmp(with.data(), without.data(), without.size()) == 0);
    // ... which is what a reader still sees when the recipes are cut off cleanly, and a cut inside them is refused.
    asset::ItemCatalog o;
    CHECK(asset::decode_item_catalog(Span<const byte>(with.data(), without.size()), o));
    CHECK(o.recipes().size() == 0);
    for (usize n = without.size() + 1; n < with.size(); ++n) CHECK_FALSE(asset::decode_item_catalog(Span<const byte>(with.data(), n), o));
    Array<byte> longer = with;
    longer.push_back(byte{0});
    CHECK_FALSE(asset::decode_item_catalog(longer.span(), o));
    // Round trip, and any single damaged byte either fails or passes the catalog's own checks.
    asset::ItemCatalog back;
    REQUIRE(asset::decode_item_catalog(with.span(), back));
    Array<byte> again;
    asset::encode_item_catalog(back, again);
    REQUIRE(again.size() == with.size());
    CHECK(std::memcmp(again.data(), with.data(), with.size()) == 0);
    for (usize i = without.size(); i < with.size(); ++i)
        for (const unsigned v : {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu}) {
            Array<byte> t2 = with;
            t2[i] = static_cast<byte>(v);
            asset::ItemCatalog dmg;
            if (asset::decode_item_catalog(t2.span(), dmg)) CHECK(asset::item_catalog_problem(dmg) == nullptr);
        }
}

TEST_CASE("recipes: authoring mistakes are cook errors that name the problem") {
    struct Bad {
        std::string text;
        const char* expect;
    };
    const std::string in = R"("in": [ { "item": "cloth" } ])", out = R"("out": [ { "item": "bandage" } ])";
    const Bad bad[] = {
        {recipes_json(kCraftItems, R"({ "in": [], "out": [] })"), "\"id\""},
        {recipes_json(kCraftItems, R"({ "id": "r", "in": [], )" + out + " }"), "'in'"},
        {recipes_json(kCraftItems, R"({ "id": "r", )" + in + R"(, "out": [] })"), "'out'"},
        {recipes_json(kCraftItems, R"({ "id": "r", )" + in + " }"), "'out'"},
        {recipes_json(kCraftItems, R"({ "id": "r", "in": [ { "item": "zzz" } ], )" + out + " }"), "not in \"items\""},
        {recipes_json(kCraftItems, R"({ "id": "r", "in": [ { "item": "cloth", "qty": 0 } ], )" + out + " }"), "'qty'"},
        {recipes_json(kCraftItems, R"({ "id": "r", "in": [ { "item": "cloth", "qty": 1000001 } ], )" + out + " }"), "'qty'"},
        {recipes_json(kCraftItems, R"({ "id": "r", "in": [ { "item": "cloth" }, { "item": "cloth" } ], )" + out + " }"), "listed twice"},
        {recipes_json(kCraftItems, R"({ "id": "r", "in": [ { "item": "cloth", "x": 1 } ], )" + out + " }"), "unknown key"},
        {recipes_json(kCraftItems, R"({ "id": "r", "time": -1, )" + in + ", " + out + " }"), "'time'"},
        {recipes_json(kCraftItems, R"({ "id": "r", "time": 3601, )" + in + ", " + out + " }"), "'time'"},
        {recipes_json(kCraftItems, R"({ "id": "r", "station": "a b", )" + in + ", " + out + " }"), "station"},
        {recipes_json(kCraftItems, R"({ "id": "r", "colour": 1, )" + in + ", " + out + " }"), "colour"},
        {recipes_json(kCraftItems, R"({ "id": "r", )" + in + ", " + out + R"( }, { "id": "r", )" + in + ", " + out + " }"), "share this \"id\""},
        {std::string(R"({ "format": "mce-items", "version": 1, "guid": ")") + kCatalogGuid + R"(", "recipes": {} })", "'recipes'"},
    };
    int n = 0;
    for (const Bad& b : bad) {
        std::string diags;
        const bool ok = cook_text("recipes_bad_" + std::to_string(n++), b.text, &diags);
        INFO("case " << (n - 1) << ": " << b.text);
        CHECK_FALSE(ok);
        CHECK_MESSAGE(diags.find(b.expect) != std::string::npos, "expected '" << b.expect << "' in: " << diags);
    }
}

TEST_CASE("recipes: the catalog's own checks refuse a recipe built wrong in code") {
    asset::ItemCatalog c;
    asset::ItemDef d;
    d.id = "a";
    d.name = "A";
    REQUIRE(c.add_item(d));
    asset::Recipe r;
    r.id = "r";
    asset::RecipeIngredient ok{0, 1}, past{5, 1}, zero{0, 0};
    CHECK_FALSE(c.add_recipe(asset::Recipe{}, Span<const asset::RecipeIngredient>(&ok, 1), Span<const asset::RecipeIngredient>(&ok, 1)));  // empty id
    REQUIRE(c.add_recipe(r, Span<const asset::RecipeIngredient>(&past, 1), Span<const asset::RecipeIngredient>(&ok, 1)));
    const char* why = asset::item_catalog_problem(c);
    REQUIRE(why != nullptr);
    CHECK(std::string(why).find("recipe") == 0);
    asset::ItemCatalog z;
    REQUIRE(z.add_item(d));
    REQUIRE(z.add_recipe(r, Span<const asset::RecipeIngredient>(&zero, 1), Span<const asset::RecipeIngredient>(&ok, 1)));
    CHECK(asset::item_catalog_problem(z) != nullptr);
    asset::ItemCatalog none;
    REQUIRE(none.add_item(d));
    REQUIRE(none.add_recipe(r, Span<const asset::RecipeIngredient>(), Span<const asset::RecipeIngredient>(&ok, 1)));
    CHECK(asset::item_catalog_problem(none) != nullptr);  // a recipe needs inputs
    asset::ItemCatalog dup;
    REQUIRE(dup.add_item(d));
    REQUIRE(dup.add_recipe(r, Span<const asset::RecipeIngredient>(&ok, 1), Span<const asset::RecipeIngredient>(&ok, 1)));
    CHECK_FALSE(dup.add_recipe(r, Span<const asset::RecipeIngredient>(&ok, 1), Span<const asset::RecipeIngredient>(&ok, 1)));
}
