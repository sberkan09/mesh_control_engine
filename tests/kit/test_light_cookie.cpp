// LightCookie3D and SunCookie3D (M-LIGHTCOOKIE 40g): the component defaults match the render side, the explicit pad is zero, and
// the fields are reached by name through the reflection path (what the editor inspector, the Luau component access and the
// MCP component tools use).

#include <doctest/doctest.h>

#include <bit>

#include "mce/kit/light_cookie.h"
#include "mce/refl/property_path.h"
#include "mce/render/clusters.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("light cookie: LightCookie3D defaults match the render light") {
    const LightCookie3D c;
    const render::PointLight d;
    CHECK(c.enabled == 1u);
    CHECK(c.texture.guid == asset::Guid{});
    CHECK(same_bits(c.strength, d.cookie_strength));
    CHECK(same_bits(c.edge, d.cookie_edge));
    CHECK(same_bits(c.roll_deg, d.cookie_roll));
    CHECK(same_bits(c.roll_speed_deg, 0.0f));
}

TEST_CASE("light cookie: SunCookie3D defaults match the render sun cookie") {
    const SunCookie3D c;
    const render::SunCookie d;
    CHECK(c.enabled == 1u);
    CHECK(c.texture.guid == asset::Guid{});
    CHECK(same_bits(c.strength, d.strength));
    CHECK(same_bits(c.size, d.size));
    CHECK(same_bits(c.offset.x, d.offset.x));
    CHECK(same_bits(c.offset.y, d.offset.y));
    CHECK(same_bits(c.roll_deg, d.roll));
    CHECK(same_bits(c.scroll.x, 0.0f));
    CHECK(same_bits(c.scroll.y, 0.0f));
}

TEST_CASE("light cookie: the explicit pads are zero") {
    const LightCookie3D a;
    const SunCookie3D b;
    for (u8 p : a.pad_) CHECK(p == 0u);
    for (u8 p : b.pad_) CHECK(p == 0u);
}

TEST_CASE("light cookie: the components are reflected with every authored field") {
    const refl::TypeRecord* l = refl::find_type(StringId("LightCookie3D"));
    const refl::TypeRecord* s = refl::find_type(StringId("SunCookie3D"));
    REQUIRE(l != nullptr);
    REQUIRE(s != nullptr);
    for (const char* f : {"enabled", "texture", "strength", "roll_deg", "roll_speed_deg", "edge"}) {
        INFO(f);
        CHECK(l->find_field(StringId(f)) != nullptr);
    }
    for (const char* f : {"enabled", "texture", "strength", "size", "offset", "scroll", "roll_deg"}) {
        INFO(f);
        CHECK(s->find_field(StringId(f)) != nullptr);
    }
}

TEST_CASE("light cookie: a field is set and read back by name through the reflection path") {
    LightCookie3D l;
    const refl::TypeRecord& lt = refl::type_of<LightCookie3D>();
    Result<refl::PropertyRef> strength = refl::resolve_path(lt, "strength");
    REQUIRE(strength.has_value());
    CHECK(refl::get_property<f32>(&l, strength.value()) == 1.0f);
    refl::set_property<f32>(&l, strength.value(), 0.25f);
    CHECK(l.strength == 0.25f);
    Result<refl::PropertyRef> speed = refl::resolve_path(lt, "roll_speed_deg");
    REQUIRE(speed.has_value());
    refl::set_property<f32>(&l, speed.value(), 45.0f);
    CHECK(l.roll_speed_deg == 45.0f);

    SunCookie3D s;
    const refl::TypeRecord& st = refl::type_of<SunCookie3D>();
    Result<refl::PropertyRef> size = refl::resolve_path(st, "size");
    REQUIRE(size.has_value());
    refl::set_property<f32>(&s, size.value(), 8.0f);
    CHECK(s.size == 8.0f);
    Result<refl::PropertyRef> scroll_y = refl::resolve_path(st, "scroll.y");
    REQUIRE(scroll_y.has_value());
    refl::set_property<f32>(&s, scroll_y.value(), 1.5f);
    CHECK(s.scroll.y == 1.5f);
    CHECK(refl::get_property<f32>(&s, scroll_y.value()) == 1.5f);
    // The pads are not reflected: a path to one does not resolve.
    CHECK_FALSE(refl::resolve_path(st, "pad_").has_value());
}
