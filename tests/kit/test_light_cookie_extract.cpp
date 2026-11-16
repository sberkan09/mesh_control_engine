// Extract feed of the light cookies (M-LIGHTCOOKIE 40g): Kit3DExtract copies a LightCookie3D into the PointLight::cookie* of the
// SpotLight3D on the same entity and the first SunCookie3D into DirectionalLight::cookie, through render::apply_quality (tier,
// r.light_cookies, Range clamps). No component, enabled 0, a missing texture and a cookie on a PointLight3D leave the plain light
// (bit-identical to a scene without the feature); roll_speed_deg and scroll advance from the render clock and nothing is written
// back to the world or its state hash.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/asset/asset_system.h"
#include "mce/convar.h"
#include "mce/cook/cook.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/light_cookie.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kPi = 3.14159265359f;
constexpr const char* kWindowGuid = "1c00a000-0000-4000-8000-000000000001";
constexpr const char* kLeavesGuid = "1c00a000-0000-4000-8000-000000000002";

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

// Two cooked 2x2 textures the extract can resolve to a key without any GPU.
struct Content {
    asset::AssetSystem assets;
    Content() {
        const std::string root = cook_test::fresh_dir("light_cookie_extract");
        const std::vector<u8> px(2 * 2 * 4, 200);
        cook_test::write_file(root + "/assets/window.png", cook_test::make_png(2, 2, px));
        cook_test::write_file(root + "/assets/window.png.mceasset",
                              cook_test::sidecar(kWindowGuid, "texture", R"({ "srgb": true, "mips": false })"));
        cook_test::write_file(root + "/assets/leaves.png", cook_test::make_png(2, 2, px));
        cook_test::write_file(root + "/assets/leaves.png.mceasset",
                              cook_test::sidecar(kLeavesGuid, "texture", R"({ "srgb": true, "mips": false })"));
        cook::CookService svc;
        REQUIRE(svc.cook(StringView(root.c_str()), cook::CookOptions{}).ok());
        const std::string cooked = root + "/cooked";
        const StringView roots[] = {StringView(cooked.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 1)));
    }
    static asset::Guid window() { return asset::parse_guid(kWindowGuid).value(); }
    static asset::Guid leaves() { return asset::parse_guid(kLeavesGuid).value(); }
};

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;

    explicit Rig(asset::AssetSystem* assets = nullptr) : scene(store, mem::heap()) {
        install_kit3d(world, assets);
        extract.init(store, assets);
        extract.set_frame_dt(0.25f);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    Entity spot(math::vec3 p = {0.0f, 1.0f, -5.0f}) {
        SpotLight3D l;
        return world.spawn_with(LocalTransform{p, math::quat::identity(), {1, 1, 1}}, l);
    }
    void frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
    }
    template <class T>
    T& mut(Entity e) {
        T* p = world.try_get_mut<T>(e);
        REQUIRE(p != nullptr);
        return *p;
    }
    // The first (and only) light of a frame.
    render::PointLight light() {
        frame();
        REQUIRE(scene.point_lights().size() == 1);
        return scene.point_lights()[0];
    }
    render::SunCookie sun() {
        frame();
        return scene.light().cookie;
    }
    // The clock the next (non-bake, first-view) extract renders with.
    f64 next_clock() const { return static_cast<f64>(extract.time() + 0.25f); }
};

