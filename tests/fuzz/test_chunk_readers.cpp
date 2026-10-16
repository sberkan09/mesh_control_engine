// M-FUZZ 1, 2: every chunk reader of the asset module, and the decoders of the other modules, over mutated
// payloads of real cooked chunks, then the walk of what a reader accepts. A reader may
// refuse a payload (an error result) or accept it, but it must not crash, hang or fire an MCE_ASSERT: the
// payload is user data. Under the ASan stage the same run also catches out-of-bounds reads.
//
// The corpus is every .mce the build produced for a sample or project (MCE_FUZZ_ROOT is the build tree);
// each chunk is mutated MCE_FUZZ_ITERS times (smaller for big chunks) from a seed that depends only on the
// pack's relative path, the chunk index and the iteration, so a failure replays from those three.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "mce/asset/container.h"
#include "mce/asset/file_io.h"
#include "mce/asset/formats.h"
#include "mce/input/input_map.h"
#include "mce/liveops/game_services.h"
#include "mce/log.h"
#include "mce/liveops/remote_config.h"
#include "mce/loc/string_table.h"
#include "mce/monetize/catalog.h"
#include "mce/physics3d/navmesh3d.h"
#include "mce/ui/ui.h"
#include "mce/ui/document_data.h"
#include "mutator.h"
#include "view_walk.h"
#include "test_support.h"

using namespace mce;
using namespace mce::asset;
namespace fs = std::filesystem;

namespace {

// Reads a payload and, when the reader accepts it, follows everything the view lets a consumer reach.
template <class V>
bool walked(Span<const byte> p) {
    auto r = V::read(p);
    if (!r) return false;
    static fuzz::Sink sink;
    if constexpr (requires { fuzz::walk(sink, r.value()); }) fuzz::walk(sink, r.value());
    return true;
}

// One reader per chunk type.
bool read_chunk(ChunkType t, Span<const byte> p) {
    switch (t) {
        case ChunkType::Manifest: return walked<ManifestView>(p);
        case ChunkType::Texture: return walked<TextureView>(p);
        case ChunkType::Sfx: return walked<SfxView>(p);
        case ChunkType::Video: return walked<VideoView>(p);
        case ChunkType::Script: return walked<ScriptView>(p);
        case ChunkType::Font: return walked<FontView>(p);
        case ChunkType::Material: return walked<MaterialChunk>(p);
        case ChunkType::Tilemap: return walked<TilemapView>(p);
        case ChunkType::AnimGraph: return walked<AnimGraphView>(p);
        case ChunkType::Terrain: return walked<TerrainView>(p);
        case ChunkType::ParticlePreset: return walked<ParticlePresetChunk>(p);
        case ChunkType::Sequence: return walked<SequenceView>(p);
        case ChunkType::Timeline: return walked<TimelineView>(p);
        case ChunkType::BehaviorTree: return walked<BtView>(p);
        case ChunkType::Skeleton2D: return walked<Skeleton2DView>(p);
        case ChunkType::Shader: return walked<ShaderView>(p);
        case ChunkType::Mesh: return walked<MeshView>(p);
        case ChunkType::Scene: return walked<SceneView>(p);
        case ChunkType::ModInfo: return walked<ModInfoView>(p);
        case ChunkType::SpriteSheet: return walked<SpriteSheetView>(p);
        case ChunkType::StringTable: { loc::StringTable o; return static_cast<bool>(loc::decode_table(p, o)); }
        case ChunkType::InputMap: { input::InputMapSet o; return static_cast<bool>(input::decode_input_map(p, o)); }
        case ChunkType::StoreCatalog: { monetize::Catalog o; return static_cast<bool>(monetize::decode_catalog(p, o)); }
        case ChunkType::RemoteConfig: { liveops::ConfigValues o; return static_cast<bool>(liveops::decode_config(p, o)); }
        case ChunkType::GameServicesCatalog: {
            liveops::ServicesCatalog o;
            return static_cast<bool>(liveops::decode_services_catalog(p, o));
        }
        case ChunkType::UiTheme: { ui::ThemeData o; return static_cast<bool>(ui::decode_ui_theme(p, o)); }
        case ChunkType::UiDocument: {
            ui::DocumentData o;
            if (!ui::decode_ui_document(p, o)) return false;
            ui::Document doc;  // what the runtime does next
            (void)ui::build_document(o, doc);
            return true;
        }
        case ChunkType::NavMesh: {
            physics::NavMesh3D nav;
            if (!nav.load(p, 4)) return false;
            Array<math::vec3> path;
            const math::aabb3 b = nav.bounds();
            nav.find_path(b.min, b.max, path);
            (void)nav.nearest_point(b.max);
            (void)nav.raycast(b.min, b.max);
            return true;
        }
        default: return true;
    }
}

bool has_reader(ChunkType t) {
    switch (t) {
        case ChunkType::Manifest: case ChunkType::Texture: case ChunkType::Sfx: case ChunkType::Video:
        case ChunkType::Script: case ChunkType::Font: case ChunkType::Material: case ChunkType::Tilemap:
        case ChunkType::AnimGraph: case ChunkType::Terrain: case ChunkType::ParticlePreset: case ChunkType::Sequence:
        case ChunkType::Timeline: case ChunkType::BehaviorTree: case ChunkType::Skeleton2D: case ChunkType::Shader:
        case ChunkType::Mesh: case ChunkType::Scene: case ChunkType::ModInfo: case ChunkType::SpriteSheet:
        case ChunkType::StringTable: case ChunkType::InputMap: case ChunkType::StoreCatalog: case ChunkType::RemoteConfig:
        case ChunkType::GameServicesCatalog: case ChunkType::UiTheme: case ChunkType::UiDocument: case ChunkType::NavMesh:
            return true;
        default: return false;
    }
}

u64 fnv(const std::string& s) {
    u64 h = 1469598103934665603ull;
    for (char c : s) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return h;
}

u32 iterations(usize payload_size) {
    const u32 base = 64;
    if (payload_size <= 64 * 1024) return base;
    if (payload_size <= 1024 * 1024) return base / 4;
    return base / 16;
}

}  // namespace

