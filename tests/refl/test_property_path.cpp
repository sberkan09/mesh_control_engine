#include <doctest/doctest.h>

#include <cstddef>

#include "test_support.h"
#include "test_types.h"

using namespace mce;
using namespace mce::refl;
using namespace refl_test;

namespace {

PropertyRef resolve_ok(const TypeRecord& t, StringView path) {
    Result<PropertyRef> r = resolve_path(t, path);
    REQUIRE_MESSAGE(r.has_value(), path);
    return r.value();
}

Errc resolve_err(const TypeRecord& t, StringView path) {
    Result<PropertyRef> r = resolve_path(t, path);
    REQUIRE_MESSAGE(!r.has_value(), path);
    CHECK(r.error().path == path);
    CHECK(StringView(r.error().stage) == "refl.path");
    return r.error().code;
}

}  // namespace

TEST_CASE("top-level paths resolve to the field") {
    const PropertyRef hp = resolve_ok(type_of<Health>(), "hp");
    CHECK(hp.valid());
    CHECK(hp.kind == FieldKind::F32);
    CHECK(hp.offset == offsetof(Health, hp));
    CHECK(hp.field == type_of<Health>().find_field(StringId("hp")));

    Health h;
    CHECK(get_property<f32>(&h, hp) == 100.0f);
    set_property<f32>(&h, hp, 42.0f);
    CHECK(h.hp == 42.0f);
}

TEST_CASE("nested struct, fixed array and lane paths") {
    const TypeRecord& t = type_of<Patrol>();

    const PropertyRef home_pos = resolve_ok(t, "home.pos");
    CHECK(home_pos.kind == FieldKind::Vec3);
    CHECK(home_pos.offset == offsetof(Patrol, home) + offsetof(Waypoint, pos));

    const PropertyRef home_y = resolve_ok(t, "home.pos.y");
    CHECK(home_y.kind == FieldKind::F32);
    CHECK(home_y.size == sizeof(f32));
    CHECK(home_y.offset == offsetof(Patrol, home) + offsetof(Waypoint, pos) + offsetof(math::vec3, y));
    CHECK(home_y.field->name == StringId("pos"));  // innermost declared field

    const PropertyRef whole = resolve_ok(t, "waypoints");
    CHECK(whole.count == 4);
    CHECK(whole.kind == FieldKind::Struct);

    const PropertyRef wp2 = resolve_ok(t, "waypoints[2]");
    CHECK(wp2.count == 1);
    CHECK(wp2.offset == offsetof(Patrol, waypoints) + 2 * sizeof(Waypoint));

    const PropertyRef wait = resolve_ok(t, "waypoints[2].wait");
    CHECK(wait.field->has(FieldFlags::EditorRange));
    CHECK(wait.field->editor_max == 10.0f);

    const PropertyRef z = resolve_ok(t, "waypoints[3].pos.z");
    const PropertyRef alpha = resolve_ok(t, "tint.a");
    const PropertyRef qw = resolve_ok(t, "facing.w");
    const PropertyRef lane = resolve_ok(t, "lanes[1]");
    CHECK(lane.kind == FieldKind::U16);

    Patrol p;
    set_property<f32>(&p, z, 9.0f);
    set_property<f32>(&p, alpha, 0.25f);
    set_property<math::vec3>(&p, home_pos, math::vec3(1, 2, 3));
    set_property<Waypoint>(&p, wp2, Waypoint{math::vec3(4, 5, 6), 2.0f});
    CHECK(p.waypoints[3].pos.z == 9.0f);
    CHECK(p.tint.a == 0.25f);
    CHECK(p.home.pos.y == 2.0f);
    CHECK(p.waypoints[2].pos.x == 4.0f);
    CHECK(get_property<f32>(&p, wait) == 2.0f);
    CHECK(get_property<f32>(&p, qw) == 1.0f);
    CHECK(get_property<u16>(&p, lane) == 2);
}

