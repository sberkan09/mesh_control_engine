// Extract feed of the seasons (M-SEASONS 19b): Kit3DExtract multiplies the tint of the Foliage3D and TerrainFoliage3D
// batches by season_foliage_tint (after foliage_look and apply_wind_sway, rgb only) and raises the local
// render::SnowCover by the season's drawn floor and lee shift, only where a Weather and a SnowCover exist. No Season,
// enabled 0, strength 0 and summer leave batches and snow bit-identical. Density thinning is dropped (the terrain
// scatter is row-major, so a prefix is not an unbiased thinning): the batch span is the full cached scatter. Foliage3D,
// TerrainFoliage3D, SnowCover, Weather and WorldClock are never written, and a day advance does not re-scatter.

#include <doctest/doctest.h>

#include <bit>
#include <string>

#include "mce/asset/asset_system.h"
#include "mce/containers/array.h"
#include "mce/convar.h"
#include "mce/cook/cook.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/season.h"
#include "mce/kit/snow_cover.h"
#include "mce/kit/weather.h"
#include "mce/kit/wind_sway.h"
#include "mce/kit/world_clock.h"
#include "mce/render/scene3d.h"
#include "mce/string_id.h"
#include "cook_test_util.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f64 kDt = 1.0 / 60.0;
const asset::Guid kMesh{0x3d00000000000001ull, 0x8000000000000001ull};
constexpr const char* kTerrainGuid = "7e7a0000-0000-4000-8000-0000000000c2";
const math::vec4 kAuthoredTint{0.9f, 0.8f, 0.7f, 0.6f};
const math::vec4 kVariation{0.5f, 0.6f, 0.7f, 0.8f};

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_v4(const math::vec4& a, const math::vec4& b) {
    return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z) && same_bits(a.w, b.w);
}

