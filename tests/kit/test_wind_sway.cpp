// WindSway (M-WINDSWAY step 1): Kit3DExtract scales the wind fields of the Foliage3D and TerrainFoliage3D batches by
// Weather.wind only when the scene has both a Weather and a WindSway. Without either the batch fields are the authored
// ones; the foliage components are never written, the state hash is unchanged by an extract, and a wind change does not
// re-scatter (the batch version stays). WindSway is saved and hash-stable.

#include <doctest/doctest.h>

#include <string>

#include "mce/asset/asset_system.h"
#include "mce/containers/array.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_sway.h"
#include "mce/render/scene3d.h"
#include "cook_test_util.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f64 kDt = 1.0 / 60.0;
const asset::Guid kMesh{0x3d00000000000001ull, 0x8000000000000001ull};
constexpr const char* kTerrainGuid = "7e7a0000-0000-4000-8000-0000000000c1";

std::string base64(const std::string& bytes) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8) | u8(bytes[i + 2]);
        for (int s = 18; s >= 0; s -= 6) out.push_back(k[(v >> s) & 63]);
    }
    if (bytes.size() - i == 2) {
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8);
        out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], '='};
    }
    return out;
}

// The wind fields of one batch as Kit3DExtract hands them over.
struct Wind {
    bool found = false;
    f32 amplitude = 0, frequency = 0, gust = 0, flutter = 0, height = 0;
    math::vec2 direction{0, 0};
    u32 version = 0;
    usize count = 0;
};

struct Rig {
    asset::AssetSystem assets;  // first: the extract releases its terrain handle when it is destroyed
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    Entity foliage, terrain;
    Entity weather_e, sway_e;
    u64 tick = 0;

    Rig() {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());

        const std::string src = cook_test::fresh_dir("wind_sway_terrain");
        std::string bytes;
        for (u32 i = 0; i < 33 * 33; ++i) cook_test::put16le(bytes, 32768);
        cook_test::write_file(src + "/assets/flat.mcterrain", R"({ "format": "mce-terrain", "version": 1,
            "guid": ")" + std::string(kTerrainGuid) + R"(", "resolution": 33, "chunk": 16, "size": [32, 32],
            "height_range": [0, 4], "heights": ")" + base64(bytes) + "\" }");
        cook::CookService svc;
        REQUIRE(svc.cook(StringView(src.c_str()), cook::CookOptions{}).ok());
        const std::string cooked = src + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        extract.reset(&assets);
        extract.bind_mesh(kMesh, 1);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});

        Foliage3D f;
        f.mesh.guid = kMesh;
        f.count = 40;
        f.seed = 3;
        f.half_extents = {4, 4};
        f.wind_amplitude = 0.2f;
        f.wind_frequency = 0.5f;
        f.wind_direction = {0.0f, 1.0f};
        f.wind_height = 3.0f;
        f.wind_gust = 0.25f;
        f.wind_flutter = 0.04f;
        foliage = world.spawn_with(LocalTransform{{0, 0, -10}, math::quat::identity(), {1, 1, 1}}, f);

        Terrain3D t;
        t.terrain.guid = asset::parse_guid(kTerrainGuid).value();
        TerrainFoliage3D tf;
        tf.mesh.guid = kMesh;
        tf.density = 0.5f;
        tf.wind_amplitude = 0.1f;
        tf.wind_frequency = 0.4f;
        tf.wind_direction = {1.0f, 0.0f};
        tf.wind_height = 2.0f;
        tf.wind_gust = 0.7f;
        tf.wind_flutter = 0.05f;
        terrain = world.spawn_with(LocalTransform{{-16, 0, -40}, math::quat::identity(), {1, 1, 1}}, t, tf);
        step();
    }
    void step() { world.tick(kDt, tick++); }
    void add_weather(f32 wind) {
        Weather w;
        w.wind = wind;
        weather_e = world.spawn_with(SaveKey{1}, w);
    }
    void add_sway(WindSway s = {}) { sway_e = world.spawn_with(SaveKey{2}, s); }
    void set_wind(f32 wind) { world.try_get_mut<Weather>(weather_e)->wind = wind; }
    // Extracts one frame and returns the Foliage3D batch (terrain = false) or the TerrainFoliage3D batch (true).
    Wind batch(bool terrain_batch) {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        Wind w;
        for (const render::InstanceBatch& b : s.batches()) {
            if (((b.id & 0x80000000u) != 0) != terrain_batch) continue;
            w.found = true;
            w.amplitude = b.wind_amplitude;
            w.frequency = b.wind_frequency;
            w.gust = b.wind_ext.y;
            w.flutter = b.wind_ext.z;
            w.height = b.wind_ext.x;
            w.direction = b.wind_direction;
            w.version = b.version;
            w.count = b.transforms.size();
        }
        return w;
    }
};

