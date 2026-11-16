// WindDirection in Kit3DExtract (M-WINDDIR 8d): the snow carrier's wind_dir follows the heading (default +x without a
// WindDirection), and a Foliage3D batch's wind_direction is rotated by the heading relative to +x, only when the scene
// has both a Weather and a WindDirection. Batch fields only: a heading change never re-scatters (version unchanged).

#include <doctest/doctest.h>

#include <cmath>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_direction.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f64 kDt = 1.0 / 60.0;
const asset::Guid kMesh{0x3d00000000000001ull, 0x8000000000000001ull};

struct Out {
    render::SnowCover snow;
    bool found = false;
    math::vec2 dir{0, 0};
    u32 version = 0;
    usize count = 0;
};

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    Entity wind_e;
    u64 tick = 0;

    explicit Rig(bool weather, bool wind, f32 degrees = 0.0f, math::vec2 authored = {1.0f, 0.0f}) {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        extract.bind_mesh(kMesh, 1);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        Foliage3D f;
        f.mesh.guid = kMesh;
        f.count = 40;
        f.seed = 3;
        f.half_extents = {4, 4};
        f.wind_direction = authored;
        world.spawn_with(LocalTransform{{0, 0, -10}, math::quat::identity(), {1, 1, 1}}, f);
        if (weather) world.spawn_with(SaveKey{1}, Weather{});
        SnowCover sc;
        sc.wind_bias = 0.5f;
        world.spawn_with(SaveKey{2}, sc);
        if (wind) {
            WindDirection w;
            w.degrees = degrees;
            wind_e = world.spawn_with(SaveKey{3}, w);
        }
        step();
    }
    void step() { world.tick(kDt, tick++); }
    void set_degrees(f32 d) {
        world.try_get_mut<WindDirection>(wind_e)->degrees = d;
        step();
    }
    Out extract_once() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        Out o;
        o.snow = s.snow_cover();
        for (const render::InstanceBatch& b : s.batches()) {
            o.found = true;
            o.dir = b.wind_direction;
            o.version = b.version;
            o.count = b.transforms.size();
        }
        return o;
    }
};

constexpr f32 kEps = 1e-6f;

}  // namespace

TEST_CASE("wind_direction_extract: no WindDirection leaves the snow at +x and the foliage as authored") {
    Rig r(true, false, 0.0f, {0.0f, 1.0f});
    const Out o = r.extract_once();
    CHECK(o.snow.wind_dir.x == 1.0f);
    CHECK(o.snow.wind_dir.y == 0.0f);
    REQUIRE(o.found);
    CHECK(o.dir.x == 0.0f);
    CHECK(o.dir.y == 1.0f);
}

TEST_CASE("wind_direction_extract: the snow wind_dir follows the heading") {
    {
        Rig r(true, true, 0.0f);
        const Out o = r.extract_once();
        CHECK(o.snow.wind_dir.x == doctest::Approx(1.0f).epsilon(kEps));
        CHECK(o.snow.wind_dir.y == doctest::Approx(0.0f).epsilon(kEps));
    }
    {
        Rig r(true, true, 180.0f);
        const Out o = r.extract_once();
        CHECK(std::fabs(o.snow.wind_dir.x + 1.0f) < kEps);
        CHECK(std::fabs(o.snow.wind_dir.y) < kEps);
    }
    {
        Rig r(true, true, 90.0f);
        const Out o = r.extract_once();
        CHECK(std::fabs(o.snow.wind_dir.x) < kEps);
        CHECK(std::fabs(o.snow.wind_dir.y - 1.0f) < kEps);
    }
}

TEST_CASE("wind_direction_extract: foliage is rotated by the heading relative to +x") {
    {
        Rig r(true, true, 90.0f, {1.0f, 0.0f});  // along +x becomes the scene wind
        const Out o = r.extract_once();
        REQUIRE(o.found);
        CHECK(std::fabs(o.dir.x) < kEps);
        CHECK(std::fabs(o.dir.y - 1.0f) < kEps);
    }
    {
        Rig r(true, true, 90.0f, {0.0f, 1.0f});  // an offset of 90 degrees keeps it
        const Out o = r.extract_once();
        REQUIRE(o.found);
        CHECK(std::fabs(o.dir.x + 1.0f) < kEps);
        CHECK(std::fabs(o.dir.y) < kEps);
    }
}

TEST_CASE("wind_direction_extract: without a Weather the foliage is unchanged but the snow still follows") {
    Rig r(false, true, 90.0f, {1.0f, 0.0f});
    const Out o = r.extract_once();
    REQUIRE(o.found);
    CHECK(o.dir.x == 1.0f);  // the foliage turn needs a Weather as well (like WindSway)
    CHECK(o.dir.y == 0.0f);
    // The snow block itself needs a Weather to whiten, so with none the carrier is the bare default (+x).
    CHECK(o.snow.wind_dir.x == 1.0f);
    CHECK(o.snow.wind_dir.y == 0.0f);
}

TEST_CASE("wind_direction_extract: a heading change does not re-scatter, and extracts are deterministic") {
    Rig r(true, true, 0.0f);
    const Out a = r.extract_once();
    r.set_degrees(135.0f);
    const u64 hash = r.world.state_hash();
    const Out b = r.extract_once();
    REQUIRE(a.found);
    REQUIRE(b.found);
    CHECK(b.version == a.version);
    CHECK(b.count == a.count);
    CHECK(b.dir.x != a.dir.x);
    CHECK(r.world.state_hash() == hash);
    const Out c = r.extract_once();
    CHECK(c.dir.x == b.dir.x);
    CHECK(c.dir.y == b.dir.y);
    CHECK(c.snow.wind_dir.x == b.snow.wind_dir.x);
    CHECK(c.snow.wind_dir.y == b.snow.wind_dir.y);
    CHECK(c.version == b.version);
}