template <class T>
u32 bytes_hash(const T& v) {
    u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
    for (usize i = 0; i < sizeof(T); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

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

// One batch as Kit3DExtract hands it over.
struct Look {
    bool found = false;
    math::vec4 tint{0, 0, 0, 0};
    math::vec4 variation{0, 0, 0, 0};
    math::vec4 wind_ext{0, 0, 0, 0};
    f32 amplitude = 0, frequency = 0;
    u32 version = 0;
    usize count = 0;
};

bool same_look(const Look& a, const Look& b) {
    return a.found == b.found && same_v4(a.tint, b.tint) && same_v4(a.variation, b.variation) &&
           same_v4(a.wind_ext, b.wind_ext) && same_bits(a.amplitude, b.amplitude) && same_bits(a.frequency, b.frequency) &&
           a.version == b.version && a.count == b.count;
}

struct Rig {
    asset::AssetSystem assets;  // first: the extract releases its terrain handle when it is destroyed
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    Entity foliage, terrain, season_e, clock_e, weather_e, snow_e;
    render::SnowCover last_snow;
    u64 tick = 0;

    Rig() {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());

        const std::string src = cook_test::fresh_dir("season_extract_terrain");
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
        f.tint = kAuthoredTint;
        f.tint_variation = kVariation;
        f.wind_amplitude = 0.2f;
        f.wind_height = 3.0f;
        foliage = world.spawn_with(LocalTransform{{0, 0, -10}, math::quat::identity(), {1, 1, 1}}, f);

        Terrain3D t;
        t.terrain.guid = asset::parse_guid(kTerrainGuid).value();
        TerrainFoliage3D tf;
        tf.mesh.guid = kMesh;
        tf.density = 0.5f;
        tf.tint = kAuthoredTint;
        tf.tint_variation = kVariation;
        tf.wind_amplitude = 0.1f;
        terrain = world.spawn_with(LocalTransform{{-16, 0, -40}, math::quat::identity(), {1, 1, 1}}, t, tf);
        world.tick(kDt, tick++);
    }
    // A Season whose day-0 hour-0 phase is `phase` (no clock needed), 12 days a year.
    void add_season(f32 phase, Season s = {}) {
        s.start_phase = phase;
        season_e = world.spawn_with(SaveKey{1}, s);
    }
    Season& season() { return *world.try_get_mut<Season>(season_e); }
    void add_clock(u32 day, f32 hour = 0.0f) {
        WorldClock c;
        c.day = day;
        c.hour = hour;
        clock_e = world.spawn_with(SaveKey{2}, c);
    }
    void set_day(u32 day) { world.try_get_mut<WorldClock>(clock_e)->day = day; }
    void add_weather(WeatherKind k = WeatherKind::Clear, f32 intensity = 0.0f) {
        Weather w;
        w.current = k;
        w.target = k;
        w.intensity = intensity;
        weather_e = world.spawn_with(SaveKey{3}, w);
    }
    void add_snow(SnowCover s = {}) { snow_e = world.spawn_with(SaveKey{4}, s); }
    void add_sway() { world.spawn_with(SaveKey{5}, WindSway{}); }

    // Extracts one frame and returns the Foliage3D batch (terrain = false) or the TerrainFoliage3D batch (true).
    Look batch(bool terrain_batch) {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        last_snow = s.snow_cover();
        Look w;
        for (const render::InstanceBatch& b : s.batches()) {
            if (((b.id & 0x80000000u) != 0) != terrain_batch) continue;
            w.found = true;
            w.tint = b.tint;
            w.variation = b.tint_variation;
            w.wind_ext = b.wind_ext;
            w.amplitude = b.wind_amplitude;
            w.frequency = b.wind_frequency;
            w.version = b.version;
            w.count = b.transforms.size();
        }
        return w;
    }
    // Both kinds and the snow of one frame.
    struct Frame {
        Look foliage, terrain;
        render::SnowCover snow;
    };
    Frame frame() {
        Frame f;
        f.foliage = batch(false);
        f.terrain = batch(true);
        f.snow = last_snow;
        return f;
    }
};

bool same_snow(const render::SnowCover& a, const render::SnowCover& b) {
    return same_bits(a.cover, b.cover) && same_bits(a.coverage, b.coverage) && same_bits(a.noise_scale, b.noise_scale) &&
           same_bits(a.wind_bias, b.wind_bias) && a.noise_enabled == b.noise_enabled;
}

math::vec4 times(const math::vec4& t, const math::vec3& m) { return {t.x * m.x, t.y * m.y, t.z * m.z, t.w}; }

}  // namespace

TEST_CASE("season extract: no Season leaves the batches and the snow as authored") {
    Rig r;
    r.add_weather();
    r.add_snow();
    const Rig::Frame f = r.frame();
    REQUIRE(f.foliage.found);
    REQUIRE(f.terrain.found);
    CHECK(same_v4(f.foliage.tint, kAuthoredTint));
    CHECK(same_v4(f.terrain.tint, kAuthoredTint));
    CHECK(same_v4(f.foliage.variation, kVariation));
    CHECK(f.foliage.count > 0);
    CHECK(f.terrain.count > 0);
}

TEST_CASE("season extract: enabled 0, strength 0 and summer are bit-identical to no Season") {
    Rig base;
    base.add_weather();
    base.add_snow();
    const Rig::Frame a = base.frame();

    const auto same = [&](Rig& r) {
        const Rig::Frame b = r.frame();
        CHECK(same_look(a.foliage, b.foliage));
        CHECK(same_look(a.terrain, b.terrain));
        CHECK(same_snow(a.snow, b.snow));
    };
    Rig off;
    off.add_weather();
    off.add_snow();
    off.add_season(0.875f);
    off.season().enabled = 0;
    same(off);

    Rig zero;
    zero.add_weather();
    zero.add_snow();
    zero.add_season(0.875f);
    zero.season().strength = 0.0f;
    same(zero);

    Rig summer;  // the centre of summer: white tint, no snow
    summer.add_weather();
    summer.add_snow();
    summer.add_season(0.375f);
    same(summer);
}