ConVar& cookie_cvar() {
    ConVar* cv = ConVar::find(StringId("r.light_cookies"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCookieCvar {
    ~ResetCookieCvar() { cookie_cvar().reset(); }
};

bool is_plain(const render::PointLight& l) {
    const render::PointLight d;
    return l.cookie == d.cookie && same_bits(l.cookie_strength, d.cookie_strength) && same_bits(l.cookie_roll, d.cookie_roll) &&
           same_bits(l.cookie_edge, d.cookie_edge);
}

bool is_plain(const render::SunCookie& c) {
    const render::SunCookie d;
    return c.texture == d.texture && same_bits(c.strength, d.strength) && same_bits(c.size, d.size) &&
           same_bits(c.offset.x, d.offset.x) && same_bits(c.offset.y, d.offset.y) && same_bits(c.roll, d.roll);
}

}  // namespace

TEST_CASE("light cookie extract: no component keeps the plain spot light") {
    Rig r;
    r.spot();
    for (int i = 0; i < 3; ++i) CHECK(is_plain(r.light()));
    CHECK(is_plain(r.sun()));
}

TEST_CASE("light cookie extract: enabled 0 keeps the plain spot light") {
    Content content;
    Rig r(&content.assets);
    const Entity e = r.spot();
    LightCookie3D c;
    c.enabled = 0;
    c.texture.guid = Content::window();
    c.strength = 0.5f;
    r.world.add(e, c);
    CHECK(is_plain(r.light()));
}

TEST_CASE("light cookie extract: a LightCookie3D copies its fields to its spot light") {
    Content content;
    Rig r(&content.assets);
    const Entity e = r.spot();
    LightCookie3D c;
    c.texture.guid = Content::window();
    c.strength = 0.5f;
    c.edge = 0.25f;
    c.roll_deg = 90.0f;
    r.world.add(e, c);
    const render::PointLight l = r.light();
    CHECK(l.cookie != ~0u);
    CHECK(l.spot_outer > 0.0f);
    CHECK(same_bits(l.cookie_strength, 0.5f));
    CHECK(same_bits(l.cookie_edge, 0.25f));
    CHECK(l.cookie_roll == doctest::Approx(kPi * 0.5f).epsilon(1e-5));
    // The key is the texture's: a second light with the same texture shares it, another texture has another key.
    const Entity e2 = r.spot({1.0f, 1.0f, -5.0f});
    r.world.add(e2, c);
    LightCookie3D other = c;
    other.texture.guid = Content::leaves();
    const Entity e3 = r.spot({2.0f, 1.0f, -5.0f});
    r.world.add(e3, other);
    r.frame();
    REQUIRE(r.scene.point_lights().size() == 3);
    u32 keys[3];
    for (u32 i = 0; i < 3; ++i) keys[i] = r.scene.point_lights()[i].cookie;
    u32 distinct = 0;
    for (u32 i = 0; i < 3; ++i) {
        CHECK(keys[i] != ~0u);
        bool seen = false;
        for (u32 j = 0; j < i; ++j) seen = seen || keys[j] == keys[i];
        distinct += seen ? 0u : 1u;
    }
    CHECK(distinct == 2);
}

TEST_CASE("light cookie extract: a missing or empty texture gives the plain light and nothing throws") {
    Content content;
    Rig r(&content.assets);
    const Entity e = r.spot();
    LightCookie3D c;
    c.texture.guid = asset::parse_guid("1c00a000-0000-4000-8000-0000000000ff").value();  // not in the content
    c.strength = 0.5f;
    r.world.add(e, c);
    for (int i = 0; i < 3; ++i) CHECK(r.light().cookie == ~0u);
    r.mut<LightCookie3D>(e).texture.guid = asset::Guid{};
    CHECK(r.light().cookie == ~0u);
    // No asset system at all: also no cookie.
    Rig bare;
    bare.world.add(bare.spot(), c);
    CHECK(bare.light().cookie == ~0u);
}

TEST_CASE("light cookie extract: a cookie on a PointLight3D is ignored") {
    Content content;
    Rig r(&content.assets);
    PointLight3D p;
    const Entity e = r.world.spawn_with(LocalTransform{{0.0f, 1.0f, -5.0f}, math::quat::identity(), {1, 1, 1}}, p);
    LightCookie3D c;
    c.texture.guid = Content::window();
    r.world.add(e, c);
    const render::PointLight l = r.light();
    CHECK(l.spot_outer == 0.0f);
    CHECK(is_plain(l));
}

TEST_CASE("light cookie extract: the Low tier keeps cookies, a tier or r.light_cookies 0 turns them off") {
    ResetCookieCvar reset;
    Content content;
    Rig r(&content.assets);
    const Entity e = r.spot();
    LightCookie3D c;
    c.texture.guid = Content::window();
    r.world.add(e, c);
    for (render::QualityTier tier : {render::QualityTier::Low, render::QualityTier::Medium, render::QualityTier::High}) {
        INFO(render::quality_tier_name(tier));
        r.extract.set_quality(render::quality_settings(tier));
        CHECK(r.light().cookie != ~0u);
    }
    render::QualitySettings off = render::quality_settings(render::QualityTier::High);
    off.light_cookies = false;
    r.extract.set_quality(off);
    CHECK(r.light().cookie == ~0u);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.light().cookie != ~0u);
    cookie_cvar().set_string("0");
    CHECK(r.light().cookie == ~0u);
    cookie_cvar().set_string("1");
    CHECK(r.light().cookie != ~0u);
}

TEST_CASE("light cookie extract: out-of-range values arrive clamped") {
    Content content;
    Rig r(&content.assets);
    const Entity e = r.spot();
    LightCookie3D c;
    c.texture.guid = Content::window();
    c.strength = 5.0f;
    c.edge = -3.0f;
    c.roll_deg = 720.0f;
    r.world.add(e, c);
    render::PointLight l = r.light();
    CHECK(l.cookie_strength == 1.0f);
    CHECK(l.cookie_edge == 0.0f);
    CHECK(std::fabs(l.cookie_roll) < 1e-4f);
    LightCookie3D& live = r.mut<LightCookie3D>(e);
    live.strength = kNaN;
    live.edge = kNaN;
    live.roll_deg = kNaN;
    l = r.light();
    CHECK(l.cookie_strength == 1.0f);
    CHECK(l.cookie_edge == 0.0f);
    CHECK(l.cookie_roll == 0.0f);
    CHECK(l.cookie != ~0u);
}

TEST_CASE("light cookie extract: roll_speed_deg turns the gobo with the render clock and the stored fields stay") {
    Content content;
    Rig r(&content.assets);
    const Entity e = r.spot();
    LightCookie3D c;
    c.texture.guid = Content::window();
    c.roll_deg = 10.0f;
    c.roll_speed_deg = 30.0f;
    r.world.add(e, c);
    f32 previous = -100.0f;
    for (int i = 0; i < 4; ++i) {
        const f64 t = r.next_clock();
        const render::PointLight l = r.light();
        const f32 expect = static_cast<f32>(std::fmod(10.0 + 30.0 * t, 360.0) * (3.14159265358979323846 / 180.0));
        INFO("frame " << i);
        CHECK(l.cookie_roll == doctest::Approx(expect).epsilon(1e-5));
        CHECK(l.cookie_roll != previous);
        previous = l.cookie_roll;
        const LightCookie3D& stored = r.mut<LightCookie3D>(e);
        CHECK(same_bits(stored.roll_deg, 10.0f));
        CHECK(same_bits(stored.roll_speed_deg, 30.0f));
    }
}

TEST_CASE("light cookie extract: no SunCookie3D keeps the plain sun, disabled too") {
    Content content;
    Rig r(&content.assets);
    CHECK(is_plain(r.sun()));
    SunCookie3D s;
    s.enabled = 0;
    s.texture.guid = Content::leaves();
    s.strength = 0.25f;
    r.world.spawn_with(s);
    CHECK(is_plain(r.sun()));
}

TEST_CASE("light cookie extract: a SunCookie3D copies its fields to the sun") {
    Content content;
    Rig r(&content.assets);
    SunCookie3D s;
    s.texture.guid = Content::leaves();
    s.strength = 0.75f;
    s.size = 12.0f;
    s.offset = {3.0f, -2.0f};
    s.roll_deg = 45.0f;
    r.world.spawn_with(s);
    const render::SunCookie c = r.sun();
    CHECK(c.texture != ~0u);
    CHECK(same_bits(c.strength, 0.75f));
    CHECK(same_bits(c.size, 12.0f));
    CHECK(c.offset.x == doctest::Approx(3.0f));
    CHECK(c.offset.y == doctest::Approx(-2.0f));
    CHECK(c.roll == doctest::Approx(kPi * 0.25f).epsilon(1e-5));
}

TEST_CASE("light cookie extract: the first SunCookie3D wins, a disabled first does not fall through, removing it clears") {
    Content content;
    Rig r(&content.assets);
    SunCookie3D a;
    a.texture.guid = Content::leaves();
    a.size = 10.0f;
    const Entity first = r.world.spawn_with(a);
    SunCookie3D b = a;
    b.size = 30.0f;
    const Entity second = r.world.spawn_with(b);
    CHECK(same_bits(r.sun().size, 10.0f));
    r.world.destroy(first);
    CHECK(same_bits(r.sun().size, 30.0f));
    r.world.destroy(second);
    CHECK(is_plain(r.sun()));
    SunCookie3D off = a;
    off.enabled = 0;
    r.world.spawn_with(off);
    r.world.spawn_with(b);
    CHECK(is_plain(r.sun()));
}

TEST_CASE("light cookie extract: the sun cookie follows the tier, r.light_cookies and the clamps") {
    ResetCookieCvar reset;
    Content content;
    Rig r(&content.assets);
    SunCookie3D s;
    s.texture.guid = Content::leaves();
    s.strength = 4.0f;
    s.size = 5000.0f;
    const Entity sun = r.world.spawn_with(s);
    for (render::QualityTier tier : {render::QualityTier::Low, render::QualityTier::Medium, render::QualityTier::High}) {
        INFO(render::quality_tier_name(tier));
        r.extract.set_quality(render::quality_settings(tier));
        const render::SunCookie c = r.sun();
        CHECK(c.texture != ~0u);
        CHECK(c.strength == 1.0f);
        CHECK(c.size == 1000.0f);
    }
    render::QualitySettings off = render::quality_settings(render::QualityTier::High);
    off.light_cookies = false;
    r.extract.set_quality(off);
    CHECK(r.sun().texture == ~0u);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    cookie_cvar().set_string("0");
    CHECK(r.sun().texture == ~0u);
    cookie_cvar().set_string("1");
    CHECK(r.sun().texture != ~0u);
    r.mut<SunCookie3D>(sun).size = kNaN;
    CHECK(r.sun().size == 20.0f);
}

TEST_CASE("light cookie extract: scroll moves the sun pattern with the render clock, wrapped by the size") {
    Content content;
    Rig r(&content.assets);
    SunCookie3D s;
    s.texture.guid = Content::leaves();
    s.size = 20.0f;
    s.offset = {4.0f, 0.0f};
    s.scroll = {2.0f, -1.0f};
    const Entity e = r.world.spawn_with(s);
    for (int i = 0; i < 4; ++i) {
        const f64 t = r.next_clock();
        const render::SunCookie c = r.sun();
        INFO("frame " << i);
        CHECK(c.offset.x == doctest::Approx(static_cast<f32>(std::fmod(4.0 + 2.0 * t, 20.0))).epsilon(1e-5));
        CHECK(c.offset.y == doctest::Approx(static_cast<f32>(std::fmod(-1.0 * t, 20.0))).epsilon(1e-5));
        CHECK(same_bits(r.mut<SunCookie3D>(e).offset.x, 4.0f));  // the stored base value never moves
        CHECK(same_bits(r.mut<SunCookie3D>(e).offset.y, 0.0f));
    }
    // A fast scroll stays inside one period, so the float offset keeps its precision.
    r.mut<SunCookie3D>(e).scroll = {1000.0f, 0.0f};
    const render::SunCookie fast = r.sun();
    CHECK(std::fabs(fast.offset.x) < 20.0f);
    CHECK(std::fabs(fast.offset.y) < 20.0f);
}

TEST_CASE("light cookie extract: nothing is written to the world or its state hash") {
    Content content;
    Rig r(&content.assets);
    const Entity lamp = r.spot();
    LightCookie3D c;
    c.texture.guid = Content::window();
    c.roll_deg = 20.0f;
    c.roll_speed_deg = 90.0f;
    r.world.add(lamp, c);
    SunCookie3D s;
    s.texture.guid = Content::leaves();
    s.scroll = {3.0f, 4.0f};
    s.offset = {1.0f, 2.0f};
    const Entity sun = r.world.spawn_with(s);
    const u64 before = r.world.state_hash();
    for (int i = 0; i < 20; ++i) r.frame();
    CHECK(r.world.state_hash() == before);
    const LightCookie3D& lc = r.mut<LightCookie3D>(lamp);
    CHECK(same_bits(lc.roll_deg, 20.0f));
    CHECK(same_bits(lc.roll_speed_deg, 90.0f));
    const SunCookie3D& sc = r.mut<SunCookie3D>(sun);
    CHECK(same_bits(sc.offset.x, 1.0f));
    CHECK(same_bits(sc.offset.y, 2.0f));
    CHECK(same_bits(sc.scroll.x, 3.0f));
}