void check_authored(const Wind& w, bool terrain) {
    REQUIRE(w.found);
    CHECK(w.count > 0);
    CHECK(w.amplitude == (terrain ? 0.1f : 0.2f));
    CHECK(w.frequency == (terrain ? 0.4f : 0.5f));
    CHECK(w.gust == (terrain ? 0.7f : 0.25f));
    CHECK(w.flutter == (terrain ? 0.05f : 0.04f));
    CHECK(w.height == (terrain ? 2.0f : 3.0f));
}

}  // namespace

TEST_CASE("wind_sway: no WindSway, or no Weather, leaves the batch fields exactly as authored") {
    Rig none;
    check_authored(none.batch(false), false);
    check_authored(none.batch(true), true);

    Rig weather_only;
    weather_only.add_weather(1.0f);
    check_authored(weather_only.batch(false), false);
    check_authored(weather_only.batch(true), true);

    Rig sway_only;
    sway_only.add_sway();
    check_authored(sway_only.batch(false), false);
    check_authored(sway_only.batch(true), true);
}

TEST_CASE("wind_sway: with Weather and WindSway the batch fields scale by Weather.wind (pinned at 0, 0.5, 1)") {
    Rig r;
    r.add_weather(0.0f);
    r.add_sway();  // defaults: amplitude 4, frequency 1.5, gust 0.6, flutter 3

    // wind 0: the authored look times 1.
    check_authored(r.batch(false), false);
    check_authored(r.batch(true), true);

    r.set_wind(0.5f);
    {
        const Wind f = r.batch(false);
        REQUIRE(f.found);
        CHECK(f.amplitude == doctest::Approx(0.2f * 2.5f));
        CHECK(f.frequency == doctest::Approx(0.5f * 1.25f));
        CHECK(f.gust == doctest::Approx(0.25f + 0.3f));
        CHECK(f.flutter == doctest::Approx(0.04f * 2.0f));
        CHECK(f.height == 3.0f);
        CHECK(f.direction.x == 0.0f);  // the authored compass is kept
        CHECK(f.direction.y == 1.0f);
        const Wind t = r.batch(true);
        REQUIRE(t.found);
        CHECK(t.amplitude == doctest::Approx(0.1f * 2.5f));
        CHECK(t.frequency == doctest::Approx(0.4f * 1.25f));
        CHECK(t.gust == doctest::Approx(1.0f));  // 0.7 + 0.3
        CHECK(t.flutter == doctest::Approx(0.05f * 2.0f));
        CHECK(t.height == 2.0f);
        CHECK(t.direction.x == 1.0f);
        CHECK(t.direction.y == 0.0f);
    }

    r.set_wind(1.0f);
    {
        const Wind f = r.batch(false);
        CHECK(f.amplitude == doctest::Approx(0.2f * 4.0f));
        CHECK(f.frequency == doctest::Approx(0.5f * 1.5f));
        CHECK(f.gust == doctest::Approx(0.85f));
        CHECK(f.flutter == doctest::Approx(0.04f * 3.0f));
        const Wind t = r.batch(true);
        CHECK(t.amplitude == doctest::Approx(0.1f * 4.0f));
        CHECK(t.frequency == doctest::Approx(0.4f * 1.5f));
        CHECK(t.gust == doctest::Approx(1.0f));  // 0.7 + 0.6 clamps to 1
        CHECK(t.flutter == doctest::Approx(0.05f * 3.0f));
    }
}

