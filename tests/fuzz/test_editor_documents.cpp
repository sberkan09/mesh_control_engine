// M-HARDEN2 2: the tilemap and terrain document readers of the editor, fed mutated source text (and, for the terrain,
// mutated cooked bytes). Nothing here may crash, hang or fire an MCE_ASSERT on any input, a reader that accepts a text
// must leave a document the brushes can work on, and what it accepts must survive a write and a second read.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "json_swap.h"
#include "mce/asset/formats.h"
#include "mce/cook/diagnostics.h"
#include "mce/editor/terrain_editor.h"
#include "mce/editor/tilemap_editor.h"
#include "mce/log.h"
#include "mutator.h"

using namespace mce;

namespace {

u32 iterations(u32 base) {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : base;
}

std::string mutant(fuzz::Mutator& m, std::vector<std::uint64_t>& storage, const std::string& seed, u32 it) {
    const Span<const byte> bytes(reinterpret_cast<const byte*>(seed.data()), seed.size());
    if (it % 2 == 1) return fuzz::swap_value(m, bytes);
    const usize len = m.mutate(bytes, storage);
    return std::string(reinterpret_cast<const char*>(storage.data()), len);
}

// A small valid cooked terrain: 33 samples, two layers, every sample on layer 0.
Array<byte> cooked_terrain() {
    asset::TerrainHeader h{};
    h.resolution = 33;
    h.chunk = 16;
    h.size[0] = h.size[1] = 32.0f;
    h.height_min = 0.0f;
    h.height_max = 10.0f;
    h.layer_count = 2;
    h.splat_resolution = 33;
    Array<u16> heights;
    heights.resize(33 * 33, u16{13107});
    Array<asset::TerrainLayer> ls;
    ls.resize(2);
    for (asset::TerrainLayer& l : ls) l.tile = 4.0f;
    Array<u8> splat;
    splat.resize(33 * 33 * 8, u8{0});
    for (usize i = 0; i < 33 * 33; ++i) splat[i * 4] = 255;
    Array<byte> blob;
    asset::write_terrain(h, heights.span(), blob, ls.span(), splat.span());
    return blob;
}

}  // namespace

TEST_CASE("fuzz: mutated tilemap text parses to a paintable map and what parses reads back") {
    log::set_level(log::Level::Off);
    editor::TilemapDocument seed_doc =
        editor::TilemapDocument::make(asset::Guid{}, "level", asset::Guid{}, 4, 2, 6, 3, 0.5f);
    seed_doc.set(0, 0, 3);
    seed_doc.fill_rect(2, 1, 4, 2, 5);
    cook::String seed_text;
    seed_doc.serialize(seed_text);
    const std::string seed(seed_text.view().data(), seed_text.view().size());
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x711E5);
    const u32 n = iterations(4000);
    u64 parsed = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: tilemap iteration " << it);
        const std::string text = mutant(m, storage, seed, it);
        editor::TilemapDocument d;
        cook::DiagList diags;
        if (!d.parse(StringView(text.data(), text.size()), "fuzz.mctilemap", diags)) continue;
        ++parsed;
        d.set(0, 0, 1);
        d.fill_rect(-2, -2, static_cast<i32>(d.width) + 2, static_cast<i32>(d.height) + 2, 1);
        (void)d.at(static_cast<i32>(d.width), static_cast<i32>(d.height));
        cook::String out;
        d.serialize(out);
        editor::TilemapDocument back;
        cook::DiagList diags2;
        REQUIRE(back.parse(out.view(), "back.mctilemap", diags2));
        CHECK(back.tiles.size() == d.tiles.size());
    }
    std::printf("fuzz: tilemap: %u mutants, %llu parsed\n", n, static_cast<unsigned long long>(parsed));
    CHECK(parsed > 0);
}

TEST_CASE("fuzz: mutated terrain source and cooked bytes load into a brushable document that reads back") {
    log::set_level(log::Level::Off);
    const std::string seed =
        R"({ "format": "mce-terrain", "version": 1, "name": "t", "resolution": 33, "size": [32, 32], "height_range": [0, 10], "fill": 2,
  "layers": [ { "tile": 3 }, { "triplanar": true } ] })";
    const Array<byte> blob = cooked_terrain();
    std::vector<std::uint64_t> storage, storage_blob;
    fuzz::Mutator m(0x7E44A1);
    const u32 n = iterations(2000);
    u64 loaded = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: terrain iteration " << it);
        const std::string text = mutant(m, storage, seed, it);
        // Every third mutant also corrupts the cooked payload.
        Array<byte> cooked = blob;
        if (it % 3 == 2) {
            const usize len = m.mutate(Span<const byte>(blob.data(), blob.size()), storage_blob);
            cooked.resize(len);
            for (usize i = 0; i < len; ++i) cooked[i] = reinterpret_cast<const byte*>(storage_blob.data())[i];
        }
        editor::TerrainDocument d;
        cook::DiagList diags;
        if (!d.load(StringView(text.data(), text.size()), cooked.span(), "fuzz.mcterrain", diags)) continue;
        ++loaded;
        editor::TerrainBrush b;
        (void)d.apply(b, math::vec2{1.0f, 1.0f});
        (void)d.height_at(-5.0f, 1000.0f);
        cook::String out;
        d.serialize(out);
        editor::TerrainDocument back;
        cook::DiagList diags2;
        CHECK(back.load(out.view(), cooked.span(), "back.mcterrain", diags2));
    }
    std::printf("fuzz: terrain: %u mutants, %llu loaded\n", n, static_cast<unsigned long long>(loaded));
    CHECK(loaded > 0);
}