TEST_CASE("season extract: spring, autumn and winter batches carry the helper's tint on both kinds") {
    struct Pick {
        f32 phase;
        const char* name;
    };
    for (const Pick p : {Pick{0.125f, "spring"}, Pick{0.625f, "autumn"}, Pick{0.875f, "winter"}, Pick{0.5f, "late summer"}}) {
        CAPTURE(p.name);
        Rig r;
        r.add_season(p.phase);
        const math::vec3 m = season_foliage_tint(r.season(), season_phase(r.season(), 0, 0.0f));
        const Rig::Frame f = r.frame();
        REQUIRE(f.foliage.found);
        REQUIRE(f.terrain.found);
        const math::vec4 want = times(kAuthoredTint, m);
        CHECK(same_v4(f.foliage.tint, want));
        CHECK(same_v4(f.terrain.tint, want));
        CHECK(same_bits(f.foliage.tint.w, kAuthoredTint.w));  // alpha untouched
        CHECK(same_bits(f.terrain.tint.w, kAuthoredTint.w));
        CHECK(same_v4(f.foliage.variation, kVariation));  // tint_variation as authored
        CHECK(same_v4(f.terrain.variation, kVariation));
    }
    Rig a;  // the centre of autumn is the authored autumn tint
    a.add_season(0.625f);
    const Rig::Frame f = a.frame();
    CHECK(f.foliage.tint.x == doctest::Approx(0.9f * 1.35f));
    CHECK(f.foliage.tint.y == doctest::Approx(0.8f * 0.7f));
    CHECK(f.foliage.tint.z == doctest::Approx(0.7f * 0.35f));
}

TEST_CASE("season extract: the wind fields are independent of the season tint") {
    Rig plain;
    plain.world.spawn_with(SaveKey{6}, Weather{});
    plain.add_sway();
    const Rig::Frame a = plain.frame();
    Rig s;
    s.world.spawn_with(SaveKey{6}, Weather{});
    s.add_sway();
    s.add_season(0.625f);
    const Rig::Frame b = s.frame();
    CHECK(same_v4(a.foliage.wind_ext, b.foliage.wind_ext));
    CHECK(same_v4(a.terrain.wind_ext, b.terrain.wind_ext));
    CHECK(same_bits(a.foliage.amplitude, b.foliage.amplitude));
    CHECK(same_bits(a.terrain.frequency, b.terrain.frequency));
    CHECK_FALSE(same_v4(a.foliage.tint, b.foliage.tint));
}

TEST_CASE("season extract: the authored components are never written") {
    const auto build = [](Rig& r, bool with_season) {
        r.add_weather(WeatherKind::Snow, 0.5f);
        r.add_snow();
        r.add_clock(3, 5.0f);
        if (with_season) r.add_season(0.875f);
    };
    const auto hashes = [](Rig& r) {
        u32 h[5] = {};
        r.world.query<const Foliage3D&>().each([&](const Foliage3D& c) { h[0] = bytes_hash(c); });
        r.world.query<const TerrainFoliage3D&>().each([&](const TerrainFoliage3D& c) { h[1] = bytes_hash(c); });
        r.world.query<const SnowCover&>().each([&](const SnowCover& c) { h[2] = bytes_hash(c); });
        r.world.query<const Weather&>().each([&](const Weather& c) { h[3] = bytes_hash(c); });
        r.world.query<const WorldClock&>().each([&](const WorldClock& c) { h[4] = bytes_hash(c); });
        u32 x = 2166136261u;
        for (const u32 v : h) x = (x ^ v) * 16777619u;
        return x;
    };
    Rig a, b;
    build(a, false);
    build(b, true);
    const u32 before = hashes(b);
    for (int i = 0; i < 3; ++i) {
        a.frame();
        b.frame();
    }
    CHECK(hashes(b) == before);
    CHECK(hashes(a) == hashes(b));
}

