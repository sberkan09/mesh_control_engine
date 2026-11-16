// WindGust in Kit3DExtract (M-WINDGUST 12b): foliage sway and the snow lee bias read the gusted wind; without a
// WindGust (or with `enabled` 0, the bit clear or a zero level) the extract output equals the old value bit for bit;
// the extract only reads (the state hash is unchanged by it) and never re-scatters (the batch version stays). Clouds
// are not gusted (the render side computes the drift as speed * time), so the cloud speed stays wind * speed_scale.

#include <doctest/doctest.h>

#include <bit>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_gust.h"
#include "mce/kit/wind_sway.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f64 kDt = 1.0 / 60.0;
const asset::Guid kMesh{0x3d00000000000001ull, 0x8000000000000001ull};

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

struct Out {
    bool found = false;
    f32 amplitude = 0, frequency = 0, gust = 0, flutter = 0;
    u32 version = 0;
    f32 bias = 0;
    f32 cloud_speed = 0;
};

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    Entity gust_e;
    u64 tick = 0;
    static constexpr f32 kWind = 0.5f;

    explicit Rig(bool with_gust) {
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
        f.wind_amplitude = 0.2f;
        f.wind_frequency = 0.5f;
        f.wind_gust = 0.25f;
        f.wind_flutter = 0.04f;
        world.spawn_with(LocalTransform{{0, 0, -10}, math::quat::identity(), {1, 1, 1}}, f);
        world.tick(kDt, tick++);  // before the weather exists, so nothing moves the wind
        Weather w;
        w.wind = kWind;
        world.spawn_with(SaveKey{1}, w);
        world.spawn_with(SaveKey{2}, WindSway{});
        SnowCover sc;
        sc.wind_bias = 0.5f;
        world.spawn_with(SaveKey{3}, sc);
        world.spawn_with(SaveKey{4}, CloudLayer{});
        if (with_gust) {
            WindGust g;
            g.strength = 0.6f;
            gust_e = world.spawn_with(SaveKey{5}, g);
        }
    }
    // Sets the level directly (no tick runs, so the system does not overwrite it).
    void level(u32 q) { world.try_get_mut<WindGust>(gust_e)->gust_q = q; }
    WindGust& gust() { return *world.try_get_mut<WindGust>(gust_e); }
    Out extract_once() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        Out o;
        for (const render::InstanceBatch& b : s.batches()) {
            o.found = true;
            o.amplitude = b.wind_amplitude;
            o.frequency = b.wind_frequency;
            o.gust = b.wind_ext.y;
            o.flutter = b.wind_ext.z;
            o.version = b.version;
        }
        o.bias = s.snow_cover().wind_bias;
        o.cloud_speed = s.clouds().speed;
        return o;
    }
};

bool same(const Out& a, const Out& b) {
    return same_bits(a.amplitude, b.amplitude) && same_bits(a.frequency, b.frequency) && same_bits(a.gust, b.gust) &&
           same_bits(a.flutter, b.flutter) && same_bits(a.bias, b.bias) && same_bits(a.cloud_speed, b.cloud_speed) &&
           a.version == b.version;
}

constexpr u32 kHalf = 1u << 23;

}  // namespace

TEST_CASE("wind_gust_extract: without a gust (or a zero level) the extract equals the old value") {
    Rig none(false);
    const Out base = none.extract_once();
    REQUIRE(base.found);
    CHECK(base.bias == 0.5f);
    CHECK(base.amplitude == doctest::Approx(0.2f * 2.5f));

    Rig zero(true);  // a WindGust at level 0
    CHECK(same(zero.extract_once(), base));
}

TEST_CASE("wind_gust_extract: a gust changes the sway batch fields and the snow bias, matching the helper") {
    Rig base_rig(false);
    const Out base = base_rig.extract_once();

    Rig r(true);
    r.level(kHalf);
    const Out o = r.extract_once();
    REQUIRE(o.found);
    Weather w;
    w.wind = Rig::kWind;
    const f32 ew = effective_wind(w, &r.gust(), kGustSway);
    CHECK(ew > Rig::kWind);
    CHECK(o.amplitude != base.amplitude);
    CHECK(o.frequency != base.frequency);
    CHECK(o.amplitude == doctest::Approx(0.2f * (1.0f + 3.0f * ew)));
    CHECK(o.frequency == doctest::Approx(0.5f * (1.0f + 0.5f * ew)));
    CHECK(o.flutter == doctest::Approx(0.04f * (1.0f + 2.0f * ew)));
    CHECK(o.gust == doctest::Approx(0.25f + 0.6f * ew));
    CHECK(o.bias == doctest::Approx(0.5f * (1.0f + 0.6f * 0.5f)));
    CHECK(o.bias != base.bias);
    CHECK(o.version == base.version);  // no re-scatter
    CHECK(o.cloud_speed == base.cloud_speed);  // clouds are not gusted

    r.level(1u << 24);  // full gust
    CHECK(r.extract_once().bias == doctest::Approx(0.5f * 1.6f));
}

TEST_CASE("wind_gust_extract: the affects bits switch each consumer off, enabled 0 switches both off") {
    Rig base_rig(false);
    const Out base = base_rig.extract_once();

    Rig r(true);
    r.level(kHalf);
    const Out both = r.extract_once();

    r.gust().affects = kGustAll & ~kGustSway;
    const Out no_sway = r.extract_once();
    CHECK(same_bits(no_sway.amplitude, base.amplitude));
    CHECK(same_bits(no_sway.gust, base.gust));
    CHECK(same_bits(no_sway.bias, both.bias));

    r.gust().affects = kGustAll & ~kGustSnow;
    const Out no_snow = r.extract_once();
    CHECK(same_bits(no_snow.bias, base.bias));
    CHECK(same_bits(no_snow.amplitude, both.amplitude));

    r.gust().affects = kGustAll;
    r.gust().enabled = 0;
    CHECK(same(r.extract_once(), base));
    r.gust().enabled = 1;
    r.gust().affects = 0;
    CHECK(same(r.extract_once(), base));
}

TEST_CASE("wind_gust_extract: the extract only reads (state hash unchanged)") {
    Rig r(true);
    r.level(kHalf);
    const u64 before = r.world.state_hash();
    r.extract_once();
    r.extract_once();
    CHECK(r.world.state_hash() == before);
}