TEST_CASE("wind_sway: custom gains, and a gain of 1 (or gust 0) changes nothing") {
    Rig r;
    r.add_weather(0.5f);
    WindSway s;
    s.amplitude_gain = 1.0f;
    s.frequency_gain = 1.0f;
    s.gust_gain = 0.0f;
    s.flutter_gain = 1.0f;
    r.add_sway(s);
    check_authored(r.batch(false), false);
    check_authored(r.batch(true), true);

    WindSway* w = r.world.try_get_mut<WindSway>(r.sway_e);
    w->amplitude_gain = 0.0f;  // a calm: sway dies out at wind 1
    w->gust_gain = -0.5f;
    r.set_wind(1.0f);
    const Wind f = r.batch(false);
    CHECK(f.amplitude == doctest::Approx(0.0f));
    CHECK(f.gust == doctest::Approx(0.0f));  // 0.25 - 0.5 clamps to 0
}

TEST_CASE("wind_sway: the components stay authored, the state hash is unchanged, and a wind change does not re-scatter") {
    Rig r;
    r.add_weather(0.0f);
    r.add_sway();
    const Wind calm = r.batch(false);
    const Wind calm_terrain = r.batch(true);
    REQUIRE(calm.found);
    REQUIRE(calm_terrain.found);

    r.set_wind(1.0f);
    const u64 hash = r.world.state_hash();
    const Wind storm = r.batch(false);
    const Wind storm_terrain = r.batch(true);
    CHECK(storm.amplitude > calm.amplitude);
    CHECK(r.world.state_hash() == hash);  // extraction does not touch the world

    const Foliage3D* f = r.world.try_get<Foliage3D>(r.foliage);
    REQUIRE(f);
    CHECK(f->wind_amplitude == 0.2f);
    CHECK(f->wind_frequency == 0.5f);
    CHECK(f->wind_gust == 0.25f);
    CHECK(f->wind_flutter == 0.04f);
    const TerrainFoliage3D* tf = r.world.try_get<TerrainFoliage3D>(r.terrain);
    REQUIRE(tf);
    CHECK(tf->wind_amplitude == 0.1f);
    CHECK(tf->wind_frequency == 0.4f);
    CHECK(tf->wind_gust == 0.7f);
    CHECK(tf->wind_flutter == 0.05f);

    // The scatter cache keys on the version: a wind change must not bump it.
    CHECK(storm.version == calm.version);
    CHECK(storm_terrain.version == calm_terrain.version);
    CHECK(storm.count == calm.count);
    CHECK(storm_terrain.count == calm_terrain.count);
    r.set_wind(0.3f);
    CHECK(r.batch(false).version == calm.version);
    CHECK(r.batch(true).version == calm_terrain.version);
}

TEST_CASE("wind_sway: saved and loaded, and the state hash is stable") {
    Array<byte> bytes;
    u64 hash = 0;
    {
        Rig a;
        a.add_weather(0.5f);
        WindSway s;
        s.amplitude_gain = 6.0f;
        s.frequency_gain = 2.0f;
        s.gust_gain = 0.3f;
        s.flutter_gain = 5.0f;
        a.add_sway(s);
        hash = a.world.state_hash();
        CHECK(a.world.state_hash() == hash);
        save::write_save(a.world, {}, bytes);
    }
    Rig b;
    b.add_weather(0.5f);
    b.add_sway();  // defaults, replaced by the save
    REQUIRE(save::read_save(b.world, bytes.span()));
    WindSway got;
    b.world.query<const WindSway&>().each([&](const WindSway& c) { got = c; });
    CHECK(got.amplitude_gain == 6.0f);
    CHECK(got.frequency_gain == 2.0f);
    CHECK(got.gust_gain == 0.3f);
    CHECK(got.flutter_gain == 5.0f);
    CHECK(b.world.state_hash() == hash);
}