TEST_CASE("season extract: winter raises the local snow cover with clear weather, only with Weather and SnowCover") {
    Season s;  // defaults: floor 0.5, lee shift 0.25 at the centre of winter
    Rig r;
    r.add_weather();
    r.add_snow();
    r.add_season(0.875f, s);
    const SeasonSnow bias = season_snow_bias(r.season(), 0.875f);
    REQUIRE(bias.cover_floor == doctest::Approx(0.5f));
    const Rig::Frame f = r.frame();
    CHECK(f.snow.cover == doctest::Approx(bias.cover_floor));
    CHECK(f.snow.wind_bias == doctest::Approx(0.5f + bias.lee_shift));
    CHECK(f.snow.coverage == doctest::Approx(0.8f));
    CHECK(r.world.try_get<SnowCover>(r.snow_e)->cover == 0.0f);  // the simulated cover is not raised

    Rig base;
    base.add_weather();
    base.add_snow();
    CHECK(base.frame().snow.cover == 0.0f);

    Rig no_snow;  // a Weather but no SnowCover: nothing to whiten
    no_snow.add_weather();
    no_snow.add_season(0.875f);
    CHECK(no_snow.frame().snow.cover == 0.0f);

    Rig no_weather;  // a SnowCover but no Weather
    no_weather.add_snow();
    no_weather.add_season(0.875f);
    CHECK(no_weather.frame().snow.cover == 0.0f);

    Rig strong;  // the floor scales with SnowCover.strength; a higher simulated-style cover is kept (max)
    SnowCover sc;
    sc.strength = 0.5f;
    strong.add_weather();
    strong.add_snow(sc);
    strong.add_season(0.875f);
    CHECK(strong.frame().snow.cover == doctest::Approx(0.25f));

    Rig clamp;  // the lee bias clamps at 1
    SnowCover hi;
    hi.wind_bias = 0.9f;
    clamp.add_weather();
    clamp.add_snow(hi);
    clamp.add_season(0.875f);
    CHECK(clamp.frame().snow.wind_bias == 1.0f);
}

TEST_CASE("season extract: r.snow_cover 0 still wins") {
    Rig r;
    r.add_weather();
    r.add_snow();
    r.add_season(0.875f);
    CHECK(r.frame().snow.cover > 0.0f);
    ConVar* cv = ConVar::find(StringId("r.snow_cover"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    const f32 off = r.frame().snow.cover;
    cv->reset();
    CHECK(off == 0.0f);
}

TEST_CASE("season extract: a day advance changes the tint without a re-scatter; no clock is the fixed start phase") {
    Rig r;
    r.add_clock(0);
    r.add_season(0.0f);  // 12-day year: day 0 is the start of spring, day 6 the start of autumn
    const Rig::Frame d0 = r.frame();
    r.set_day(6);
    const Rig::Frame d6 = r.frame();
    CHECK_FALSE(same_v4(d0.foliage.tint, d6.foliage.tint));
    CHECK_FALSE(same_v4(d0.terrain.tint, d6.terrain.tint));
    CHECK(d0.foliage.version == d6.foliage.version);  // no re-scatter
    CHECK(d0.terrain.version == d6.terrain.version);
    CHECK(d0.foliage.count == d6.foliage.count);
    const math::vec3 m = season_foliage_tint(r.season(), season_phase(r.season(), 6, 0.0f));
    CHECK(same_v4(d6.foliage.tint, times(kAuthoredTint, m)));

    Rig none;  // no WorldClock: day 0, hour 0
    none.add_season(0.625f);
    Rig clock0;
    clock0.add_clock(0, 0.0f);
    clock0.add_season(0.625f);
    CHECK(same_v4(none.frame().foliage.tint, clock0.frame().foliage.tint));
}

TEST_CASE("season extract: density thinning is dropped, so winter keeps every cached copy") {
    Rig base;
    const Rig::Frame a = base.frame();
    Rig winter;
    winter.add_season(0.875f);
    winter.season().winter_density = 0.1f;
    REQUIRE(season_foliage_density(winter.season(), 0.875f) < 0.5f);
    const Rig::Frame b = winter.frame();
    CHECK(b.foliage.count == a.foliage.count);
    CHECK(b.terrain.count == a.terrain.count);
    CHECK(b.foliage.version == a.foliage.version);
    CHECK(b.terrain.version == a.terrain.version);
}
