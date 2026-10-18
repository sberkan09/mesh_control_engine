// HLOD city (M-OPENWORLD A4): a procedural city of streamed areas, extracted without a GPU twice: once with every area
// loaded (each area's buildings merged per cell by StaticMerge3D), once with only the areas near the camera loaded
// and every other area drawn as its HLOD proxy (ChunkLod3D, the area's pieces merged into one mesh, as hlod_bake
// writes it). Prints the draws, instances, triangles and CPU time of both and checks that the HLOD frame submits
// under half the draws over a quarter of the entities. Counters, not GPU time: frame time on a GPU is A1's measurement.
//
//   sample_hlod_city -blocks 8 -near 150

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "mce/asset/asset_system.h"
#include "mce/asset/container.h"
#include "mce/asset/formats.h"
#include "mce/asset/xxhash.h"
#include "mce/core.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/chunk_lod3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/static_merge3d.h"
#include "mce/render/scene3d.h"
#include "mce/render/static_merge.h"
#include "mce/scene/streaming.h"

using namespace mce;
using math::quat;
using math::vec3;

namespace {

MCE_CVAR(cv_blocks, "blocks", 8, "City side in areas (blocks x blocks)", CVarFlags::None);
MCE_CVAR(cv_near, "near", 150, "Areas whose centre is within this many metres of the camera stay loaded", CVarFlags::None);

constexpr f32 kArea = 96.0f;  // metres per area side
constexpr u32 kLots = 4;      // lots per area side
constexpr f32 kLot = kArea / kLots;

struct Piece {
    asset::Guid guid;
    vec3 half;  // its body, resting on y = 0
    f32 color[3];
    Array<byte> blob;
};

// A piece resting on y = 0: one submesh of boxes (the first is the body, the others sills and ledges around it),
// quantized as the glTF importer does. With `ledges` the submesh gets one coarser level, the body alone, as a
// model's simplified LOD would be: far away the ledges are below a pixel.
Array<byte> piece_mesh(vec3 half, const f32 color[3], bool ledges) {
    struct Box {
        vec3 centre, half;
    };
    Array<Box> boxes;
    boxes.push_back({{0, half.y, 0}, half});
    if (ledges) {
        const f32 y = half.y * 0.6f;
        boxes.push_back({{0, y, half.z}, {half.x + 0.3f, 0.1f, 0.3f}});
        boxes.push_back({{0, y, -half.z}, {half.x + 0.3f, 0.1f, 0.3f}});
        boxes.push_back({{half.x, y, 0}, {0.3f, 0.1f, half.z + 0.3f}});
        boxes.push_back({{-half.x, y, 0}, {0.3f, 0.1f, half.z + 0.3f}});
    }
    vec3 lo = boxes[0].centre - boxes[0].half, hi = boxes[0].centre + boxes[0].half;
    for (const Box& b : boxes) {
        lo = math::min(lo, b.centre - b.half);
        hi = math::max(hi, b.centre + b.half);
    }
    const vec3 centre = (lo + hi) * 0.5f, extent = (hi - lo) * 0.5f;
    Array<asset::MeshVertex> vertices;
    Array<u16> indices;
    const auto snorm = [](f32 v) { return static_cast<i16>(std::lround(math::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
    const f32 axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Box& b : boxes)
        for (const auto& a : axes) {
            const vec3 n{a[0], a[1], a[2]};
            vec3 u = std::fabs(n.y) > 0.5f ? vec3{1, 0, 0} : vec3{0, 1, 0};
            const vec3 v = math::cross(n, u);
            u = math::cross(v, n);
            const f32 su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
            const u16 base = static_cast<u16>(vertices.size());
            for (int c = 0; c < 4; ++c) {
                const vec3 p = b.centre + (n + u * su[c] + v * sv[c]) * b.half;
                asset::MeshVertex q{};
                q.position[0] = snorm((p.x - centre.x) / extent.x);
                q.position[1] = snorm((p.y - centre.y) / extent.y);
                q.position[2] = snorm((p.z - centre.z) / extent.z);
                f32 ox = n.x, oy = n.y;
                if (n.z < 0) {
                    ox = (1.0f - std::fabs(n.y)) * (n.x >= 0 ? 1.0f : -1.0f);
                    oy = (1.0f - std::fabs(n.x)) * (n.y >= 0 ? 1.0f : -1.0f);
                }
                q.normal[0] = snorm(ox);
                q.normal[1] = snorm(oy);
                vertices.push_back(q);
            }
            for (const int i : {0, 1, 2, 2, 3, 0}) indices.push_back(static_cast<u16>(base + i));
        }
    asset::MeshSubmesh sm{};
    sm.index_count = static_cast<u32>(indices.size());
    sm.vertex_count = static_cast<u32>(vertices.size());
    sm.center[0] = centre.x;
    sm.center[1] = centre.y;
    sm.center[2] = centre.z;
    sm.extent[0] = extent.x;
    sm.extent[1] = extent.y;
    sm.extent[2] = extent.z;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    for (int i = 0; i < 3; ++i) sm.material.base_color[i] = color[i];
    sm.material.base_color[3] = 1.0f;
    sm.material.roughness = 0.8f;
    asset::MeshLod body{};  // level 1: the first box's 12 triangles, same vertices
    body.index_count = 36;
    body.vertex_count = sm.vertex_count;
    body.error = 0.3f;
    if (ledges) sm.reserved[0] = 1;
    asset::MeshNode node{};
    node.parent = -1;
    node.scale[0] = node.scale[1] = node.scale[2] = 1.0f;
    node.rotation[3] = 1.0f;
    asset::MeshData d;
    d.vertices = vertices.span();
    d.indices = std::as_bytes(indices.span());
    d.submeshes = Span<const asset::MeshSubmesh>(&sm, 1);
    d.nodes = Span<const asset::MeshNode>(&node, 1);
    if (ledges) d.lods = Span<const asset::MeshLod>(&body, 1);
    d.bounds_min[0] = lo.x;
    d.bounds_min[1] = lo.y;
    d.bounds_min[2] = lo.z;
    d.bounds_max[0] = hi.x;
    d.bounds_max[1] = hi.y;
    d.bounds_max[2] = hi.z;
    Array<byte> out;
    asset::write_mesh(d, out);
    return out;
}

// A cooked tree (manifest + one content-addressed container per piece) the asset system mounts: StaticMerge3D reads
// the pieces' meshes through it, as a cooked game does.
bool write_tree(const std::filesystem::path& root, const Array<Piece>& pieces) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto write = [](const fs::path& p, Span<const byte> b) {
        std::error_code e;
        fs::create_directories(p.parent_path(), e);
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
        std::fclose(f);
        return ok;
    };
    Array<asset::ManifestItem> items;
    for (const Piece& p : pieces) {
        asset::ContainerWriter w;
        asset::ContainerWriter::ChunkDesc d;
        d.type = asset::ChunkType::Mesh;
        d.version = asset::chunk_version(asset::MESH_VERSION, 0);
        d.guid = p.guid;
        w.add(d, p.blob.span());
        Array<byte> mesh;
        w.finish(mesh);
        const u64 hash = asset::xxh64(mesh.data(), mesh.size());
        const std::string hex = asset::to_hex(p.guid).c_str();
        char file[64];
        std::snprintf(file, sizeof file, "%016llx.mce", static_cast<unsigned long long>(hash));
        if (!write(root / hex.substr(0, 2) / hex / file, mesh.span())) return false;
        items.push_back(asset::ManifestItem{p.guid, hash, asset::ChunkType::Mesh, "models/piece.glb"});
    }
    Array<byte> mp;
    asset::write_manifest(Span<asset::ManifestItem>(items.data(), items.size()), mp);
    asset::ContainerWriter mw;
    asset::ContainerWriter::ChunkDesc md;
    md.type = asset::ChunkType::Manifest;
    mw.add(md, mp.span());
    Array<byte> manifest;
    mw.finish(manifest);
    return write(root / "manifest.mce", manifest.span());
}

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next(u32 n) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>((s >> 33) % n);
    }
};

