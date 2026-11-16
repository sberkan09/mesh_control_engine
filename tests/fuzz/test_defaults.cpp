// M-FUZZ 4: a project's convar defaults (.mcdefaults source and the ConvarDefaults chunk, "name value" lines). No
// committed sample ships one, so the corpus is written here: the source text goes through the importer mutated (raw bytes
// and valid JSON with one value swapped), and the chunk the importer makes is mutated byte-wise and read by
// console::set_defaults from a file, as the player does.
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mce/asset/container.h"
#include "mce/convar.h"
#include "mce/cook/importer.h"
#include "mce/log.h"
#include "import_support.h"
#include "json_swap.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;
namespace fs = std::filesystem;

namespace {

const char* const kDefaults = R"({ "format": "mce-defaults", "version": 1, "guid": "7e7a0000-0000-4000-8000-0000000000d1",
  "convars": { "ui.reference_height": 720, "access.captions": true, "audio.music_db": -6, "game.name": "Ember Step", "net.tick": 30.5 } })";

u32 iterations() {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : 300u;
}

bool chunk_of(const cook::IImporter& imp, Span<const byte> text, std::string* payload) {
    asset::ContainerWriter w;
    if (!fuzz::import_bytes(imp, "defaults/project.mcdefaults", text, &w)) return false;
    Array<byte> bytes;
    w.finish(bytes);
    auto c = asset::ContainerView::open(bytes.span());
    if (!c) return false;
    const u32 i = c.value().find(asset::ChunkType::ConvarDefaults);
    if (i >= c.value().chunk_count()) return false;
    if (payload) {
        const Span<const byte> p = c.value().payload(i);
        payload->assign(reinterpret_cast<const char*>(p.data()), p.size());
    }
    return true;
}

}  // namespace

TEST_CASE("fuzz: convar defaults survive mutated source and mutated chunks") {
    log::set_level(log::Level::Off);
    cook::ImporterRegistry reg;
    cook::ImporterRegistry::add_builtins(reg);
    const cook::IImporter* imp = reg.for_extension(".mcdefaults");
    REQUIRE(imp != nullptr);

    const std::string text = kDefaults;
    const Span<const byte> good(reinterpret_cast<const byte*>(text.data()), text.size());
    std::string chunk;
    REQUIRE_MESSAGE(chunk_of(*imp, good, &chunk), "the defaults must cook untouched");
    REQUIRE(!chunk.empty());

    const std::string path = (fs::temp_directory_path() / "mce_fuzz_defaults.txt").string();
    const u32 n = iterations();
    std::vector<std::uint64_t> storage;
    u64 cooked = 0, loaded = 0;
    fuzz::Mutator mt(0x64656673u);
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
        INFO("replay: defaults source iteration " << it);
        std::string out;
        if (chunk_of(*imp, Span<const byte>(reinterpret_cast<const byte*>(storage.data()), len), &out)) {
            ++cooked;  // an empty "convars" object cooks to an empty chunk, which is fine
        }
    }
    fuzz::Mutator mc(0x63766172u);
    const Span<const byte> cspan(reinterpret_cast<const byte*>(chunk.data()), chunk.size());
    for (u32 it = 0; it < n * 2; ++it) {
        INFO("replay: defaults chunk iteration " << it);
        const usize len = mc.mutate(cspan, storage);
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(storage.data()), static_cast<std::streamsize>(len));
        }
        loaded += static_cast<bool>(console::set_defaults(StringView(path.c_str(), path.size())));
    }
    std::error_code ec;
    fs::remove(path, ec);
    std::printf("fuzz: defaults %u source mutants (%llu cooked), %u chunk mutants (%llu read)\n", n,
                static_cast<unsigned long long>(cooked), n * 2, static_cast<unsigned long long>(loaded));
    CHECK(cooked > 0);
}