TEST_CASE("fuzz: chunk readers survive mutated payloads of every cooked sample chunk") {
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
    REQUIRE_MESSAGE(!packs.empty(), "no assets.mce under " << root);

    log::set_level(log::Level::Off);  // a refused or half-valid payload logs by design
    std::map<std::string, u32> chunks_by_type, unread_by_type;
    u64 mutants = 0, accepted = 0;
    std::vector<std::uint64_t> storage;
    for (const fs::path& pack : packs) {
        Array<byte> bytes;
        if (!read_file(pack.string(), bytes)) continue;
        auto opened = ContainerView::open(bytes.span(), pack.string());
        if (!opened) continue;
        const ContainerView& v = opened.value();
        const std::string rel = fs::relative(pack, root).generic_string();
        for (u32 i = 0; i < v.chunk_count(); ++i) {
            const ChunkType type = v.entry(i).type;
            if (!has_reader(type)) {
                ++unread_by_type[chunk_type_name(type)];
                continue;
            }
            const Span<const byte> good = v.payload(i);
            ++chunks_by_type[chunk_type_name(type)];
            // The untouched payload reads (or is refused) without a fault.
            read_chunk(type, good);
            fuzz::Mutator m(fnv(rel) ^ (static_cast<u64>(i) * 0x100000001B3ull));
            const u32 iters = iterations(good.size());
            for (u32 it = 0; it < iters; ++it) {
                const usize n = m.mutate(good, storage);
                const auto* p = reinterpret_cast<const byte*>(storage.data());
                INFO("replay: pack " << rel << " chunk " << i << " (" << chunk_type_name(type) << ") iteration " << it);
                if (read_chunk(type, Span<const byte>(p, n))) ++accepted;
                ++mutants;
            }
        }
    }
    std::printf("fuzz: %zu packs, %llu mutants, %llu read without an error\n", packs.size(),
                static_cast<unsigned long long>(mutants), static_cast<unsigned long long>(accepted));
    for (const auto& kv : chunks_by_type) std::printf("fuzz:   %-16s %u chunks\n", kv.first.c_str(), kv.second);
    for (const auto& kv : unread_by_type) std::printf("fuzz:   %-16s %u chunks, no reader in this test\n", kv.first.c_str(), kv.second);
    // The corpus must be broad enough to mean something.
    CHECK(chunks_by_type.size() >= 10);
    CHECK(mutants > 1000);
}
