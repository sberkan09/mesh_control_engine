// M-FUZZ 1: every chunk reader of the asset module over mutated payloads of real cooked chunks. A reader may
// refuse a payload (an error result) or accept it, but it must not crash, hang or fire an MCE_ASSERT: the
// payload is user data. Under the ASan stage the same run also catches out-of-bounds reads.
//
// The corpus is every assets.mce the build produced for a sample or project (MCE_FUZZ_ROOT is the build tree);
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
#include "mutator.h"
#include "test_support.h"

using namespace mce;
using namespace mce::asset;
namespace fs = std::filesystem;

namespace {

// One reader per chunk type; the result is dropped, only the call matters.
bool read_chunk(ChunkType t, Span<const byte> p) {
    switch (t) {
        case ChunkType::Manifest: return static_cast<bool>(ManifestView::read(p));
        case ChunkType::Texture: return static_cast<bool>(TextureView::read(p));
        case ChunkType::Sfx: return static_cast<bool>(SfxView::read(p));
        case ChunkType::Video: return static_cast<bool>(VideoView::read(p));
        case ChunkType::Script: return static_cast<bool>(ScriptView::read(p));
        case ChunkType::Font: return static_cast<bool>(FontView::read(p));
        case ChunkType::Material: return static_cast<bool>(MaterialChunk::read(p));
        case ChunkType::Tilemap: return static_cast<bool>(TilemapView::read(p));
        case ChunkType::AnimGraph: return static_cast<bool>(AnimGraphView::read(p));
        case ChunkType::Terrain: return static_cast<bool>(TerrainView::read(p));
        case ChunkType::ParticlePreset: return static_cast<bool>(ParticlePresetChunk::read(p));
        case ChunkType::Sequence: return static_cast<bool>(SequenceView::read(p));
        case ChunkType::Timeline: return static_cast<bool>(TimelineView::read(p));
        case ChunkType::BehaviorTree: return static_cast<bool>(BtView::read(p));
        case ChunkType::Skeleton2D: return static_cast<bool>(Skeleton2DView::read(p));
        case ChunkType::Shader: return static_cast<bool>(ShaderView::read(p));
        case ChunkType::Mesh: return static_cast<bool>(MeshView::read(p));
        case ChunkType::Scene: return static_cast<bool>(SceneView::read(p));
        case ChunkType::ModInfo: return static_cast<bool>(ModInfoView::read(p));
        default: return true;
    }
}

bool has_reader(ChunkType t) {
    switch (t) {
        case ChunkType::Manifest: case ChunkType::Texture: case ChunkType::Sfx: case ChunkType::Video:
        case ChunkType::Script: case ChunkType::Font: case ChunkType::Material: case ChunkType::Tilemap:
        case ChunkType::AnimGraph: case ChunkType::Terrain: case ChunkType::ParticlePreset: case ChunkType::Sequence:
        case ChunkType::Timeline: case ChunkType::BehaviorTree: case ChunkType::Skeleton2D: case ChunkType::Shader:
        case ChunkType::Mesh: case ChunkType::Scene: case ChunkType::ModInfo: return true;
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
    for (const char* sub : {"samples", "projects"}) {
        if (!fs::exists(root / sub)) continue;
        for (const auto& e : fs::recursive_directory_iterator(root / sub))
            if (e.is_regular_file() && e.path().filename() == "assets.mce") packs.push_back(e.path());
    }
    std::sort(packs.begin(), packs.end());
    REQUIRE_MESSAGE(!packs.empty(), "no assets.mce under " << root);

    std::map<std::string, u32> chunks_by_type;
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
            if (!has_reader(type)) continue;
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
    // The corpus must be broad enough to mean something.
    CHECK(chunks_by_type.size() >= 10);
    CHECK(mutants > 1000);
}
