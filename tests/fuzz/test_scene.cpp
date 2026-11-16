// M-FUZZ 3c: scene instantiate. A cooked Scene chunk is validated against the reflection registry and then spawned;
// a damaged one (column types, row counts, entity references, fixups) must be refused, never crash. The corpus is the
// scene chunks of every pack the build produced (MCE_FUZZ_ROOT); kit components are linked so the real columns resolve.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "mce/asset/container.h"
#include "mce/asset/file_io.h"
#include "mce/asset/formats.h"
#include "mce/ecs/world.h"
#include "mce/log.h"
#include "mce/scene/scene.h"
#include "mutator.h"
#include "test_support.h"

using namespace mce;
namespace fs = std::filesystem;

namespace {

u64 fnv(const std::string& s) {
    u64 h = 1469598103934665603ull;
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

bool spawn(Span<const byte> payload) {
    auto v = asset::SceneView::read(payload);
    if (!v) return false;
    World world;
    Array<Entity> spawned;
    return static_cast<bool>(scene::instantiate(world, v.value(), &spawned));
}

}  // namespace

TEST_CASE("fuzz: scene instantiate survives mutated scene chunks of every cooked pack") {
    const char* root_env = std::getenv("MCE_FUZZ_ROOT");
    REQUIRE_MESSAGE(root_env != nullptr, "MCE_FUZZ_ROOT (the build tree) is not set");
    const fs::path root = root_env;
    std::vector<fs::path> packs;
    for (const char* sub : {"samples", "projects", "template_probe"}) {
        if (!fs::exists(root / sub)) continue;
        for (const auto& e : fs::recursive_directory_iterator(root / sub))
            if (e.is_regular_file() && e.path().extension() == ".mce") packs.push_back(e.path());
    }
    std::sort(packs.begin(), packs.end());
    REQUIRE(!packs.empty());
    log::set_level(log::Level::Off);

    const char* it_env = std::getenv("MCE_FUZZ_ITERS");
    const u32 iters = it_env ? static_cast<u32>(std::strtoul(it_env, nullptr, 10)) : 60u;
    std::vector<std::uint64_t> storage;
    u64 scenes = 0, mutants = 0, accepted = 0, untouched_ok = 0;
    for (const fs::path& pack : packs) {
        Array<byte> bytes;
        if (!asset::read_file(pack.string(), bytes)) continue;
        auto opened = asset::ContainerView::open(bytes.span(), pack.string());
        if (!opened) continue;
        const asset::ContainerView& v = opened.value();
        const std::string rel = fs::relative(pack, root).generic_string();
        for (u32 i = 0; i < v.chunk_count(); ++i) {
            if (v.entry(i).type != asset::ChunkType::Scene) continue;
            const Span<const byte> good = v.payload(i);
            ++scenes;
            untouched_ok += spawn(good);
            fuzz::Mutator m(fnv(rel) ^ (static_cast<u64>(i) * 0x100000001B3ull));
            for (u32 it = 0; it < iters; ++it) {
                const usize n = m.mutate(good, storage);
                INFO("replay: pack " << rel << " chunk " << i << " iteration " << it);
                accepted += spawn(Span<const byte>(reinterpret_cast<const byte*>(storage.data()), n));
                ++mutants;
            }
        }
    }
    std::printf("fuzz: scene instantiate: %llu scenes (%llu spawn untouched), %llu mutants, %llu spawned\n",
                static_cast<unsigned long long>(scenes), static_cast<unsigned long long>(untouched_ok), static_cast<unsigned long long>(mutants),
                static_cast<unsigned long long>(accepted));
    CHECK(scenes > 0);
}
