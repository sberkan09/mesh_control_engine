// Halo (M-HALO 54c): a plain saved component, no system, and the pure helper halo_level. Kit3DExtract fills render::Halo
// from the first Halo and the cirrus cover of the frame (so a scene without a Cirrus, a Weather, or with an equirect sky
// keeps the default carrier, bit for bit), then apply_quality. Weather, Cirrus and CloudLayer are never written.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>

#include "mce/containers/array.h"
#include "mce/convar.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cirrus.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/halo.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_halo(const render::Halo& a, const render::Halo& b) {  // the render struct has no implicit padding
    return same_bits(a.strength, b.strength) && same_bits(a.sun_dogs, b.sun_dogs) && same_bits(a.moon, b.moon) &&
           same_bits(a.dispersion, b.dispersion) && a.enabled == b.enabled;
}
bool same_authoring(const Halo& a, const Halo& b) {
    return same_bits(a.strength, b.strength) && same_bits(a.sun_dogs, b.sun_dogs) && same_bits(a.moon, b.moon) &&
           same_bits(a.dispersion, b.dispersion);
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    u64 t = 0;
    Entity halo_e, cirrus_e, weather_e;
    explicit Rig(bool with_weather = true, bool with_cirrus = true, bool with_halo = true) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (with_weather) {
            Weather w;
            w.current = w.target = WeatherKind::Clear;
            w.intensity = 0.0f;
            weather_e = world.spawn_with(SaveKey{1}, w);
        }
        if (with_cirrus) cirrus_e = world.spawn_with(SaveKey{2}, Cirrus{});
        if (with_halo) halo_e = world.spawn_with(SaveKey{3}, Halo{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    Halo& halo() { return *world.try_get_mut<Halo>(halo_e); }
    Cirrus& cirrus() { return *world.try_get_mut<Cirrus>(cirrus_e); }
    Weather& weather() { return *world.try_get_mut<Weather>(weather_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    void carriers(render::Halo& h, render::Cirrus& c) {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        h = s.halo();
        c = s.cirrus();
    }
    render::Halo carrier() {
        render::Halo h;
        render::Cirrus c;
        carriers(h, c);
        return h;
    }
};

}  // namespace

TEST_CASE("halo: defaults") {
    const Halo h;
    CHECK(h.strength == 1.0f);
    CHECK(h.sun_dogs == 1.0f);
    CHECK(h.moon == 0.5f);
    CHECK(h.dispersion == 1.0f);
    CHECK(sizeof(Halo) == 4 * sizeof(f32));  // no padding to hash
}

TEST_CASE("halo_level: 0 under thin cirrus, rises with the cover, full from 0.5") {
    const Halo h;
    CHECK(halo_level(h, 0.0f) == 0.0f);
    CHECK(halo_level(h, kHaloCoverLo) == 0.0f);
    CHECK(halo_level(h, 0.05f) == 0.0f);
    f32 prev = 0.0f;
    for (f32 c = 0.11f; c <= 1.0f; c += 0.01f) {
        const f32 v = halo_level(h, c);
        CHECK(v >= prev);
        CHECK(v <= 1.0f);
        prev = v;
    }
    CHECK(halo_level(h, 0.3f) == doctest::Approx(0.5f));
    CHECK(halo_level(h, kHaloCoverHi) == doctest::Approx(1.0f));
    CHECK(halo_level(h, 1.0f) == 1.0f);
    CHECK(halo_level(h, 7.0f) == 1.0f);
}

TEST_CASE("halo_level: strength scales it, 0 is exactly 0, NaN and out-of-range fall back") {
    Halo h;
    h.strength = 0.4f;
    CHECK(halo_level(h, 1.0f) == doctest::Approx(0.4f));
    h.strength = 0.0f;
    for (const f32 c : {0.0f, 0.3f, 1.0f}) CHECK(same_bits(halo_level(h, c), 0.0f));
    h.strength = kNaN;  // the default, 1
    CHECK(halo_level(h, 1.0f) == 1.0f);
    h.strength = kInf;
    CHECK(halo_level(h, 1.0f) == 1.0f);
    h.strength = 5.0f;  // clamps to 1
    CHECK(halo_level(h, 1.0f) == 1.0f);
    h.strength = -2.0f;  // clamps to 0
    CHECK(halo_level(h, 1.0f) == 0.0f);
    h.strength = 1.0f;
    CHECK(halo_level(h, kNaN) == 0.0f);
    CHECK(halo_level(h, -1.0f) == 0.0f);
    CHECK(halo_level(h, -kInf) == 0.0f);
    CHECK(halo_level(h, kInf) == 1.0f);
}

TEST_CASE("halo extract: the carrier is the helper over the frame's cirrus cover and the authoring") {
    Rig r;
    r.halo().sun_dogs = 0.6f;
    r.halo().moon = 0.25f;
    r.halo().dispersion = 0.3f;
    r.weather().target = WeatherKind::Rain;  // a front: the cirrus thickens
    render::Halo h;
    render::Cirrus c;
    r.carriers(h, c);
    REQUIRE(c.coverage > kHaloCoverHi);
    CHECK(h.enabled);
    CHECK(h.strength == halo_level(r.halo(), c.coverage));
    CHECK(h.strength > 0.0f);
    CHECK(h.sun_dogs == 0.6f);
    CHECK(h.moon == 0.25f);
    CHECK(h.dispersion == 0.3f);
    CHECK(same_halo(h, r.carrier()));  // deterministic

    // thin cirrus: no ring
    r.cirrus().strength = 0.05f;
    CHECK(r.carrier().strength == 0.0f);
    r.cirrus().strength = 0.0f;  // no cover at all
    CHECK(r.carrier().strength == 0.0f);
}

TEST_CASE("halo extract: strength 0 is 0, a second Halo is ignored") {
    Rig r;
    r.weather().target = WeatherKind::Rain;
    CHECK(r.carrier().strength > 0.0f);
    Halo other;
    other.strength = 0.1f;
    r.world.spawn_with(SaveKey{9}, other);
    const render::Halo a = r.carrier();
    CHECK(a.strength > 0.5f);  // not the second one's 0.1
    r.halo().strength = 0.0f;
    CHECK(same_bits(r.carrier().strength, 0.0f));
}

TEST_CASE("halo extract: without a Halo, a Cirrus, a Weather or with an equirect sky the carrier is the default") {
    const render::Halo def;
    {
        Rig r(true, true, false);
        CHECK(same_halo(r.carrier(), def));
    }
    {
        Rig r(true, false, true);
        CHECK(same_halo(r.carrier(), def));
    }
    {
        Rig r(false, true, true);
        CHECK(same_halo(r.carrier(), def));
    }
    {
        Rig r;
        r.weather().target = WeatherKind::Rain;
        REQUIRE(r.carrier().strength > 0.0f);
        r.env().sky_texture.guid = asset::Guid{1, 2};  // the texture wins over every sky layer
        CHECK(same_halo(r.carrier(), def));
        r.env().sky_texture.guid = asset::Guid{};
        CHECK(r.carrier().strength > 0.0f);
    }
}

TEST_CASE("halo extract: a stale carrier never survives") {
    Rig r;
    r.weather().target = WeatherKind::Rain;
    REQUIRE(r.carrier().strength > 0.0f);
    r.world.destroy(r.halo_e);
    CHECK(same_halo(r.carrier(), render::Halo{}));
}

TEST_CASE("halo extract: the Low tier and r.halo 0 zero the strength, the clamps apply") {
    Rig r;
    r.weather().target = WeatherKind::Rain;
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.carrier().strength == 0.0f);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.carrier().strength > 0.0f);

    ConVar* cv = ConVar::find(StringId("r.halo"));
    REQUIRE(cv != nullptr);
    cv->set_float(0.0f);
    render::Halo h;
    render::Cirrus c;
    r.carriers(h, c);
    cv->reset();
    CHECK(h.strength == 0.0f);
    CHECK(c.coverage > 0.0f);  // the cirrus is left alone
    CHECK(r.carrier().strength > 0.0f);

    r.halo().sun_dogs = kNaN;  // authoring the renderer repairs (apply_quality)
    r.halo().moon = 9.0f;
    r.halo().dispersion = -1.0f;
    const render::Halo s = r.carrier();
    CHECK(s.sun_dogs == 1.0f);
    CHECK(s.moon == 1.0f);
    CHECK(s.dispersion == 0.0f);
}

TEST_CASE("halo extract: Weather, Cirrus, CloudLayer and Halo are never written") {
    Rig r;
    r.world.spawn_with(SaveKey{6}, CloudLayer{});
    r.weather().target = WeatherKind::Rain;
    const u64 before = r.world.state_hash();  // hashes every saved column
    const Halo h0 = r.halo();
    for (int i = 0; i < 3; ++i) CHECK(r.carrier().strength > 0.0f);
    CHECK(r.world.state_hash() == before);
    CHECK(same_authoring(r.halo(), h0));
}

TEST_CASE("halo: save and load round-trips the fields and the state hash") {
    Array<byte> bytes;
    u64 hash_a = 0;
    u64 hash_s = 0;
    Halo l;
    l.strength = 0.7f;
    l.sun_dogs = 0.2f;
    l.moon = 0.8f;
    l.dispersion = 0.45f;
    {
        Rig a;
        a.halo() = l;
        a.run(100);
        save::write_save(a.world, {}, bytes);
        hash_s = a.world.state_hash();
        a.run(50);
        hash_a = a.world.state_hash();
    }
    Rig b;
    b.run(77);
    REQUIRE(save::read_save(b.world, bytes.span()));
    CHECK(b.world.state_hash() == hash_s);
    b.t = 100;
    b.run(50);
    CHECK(b.world.state_hash() == hash_a);
    CHECK(same_authoring(b.halo(), l));
}

TEST_CASE("halo: a scene without the component hashes as before") {
    Rig a(true, true, false);
    Rig b(true, true, false);
    a.run(30);
    b.run(30);
    CHECK(a.world.state_hash() == b.world.state_hash());
}
