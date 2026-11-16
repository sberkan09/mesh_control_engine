// M-FUZZ 4: the road network (.mcroads source and the RoadNetwork chunk). No committed sample ships one, so the
// corpus is a small crossroads written here: its source text goes through the importer mutated, and the chunk
// the importer makes is mutated byte-wise and read back through RoadView with every range followed.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mce/asset/container.h"
#include "mce/asset/roads_format.h"
#include "mce/cook/importer.h"
#include "mce/log.h"
#include "json_swap.h"
#include "mutator.h"
#include "test_support.h"
#include "view_walk.h"

using namespace mce;

namespace {

class NoLookup final : public cook::IAssetLookup {
public:
    bool type_of(cook::Guid, cook::ChunkType&) const override { return false; }
    bool guid_for_path(StringView, cook::Guid&) const override { return false; }
};

const char* const kCrossroads = R"({ "format": "mce-roads", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000b1",
  "name": "cross",
  "nodes": [{"name": "c", "at": [0, 0, 0]}, {"name": "n", "at": [0, 0, -60]}, {"name": "s", "at": [0, 0, 60]},
            {"name": "e", "at": [60, 0, 0]}, {"name": "w", "at": [-60, 0, 0]}],
  "roads": [{"name": "north", "from": "c", "to": "n", "lanes": 1}, {"name": "south", "from": "c", "to": "s", "lanes": 1},
            {"name": "east", "from": "c", "to": "e", "lanes": 1, "priority": 1}, {"name": "west", "from": "c", "to": "w", "lanes": 1, "priority": 1}],
  "signals": [{"node": "c", "amber": 2, "all_red": 1, "offset": 5, "phases": [{"green": ["east", "west"], "seconds": 20}, {"green": ["north", "south"], "seconds": 10}]}] })";

// Cooks `text`; on success the RoadNetwork chunk's payload lands in `payload`.
bool cook_roads(const cook::IImporter& imp, Span<const byte> text, std::vector<byte>* payload) {
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(StringView(reinterpret_cast<const char*>(text.data()), text.size()), err)) return false;
    cook::ImportInput in;
    in.display_path = "assets/net.mcroads";
    in.source_path = "net.mcroads";
    in.source = text;
    in.settings = doc.root();
    static const NoLookup lookup;
    in.lookup = &lookup;
    cook::DiagList diags;
    asset::ContainerWriter w;
    if (!imp.cook(in, w, diags) || diags.error_count() != 0) return false;
    Array<byte> bytes;
    w.finish(bytes);
    auto c = asset::ContainerView::open(bytes.span());
    if (!c) return false;
    const u32 i = c.value().find(asset::ChunkType::RoadNetwork);
    if (i >= c.value().chunk_count()) return false;
    if (payload) {
        const Span<const byte> p = c.value().payload(i);
        payload->assign(p.data(), p.data() + p.size());
    }
    return true;
}

bool read_and_walk(Span<const byte> p) {
    auto v = asset::RoadView::read(p);
    if (!v) return false;
    static fuzz::Sink sink;
    fuzz::walk(sink, v.value());
    return true;
}

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

}  // namespace

TEST_CASE("fuzz: road networks survive mutated source and mutated chunks") {
    log::set_level(log::Level::Off);
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    const cook::IImporter* imp = reg.for_extension(".mcroads");
    REQUIRE(imp != nullptr);

    const std::string text = kCrossroads;
    const Span<const byte> good(reinterpret_cast<const byte*>(text.data()), text.size());
    std::vector<byte> chunk;
    REQUIRE_MESSAGE(cook_roads(*imp, good, &chunk), "the crossroads must cook untouched");
    REQUIRE(read_and_walk(Span<const byte>(chunk.data(), chunk.size())));

    const u32 n = iterations();
    std::vector<std::uint64_t> storage;
    u64 text_ok = 0, chunk_ok = 0;
    fuzz::Mutator mt(0x726f6164u);
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
        INFO("replay: roads source iteration " << it);
        std::vector<byte> out;
        if (cook_roads(*imp, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len), &out)) {
            ++text_ok;
            CHECK_MESSAGE(read_and_walk(Span<const byte>(out.data(), out.size())), "a chunk the importer made must read back");
        }
    }
    fuzz::Mutator mc(0x63686e6bu);
    for (u32 it = 0; it < n * 4; ++it) {
        INFO("replay: roads chunk iteration " << it);
        const usize len = mc.mutate(Span<const byte>(chunk.data(), chunk.size()), storage);
        chunk_ok += read_and_walk(Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len));
    }
    std::printf("fuzz: roads %u source mutants (%llu cooked), %u chunk mutants (%llu read)\n", n,
                static_cast<unsigned long long>(text_ok), n * 4, static_cast<unsigned long long>(chunk_ok));
    CHECK(text_ok > 0);
}