TEST_CASE("enum, entity-ref and string leaves") {
    const TypeRecord& t = type_of<Patrol>();
    Patrol p;

    const PropertyRef state = resolve_ok(t, "state");
    CHECK(state.type_id == StringId("EnemyState"));
    set_property<EnemyState>(&p, state, EnemyState::Chase);
    CHECK(p.state == EnemyState::Chase);

    const PropertyRef target = resolve_ok(t, "target");
    CHECK(target.kind == FieldKind::EntityRef);
    set_property<FakeEntity>(&p, target, FakeEntity{7, 2});
    CHECK(p.target.index == 7);

    const PropertyRef label = resolve_ok(t, "label");
    CHECK(get_string(&p, label) == "guard");
    set_string(&p, label, "sentinel");
    CHECK(p.label == StringView("sentinel"));
    set_string(&p, label, "a-label-much-longer-than-fifteen");
    CHECK(p.label.size() == 15);
}

TEST_CASE("a resolved ref is reusable across instances") {
    const PropertyRef y = resolve_ok(type_of<Patrol>(), "waypoints[1].pos.y");
    Patrol a, b;
    set_property<f32>(&a, y, 1.0f);
    set_property<f32>(&b, y, 2.0f);
    CHECK(a.waypoints[1].pos.y == 1.0f);
    CHECK(b.waypoints[1].pos.y == 2.0f);
}

TEST_CASE("malformed and unknown paths fail with a typed error") {
    const TypeRecord& t = type_of<Patrol>();
    CHECK(resolve_err(t, "nope") == Errc::NotFound);
    CHECK(resolve_err(t, "home.nope") == Errc::NotFound);
    CHECK(resolve_err(t, "") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "home.") == Errc::InvalidArgument);
    CHECK(resolve_err(t, ".home") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "home..pos") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "home pos") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "waypoints[4]") == Errc::InvalidArgument);     // out of range
    CHECK(resolve_err(t, "waypoints[]") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "waypoints[1") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "waypoints[x]") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "waypoints.pos") == Errc::InvalidArgument);    // array needs an index
    CHECK(resolve_err(t, "home[0]") == Errc::InvalidArgument);          // not an array
    CHECK(resolve_err(t, "home.pos.w") == Errc::InvalidArgument);       // vec3 has no w
    CHECK(resolve_err(t, "tint.x") == Errc::InvalidArgument);           // Color lanes are rgba
    CHECK(resolve_err(t, "seed.x") == Errc::InvalidArgument);           // scalar has no lanes
    CHECK(resolve_err(t, "home.pos.x.y") == Errc::InvalidArgument);
    CHECK(resolve_err(t, "waypoints[99999999999999999999]") == Errc::InvalidArgument);
}

TEST_CASE("nested struct whose type is not registered reports NotFound") {
    static constexpr FieldRecord kFields[1] = {[] {
        FieldRecord f;
        f.name = StringId("inner");
        f.name_str = "inner";
        f.kind = FieldKind::Struct;
        f.size = 4;
        f.type_id = StringId("NeverRegistered");
        return f;
    }()};
    TypeRecord t;
    t.id = StringId("Outer_Unregistered");
    t.name = "Outer_Unregistered";
    t.size = 4;
    t.align = 4;
    t.fields = Span<const FieldRecord>(kFields, 1);
    CHECK(resolve_ok(t, "inner").kind == FieldKind::Struct);
    CHECK(resolve_err(t, "inner.x") == Errc::NotFound);
}

#if MCE_ASSERTS_ENABLED
TEST_CASE("typed access with the wrong C++ type asserts") {
    const PropertyRef hp = resolve_ok(type_of<Health>(), "hp");
    const PropertyRef state = resolve_ok(type_of<Patrol>(), "state");
    const PropertyRef wps = resolve_ok(type_of<Patrol>(), "waypoints");
    Health h;
    Patrol p;
    {
        test::ExpectAssert expect;
        (void)property_ptr<i32>(&h, hp);  // same size, wrong kind
    }
    {
        test::ExpectAssert expect;
        (void)property_ptr<Unreflected>(&p, state);  // different enum type
    }
    {
        test::ExpectAssert expect;
        (void)property_ptr<Waypoint>(&p, wps);  // whole array, no index
    }
    {
        test::ExpectAssert expect;
        (void)get_string(&h, hp);
    }
}
#endif
