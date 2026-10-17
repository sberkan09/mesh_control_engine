// ChunkLod3D (M-OPENWORLD A4) without a GPU: a streamed area that is not loaded draws its proxy, beyond
// impostor_distance its impostor, beyond max_distance nothing; a loaded area hides it.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/kit/chunk_lod3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/scene/streaming.h"
#include "mesh_fixture.h"

using namespace mce;
using math::vec3;

namespace {

const asset::Guid kProxy{0x4d00000000000001ull, 0x8000000000000001ull};
const asset::Guid kImpostor{0x4d00000000000002ull, 0x8000000000000001ull};

struct Fixture {
    render::MeshStore store;
    World world;
    kit::Kit3DExtract extract;
    Entity chunk;

    Fixture() {
        for (u32 key : {1u, 2u}) {
            mesh_fixture::Builder box;
            box.add_node(-1);
            mesh_fixture::Box b;
            b.half = {10.0f, 5.0f, 10.0f};
            box.add_box(b);
            const Array<byte> blob = box.build();
            REQUIRE(store.upload(key, 1, blob.span()).has_value());
        }
        kit::install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        extract.bind_mesh(kProxy, 1);
        extract.bind_mesh(kImpostor, 2);
        world.spawn_with(LocalTransform{}, kit::Camera3DComponent{});  // at the origin, looking down -Z
        scene::SceneChunk sc;
        sc.half_extents = {10.0f, 10.0f, 10.0f};
        kit::ChunkLod3D lod;
        lod.proxy.guid = kProxy;
        lod.impostor.guid = kImpostor;
        lod.impostor_distance = 200.0f;
        lod.max_distance = 1000.0f;
        chunk = world.spawn_with(LocalTransform{{0, 0, -100}, math::quat::identity(), {1, 1, 1}}, sc, lod,
                                 scene::SceneChunkStatus{});
        world.tick(1.0 / 60.0, 0);
    }
    // One frame: the mesh key of the one ChunkLod3D draw, or ~0u when nothing drew.
    u32 frame() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        cam.far_z = 5000.0f;
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        const u32 drawn = extract.stats().chunk_proxies + extract.stats().chunk_impostors;
        REQUIRE(drawn <= 1);
        if (drawn == 0) return ~0u;
        REQUIRE(!s.instances().empty());
        return s.instances().back().mesh;
    }
    void place(f32 z) { world.try_get_mut<LocalTransform>(chunk)->position = {0, 0, z}; world.tick(1.0 / 60.0, 1); }
};

}  // namespace

TEST_CASE("chunk lod: the proxy near, the impostor far, nothing past max_distance, hidden while loaded") {
    Fixture f;
    CHECK(f.frame() == 1u);  // 90 m to the box: the proxy
    CHECK(f.extract.stats().chunk_proxies == 1);
    CHECK(f.world.try_get<LocalTransform>(f.chunk)->position.z == -100.0f);

    f.place(-400.0f);  // 390 m: the impostor
    CHECK(f.frame() == 2u);
    CHECK(f.extract.stats().chunk_impostors == 1);

    f.place(-1200.0f);  // past max_distance
    CHECK(f.frame() == ~0u);

    f.place(-100.0f);
    f.world.try_get_mut<scene::SceneChunkStatus>(f.chunk)->state = scene::ChunkState::Loaded;  // its entities draw
    CHECK(f.frame() == ~0u);
    f.world.try_get_mut<scene::SceneChunkStatus>(f.chunk)->state = scene::ChunkState::Loading;  // no hole while it loads
    CHECK(f.frame() == 1u);

    f.place(-400.0f);
    f.world.try_get_mut<kit::ChunkLod3D>(f.chunk)->impostor.guid = asset::Guid{};  // no impostor: the proxy stays
    CHECK(f.frame() == 1u);
    f.world.try_get_mut<kit::ChunkLod3D>(f.chunk)->enabled = 0;
    CHECK(f.frame() == ~0u);
}

TEST_CASE("chunk lod: a world without ChunkLod3D draws exactly as before") {
    render::MeshStore store;
    World world;
    kit::install_kit3d(world, nullptr);
    kit::Kit3DExtract extract;
    extract.init(store, nullptr);
    world.spawn_with(LocalTransform{}, kit::Camera3DComponent{});
    world.tick(1.0 / 60.0, 0);
    render::Camera3D cam;
    REQUIRE(extract.camera(world, 1.0f, cam));
    render::Scene3D s(store, mem::heap());
    s.begin(cam, {320, 180});
    extract.extract(world, s, 1.0f);
    CHECK(extract.stats().chunk_proxies == 0);
    CHECK(extract.stats().chunk_impostors == 0);
    CHECK(s.instances().empty());
}