// One placed piece of an area: which piece, and where (absolute).
struct Placed {
    u32 piece;
    vec3 position;
};

// The city: per area its pieces. Lots carry a stack of floors, a roof and four small props (below the HLOD bake's
// 1 m min_size, so the proxy leaves them out as hlod_bake does).
Array<Array<Placed>> make_city(u32 side) {
    Rng rng;
    Array<Array<Placed>> areas;
    const f32 half_city = static_cast<f32>(side) * kArea * 0.5f;
    for (u32 az = 0; az < side; ++az)
        for (u32 ax = 0; ax < side; ++ax) {
            Array<Placed> a;
            for (u32 lz = 0; lz < kLots; ++lz)
                for (u32 lx = 0; lx < kLots; ++lx) {
                    const f32 x = static_cast<f32>(ax) * kArea + (static_cast<f32>(lx) + 0.5f) * kLot - half_city;
                    const f32 z = static_cast<f32>(az) * kArea + (static_cast<f32>(lz) + 0.5f) * kLot - half_city;
                    const u32 floors = 2 + rng.next(6);
                    for (u32 f = 0; f < floors; ++f) a.push_back({rng.next(2), {x, static_cast<f32>(f) * 3.0f, z}});
                    a.push_back({2, {x, static_cast<f32>(floors) * 3.0f, z}});
                    for (u32 p = 0; p < 4; ++p)
                        a.push_back({3, {x + (p & 1 ? 9.5f : -9.5f), 0.0f, z + (p & 2 ? 9.5f : -9.5f)}});
                }
            areas.push_back(std::move(a));
        }
    return areas;
}

