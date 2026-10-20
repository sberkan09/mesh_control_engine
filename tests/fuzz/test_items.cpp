// M-FUZZ / M-INVENTORY 1: the item catalog (.mcitems source and the ItemCatalog chunk). No committed sample ships
// one, so the corpus is a small catalog written here: its source text goes through the importer mutated (byte damage,
// and valid JSON with one value swapped for another kind), and the chunk the importer makes is mutated byte-wise and
// read back through decode_item_catalog. Whatever the reader accepts must pass the catalog's own checks.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mce/asset/container.h"
#include "mce/asset/items_format.h"
#include "mce/cook/importer.h"
#include "mce/log.h"
#include "json_swap.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;

namespace {

class NoLookup final : public cook::IAssetLookup {
public:
    bool type_of(cook::Guid, cook::ChunkType&) const override { return false; }
    bool guid_for_path(StringView, cook::Guid&) const override { return false; }
};

const char* const kCatalog = R"({ "format": "mce-items", "version": 1, "guid": "17e30000-0000-4000-8000-0000000000f1",
  "items": [
    { "id": "medkit", "name": "Medkit", "stack": 5, "weight": 0.4, "tags": ["consumable", "heal"], "stats": { "heal": 25, "use_time": 1.5 } },
    { "id": "key", "stack": 1 },
    { "id": "ammo", "stack": 99, "weight": 0.01, "tags": ["ammo"] } ],
  "tables": [
    { "id": "chest_common", "rolls": [1, 3], "entries": [
        { "item": "medkit", "weight": 5, "qty": [1, 2] }, { "item": "ammo", "weight": 10, "qty": [10, 30] },
        { "table": "chest_rare", "weight": 1 }, { "item": "key", "guaranteed": true } ] },
    { "id": "chest_rare", "rolls": 1, "entries": [ { "item": "medkit", "weight": 1, "qty": 5 } ] } ],
  "recipes": [
    { "id": "ration", "name": "Ration", "station": "campfire", "time": 2.5,
      "in": [ { "item": "ammo", "qty": 10 }, { "item": "key" } ], "out": [ { "item": "medkit", "qty": 2 } ] },
    { "id": "scrap", "in": [ { "item": "medkit" } ], "out": [ { "item": "ammo", "qty": 5 } ] } ] })";

// Cooks `text`; on success the ItemCatalog chunk's payload lands in `payload`.
bool cook_items(const cook::IImporter& imp, Span<const byte> text, std::vector<byte>* payload) {
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(StringView(reinterpret_cast<const char*>(text.data()), text.size()), err)) return false;
    cook::ImportInput in;
    in.display_path = "assets/loot.mcitems";
    in.source_path = "loot.mcitems";
    in.source = text;
    in.settings = doc.root();
    static const NoLookup lookup;
    in.lookup = &lookup;
    cook::DiagList diags;
    Array<cook::DepRef> deps;
    imp.scan_dependencies(in, deps, diags);
    if (diags.error_count() != 0) return false;
    asset::ContainerWriter w;
    if (!imp.cook(in, w, diags) || diags.error_count() != 0) return false;
    Array<byte> bytes;
    w.finish(bytes);
    auto c = asset::ContainerView::open(bytes.span());
    if (!c) return false;
    const u32 i = c.value().find(asset::ChunkType::ItemCatalog);
    if (i >= c.value().chunk_count()) return false;
    if (payload) {
        const Span<const byte> p = c.value().payload(i);
        payload->assign(p.data(), p.data() + p.size());
    }
    return true;
}

bool read_back(Span<const byte> p) {
    asset::ItemCatalog c;
    if (!asset::decode_item_catalog(p, c)) return false;
    CHECK(asset::item_catalog_problem(c) == nullptr);
    for (const asset::ItemDef& d : c.items()) {  // follow every range
        (void)c.stats_of(d);
        (void)c.tags_of(d);
    }
    for (const asset::LootTable& t : c.tables()) (void)c.entries_of(t);
    for (const asset::Recipe& r : c.recipes()) {
        (void)c.inputs_of(r);
        (void)c.outputs_of(r);
    }
    return true;
}

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

}  // namespace

TEST_CASE("fuzz: item catalogs survive mutated source and mutated chunks") {
    log::set_level(log::Level::Off);
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    const cook::IImporter* imp = reg.for_extension(".mcitems");
    REQUIRE(imp != nullptr);

    const std::string text = kCatalog;
    const Span<const byte> good(reinterpret_cast<const byte*>(text.data()), text.size());
    std::vector<byte> chunk;
    REQUIRE_MESSAGE(cook_items(*imp, good, &chunk), "the catalog must cook untouched");
    REQUIRE(read_back(Span<const byte>(chunk.data(), chunk.size())));

    const u32 n = iterations();
    std::vector<std::uint64_t> storage;
    u64 text_ok = 0, chunk_ok = 0;
    fuzz::Mutator mt(0x6974656du);
    for (u32 it = 0; it < n; ++it) {
        usize len;
        std::string swapped;
        if (it % 2 == 1) {
            swapped = fuzz::swap_value(mt, good);
            len = swapped.size();
            if (storage.size() * 8 < len) storage.resize((len + 7) / 8);
            std::memcpy(storage.data(), swapped.data(), len);
        } else {
            len = mt.mutate(good, storage);
        }
        INFO("replay: items source iteration " << it);
        std::vector<byte> out;
        if (cook_items(*imp, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len), &out)) {
            ++text_ok;
            CHECK_MESSAGE(read_back(Span<const byte>(out.data(), out.size())), "a chunk the importer made must read back");
        }
    }
    fuzz::Mutator mc(0x6368756eu);
    for (u32 it = 0; it < n * 4; ++it) {
        INFO("replay: items chunk iteration " << it);
        const usize len = mc.mutate(Span<const byte>(chunk.data(), chunk.size()), storage);
        chunk_ok += read_back(Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len));
    }
    std::printf("fuzz: items %u source mutants (%llu cooked), %u chunk mutants (%llu read)\n", n, static_cast<unsigned long long>(text_ok), n * 4,
                static_cast<unsigned long long>(chunk_ok));
    CHECK(text_ok > 0);
}