asset::Guid proxy_guid(u32 area) { return asset::Guid{0x4c00000000000000ull | area, 0x8000000000000001ull}; }

struct Counters {
    u32 draws = 0;
    u32 instances = 0;
    u32 triangles = 0;
    u32 far_triangles = 0;  // ... of them at the coarser level (the body alone)
    u32 meshes = 0;
    u32 merged_cells = 0;
    u32 proxies = 0;
    u32 loaded = 0;
    u32 entities = 0;
    f64 extract_ms = 0.0;  // extract + build of the measured frame
};

f64 now_ms() {
    using namespace std::chrono;
    return duration<f64, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Builds the world (every area loaded, or only those within `near` of the camera) and extracts two frames: the
// first merges the loaded areas' cells, the second is the steady state that is reported.
Counters run(asset::AssetSystem& assets, const Array<Piece>& pieces, const Array<Array<Placed>>& city, bool hlod, f32 near) {
    render::MeshStore store;
    for (const Piece& p : pieces) MCE_VERIFY(store.upload(assets.find(p.guid), 1, p.blob.span()).has_value());
    World world;
    ecs::install_hierarchy(world);
    kit::install_kit3d(world, &assets);
    kit::Kit3DExtract extract;
    extract.init(store, &assets);

    const vec3 eye{0.0f, 40.0f, 0.0f};
    world.spawn_with(LocalTransform{eye, quat::identity(), {1, 1, 1}}, kit::Camera3DComponent{});  // looking down -Z
    Counters c;
    const u32 side = static_cast<u32>(std::lround(std::sqrt(static_cast<f64>(city.size()))));
    const f32 half_city = static_cast<f32>(side) * kArea * 0.5f;
    Array<asset::MeshView> views;
    for (const Piece& p : pieces) views.push_back(asset::MeshView::read(p.blob.span()).value());
    u32 next_key = 100000;
    for (u32 i = 0; i < city.size(); ++i) {
        const vec3 centre{(static_cast<f32>(i % side) + 0.5f) * kArea - half_city, 0.0f,
                          (static_cast<f32>(i / side) + 0.5f) * kArea - half_city};
        const bool loaded = !hlod || math::length(vec3{centre.x - eye.x, 0.0f, centre.z - eye.z}) <= near;
        scene::SceneChunk sc;
        sc.half_extents = {kArea * 0.5f, 30.0f, kArea * 0.5f};
        scene::SceneChunkStatus status;
        status.state = loaded ? scene::ChunkState::Loaded : scene::ChunkState::Unloaded;
        if (loaded) {
            ++c.loaded;
            const Entity root = world.spawn_with(LocalTransform{}, kit::StaticMerge3D{});
            for (const Placed& p : city[i]) {
                kit::MeshRef m;
                m.mesh.guid = pieces[p.piece].guid;
                const Entity e = world.spawn_with(LocalTransform{p.position, quat::identity(), {1, 1, 1}}, m);
                world.add(e, ChildOf{root});
            }
            world.spawn_with(LocalTransform{centre, quat::identity(), {1, 1, 1}}, sc, status);
            continue;
        }
        // The area's HLOD proxy, as hlod_bake bakes it: the pieces of at least 1 m, merged relative to the centre.
        Array<render::MergeSource> sources;
        for (const Placed& p : city[i]) {
            const vec3 h = pieces[p.piece].half;
            if (2.0f * math::length(h) < 1.0f) continue;
            render::MergeSource s;
            s.mesh = &views[p.piece];
            s.world = math::translation(p.position - centre);
            sources.push_back(s);
        }
        Array<byte> blob;
        MCE_VERIFY(render::build_merged_mesh(sources.span(), blob));
        const u32 key = next_key++;
        MCE_VERIFY(store.upload(key, 1, blob.span()).has_value());
        extract.bind_mesh(proxy_guid(i), key);
        kit::ChunkLod3D lod;
        lod.proxy.guid = proxy_guid(i);
        lod.impostor_distance = 100000.0f;  // the sample counts proxies only
        world.spawn_with(LocalTransform{centre, quat::identity(), {1, 1, 1}}, sc, lod, status);
    }
    c.entities = static_cast<u32>(world.entity_count());

    for (int frame = 0; frame < 2; ++frame) {
        world.tick(1.0 / 60.0, static_cast<u64>(frame));
        ecs::propagate_transforms(world);
        render::Camera3D cam;
        MCE_VERIFY(extract.camera(world, 1.0f, cam));
        cam.far_z = 5000.0f;
        const f64 t0 = now_ms();
        render::Scene3D scene(store, mem::heap());
        scene.begin(cam, {1280, 720});
        extract.extract(world, scene, 1.0f);
        scene.build();
        c.extract_ms = now_ms() - t0;
        c.draws = static_cast<u32>(scene.draws().size());
        c.instances = static_cast<u32>(scene.instances().size());
        c.triangles = scene.stats().triangles;
        c.far_triangles = scene.stats().lod_triangles[1];
        const kit::Kit3DStats& st = extract.stats();
        c.meshes = st.meshes;
        c.merged_cells = st.merged_cells;
        c.proxies = st.chunk_proxies;
    }
    return c;
}

void print(const char* name, const Counters& c) {
    std::printf("hlod_city: %s | %u areas loaded, %u entities | draws %u, instances %u, triangles %u (%u far level) | "
                "meshes %u, merged cells %u, proxies %u | extract+build %.2f ms\n",
                name, c.loaded, c.entities, c.draws, c.instances, c.triangles, c.far_triangles, c.meshes, c.merged_cells,
                c.proxies, c.extract_ms);
}

}  // namespace

int main(int argc, char** argv) {
    log::init({.app_name = "hlod_city", .level = log::Level::Warn});
    console::parse_command_line(argc, argv);
    const u32 side = static_cast<u32>(math::clamp(cv_blocks.get_int(), 1, 64));
    const f32 near = static_cast<f32>(math::max(cv_near.get_int(), 0));

    Array<Piece> pieces;
    const f32 grey[3] = {0.55f, 0.55f, 0.58f}, brick[3] = {0.6f, 0.35f, 0.3f}, roof[3] = {0.2f, 0.22f, 0.25f},
              prop[3] = {0.3f, 0.5f, 0.3f};
    const struct {
        vec3 half;
        const f32* color;
    } kinds[] = {{{8, 1.5f, 8}, grey}, {{8, 1.5f, 8}, brick}, {{8.5f, 0.25f, 8.5f}, roof}, {{0.3f, 0.3f, 0.3f}, prop}};
    for (u32 i = 0; i < 4; ++i) {
        Piece p;
        p.guid = asset::Guid{0x4b00000000000001ull + i, 0x8000000000000001ull};
        p.half = kinds[i].half;
        for (int k = 0; k < 3; ++k) p.color[k] = kinds[i].color[k];
        p.blob = piece_mesh(p.half, p.color, i < 2);  // the floors have sills
        pieces.push_back(std::move(p));
    }
    const std::filesystem::path tree = std::filesystem::temp_directory_path() / "mce_hlod_city";
    if (!write_tree(tree, pieces)) {
        std::fprintf(stderr, "hlod_city: cannot write %s\n", tree.string().c_str());
        return 1;
    }
    asset::AssetSystem assets;
    const std::string root = tree.generic_string();
    const StringView roots[] = {StringView(root.c_str())};
    if (!assets.mount(Span<const StringView>(roots, 1))) {
        std::fprintf(stderr, "hlod_city: cannot mount %s\n", root.c_str());
        return 1;
    }

    const Array<Array<Placed>> city = make_city(side);
    usize total = 0;
    for (const Array<Placed>& a : city) total += a.size();
    std::printf("hlod_city: %u x %u areas of %.0f m, %zu pieces\n", side, side, static_cast<f64>(kArea), total);
    const Counters off = run(assets, pieces, city, false, near);
    const Counters on = run(assets, pieces, city, true, near);
    print("hlod off", off);
    print("hlod on ", on);
    // The gate is the submission: fewer draws and entities. Triangles are reported, not gated: an area proxy is culled
    // as one box (a cell of the loaded area is 32 m), so it can draw more triangles than the cells it stands for until
    // the cook's simplified far levels (the hlod importer's lod_levels) cut them; these pieces carry one level only.
    const bool ok = on.proxies > 0 && off.proxies == 0 && on.draws * 2 < off.draws && on.entities * 4 < off.entities;
    std::printf("hlod_city: %s (draws %.2fx, triangles %.2fx, extract+build %.2fx)\n", ok ? "ok" : "FAILED",
                static_cast<f64>(on.draws) / math::max(1.0, static_cast<f64>(off.draws)),
                static_cast<f64>(on.triangles) / math::max(1.0, static_cast<f64>(off.triangles)),
                on.extract_ms / math::max(1e-6, off.extract_ms));
    std::error_code ec;
    std::filesystem::remove_all(tree, ec);
    log::shutdown();
    return ok ? 0 : 1;
}
