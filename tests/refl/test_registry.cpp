#include <doctest/doctest.h>

#include <cstddef>
#include <cstring>

#include "test_support.h"
#include "test_types.h"

using namespace mce;
using namespace mce::refl;
using namespace refl_test;

namespace refl_test {
const TypeRecord* health_record_from_second_tu();
}

namespace {

const FieldRecord& field(const TypeRecord& t, const char* name) {
    const FieldRecord* f = t.find_field(StringId(name));
    REQUIRE(f != nullptr);
    return *f;
}

}  // namespace

TEST_CASE("MCE_COMPONENT builds the expected field table") {
    const TypeRecord& t = type_of<Health>();
    CHECK(t.id == StringId("Health"));
    CHECK(std::strcmp(t.name, "Health") == 0);
    CHECK(t.size == sizeof(Health));
    CHECK(t.align == alignof(Health));
    CHECK(t.is_component());
    CHECK(t.base == nullptr);
    REQUIRE(t.fields.size() == 3);

    // Declaration order is preserved.
    CHECK(std::strcmp(t.fields[0].name_str, "hp") == 0);
    CHECK(std::strcmp(t.fields[1].name_str, "max") == 0);
    CHECK(std::strcmp(t.fields[2].name_str, "armor") == 0);

    const FieldRecord& hp = field(t, "hp");
    CHECK(hp.kind == FieldKind::F32);
    CHECK(hp.offset == offsetof(Health, hp));
    CHECK(hp.size == sizeof(f32));
    CHECK(hp.count == 1);
    CHECK(hp.has(FieldFlags::Save));
    CHECK(hp.has(FieldFlags::EditorRange));
    CHECK_FALSE(hp.has(FieldFlags::ReadOnly));
    CHECK(hp.editor_min == 0.0f);
    CHECK(hp.editor_max == 1000.0f);
    CHECK(std::strcmp(hp.help, "current hit points") == 0);

    const FieldRecord& max = field(t, "max");
    CHECK(max.offset == offsetof(Health, max));
    CHECK(max.flags == FieldFlags::None);
    CHECK(std::strcmp(max.help, "") == 0);

    const FieldRecord& armor = field(t, "armor");
    CHECK(armor.kind == FieldKind::U8);
    CHECK(armor.offset == offsetof(Health, armor));
    CHECK(armor.flags == (FieldFlags::ReadOnly | FieldFlags::Transient));

    CHECK(t.find_field(StringId("nope")) == nullptr);
}

TEST_CASE("every supported field kind is deduced from the member type") {
    const TypeRecord& t = type_of<Patrol>();
    REQUIRE(t.fields.size() == 12);

    struct Expect {
        const char* name;
        FieldKind kind;
        usize offset;
        usize size;
        u32 count;
    };
    const Expect expected[] = {
        {"state", FieldKind::Enum, offsetof(Patrol, state), sizeof(EnemyState), 1},
        {"mode", FieldKind::Enum, offsetof(Patrol, mode), sizeof(Unreflected), 1},
        {"waypoints", FieldKind::Struct, offsetof(Patrol, waypoints), sizeof(Waypoint), 4},
        {"home", FieldKind::Struct, offsetof(Patrol, home), sizeof(Waypoint), 1},
        {"tint", FieldKind::Color, offsetof(Patrol, tint), sizeof(math::Color), 1},
        {"facing", FieldKind::Quat, offsetof(Patrol, facing), sizeof(math::quat), 1},
        {"label", FieldKind::String, offsetof(Patrol, label), sizeof(FixedString<16>), 1},
        {"target", FieldKind::EntityRef, offsetof(Patrol, target), sizeof(FakeEntity), 1},
        {"seed", FieldKind::I64, offsetof(Patrol, seed), sizeof(i64), 1},
        {"active", FieldKind::Bool, offsetof(Patrol, active), sizeof(bool), 1},
        {"timer", FieldKind::F64, offsetof(Patrol, timer), sizeof(f64), 1},
        {"lanes", FieldKind::U16, offsetof(Patrol, lanes), sizeof(u16), 3},
    };
    for (const Expect& e : expected) {
        CAPTURE(e.name);
        const FieldRecord& f = field(t, e.name);
        CHECK(f.kind == e.kind);
        CHECK(f.offset == e.offset);
        CHECK(f.size == e.size);
        CHECK(f.count == e.count);
        CHECK(f.is_array() == (e.count > 1));
    }

    CHECK(field(t, "state").type_id == StringId("EnemyState"));
    CHECK_FALSE(field(t, "mode").type_id.valid());  // un-reflected enum: integer only
    CHECK(field(t, "waypoints").type_id == StringId("Waypoint"));
    CHECK(field(t, "target").type_id == StringId("FakeEntity"));
    CHECK_FALSE(field(t, "seed").type_id.valid());
    REQUIRE(field(t, "label").string_ops != nullptr);
    CHECK(field(t, "label").string_ops->capacity == 15);
}

TEST_CASE("registry lookup, ordering and flags") {
    CHECK(find_type(StringId("Health")) == &type_of<Health>());
    CHECK(find_type(StringId("Patrol")) == &type_of<Patrol>());
    CHECK(find_type(StringId("Missing")) == nullptr);

    const TypeRecord* wp = find_type(StringId("Waypoint"));
    REQUIRE(wp != nullptr);
    CHECK_FALSE(wp->is_component());  // MCE_REFL_STRUCT

    // Sorted by name, independent of static-init order.
    Span<const TypeRecord* const> types = all();
    REQUIRE(types.size() >= 5);  // Health OnlyHere Patrol Tag Waypoint
    for (usize i = 1; i < types.size(); ++i) {
        CHECK(std::strcmp(types[i - 1]->name, types[i]->name) < 0);
    }
}

TEST_CASE("a type declared in a header and included by two TUs registers once") {
    CHECK(health_record_from_second_tu() == &type_of<Health>());
    u32 seen = 0;
    for (const TypeRecord* t : all()) seen += t->id == StringId("Health") ? 1u : 0u;
    CHECK(seen == 1);
}

TEST_CASE("a type no code references still registers before main") {
    const TypeRecord* t = find_type(StringId("OnlyHere"));
    REQUIRE(t != nullptr);
    CHECK(t->fields.size() == 2);
}

TEST_CASE("zero-field components are valid") {
    const TypeRecord& t = type_of<Tag>();
    CHECK(t.fields.empty());
    CHECK(t.is_component());
    CHECK(find_type(StringId("Tag")) == &t);
}

TEST_CASE("construct_default produces the declared default values") {
    alignas(Patrol) unsigned char storage[sizeof(Patrol)];
    std::memset(storage, 0xCD, sizeof(storage));
    const TypeRecord& t = type_of<Patrol>();
    REQUIRE(t.construct_default != nullptr);
    t.construct_default(storage);
    Patrol p;
    std::memcpy(&p, storage, sizeof(p));
    CHECK(p.state == EnemyState::Patrol);
    CHECK(p.seed == -7);
    CHECK(p.waypoints[3].wait == 0.5f);
    CHECK(p.label == StringView("guard"));
    CHECK(p.lanes[2] == 3);
}

TEST_CASE("enums reflect names and values") {
    const EnumRecord& e = enum_of<EnemyState>();
    CHECK(find_enum(StringId("EnemyState")) == &e);
    CHECK(e.size == 1);
    CHECK_FALSE(e.is_signed);
    REQUIRE(e.values.size() == 4);
    CHECK(std::strcmp(e.values[2].name_str, "Chase") == 0);
    CHECK(e.values[2].value == 5);
    REQUIRE(e.find(StringId("Attack")) != nullptr);
    CHECK(e.find(StringId("Attack"))->value == 6);
    REQUIRE(e.find(i64{5}) != nullptr);
    CHECK(e.find(i64{5})->name == StringId("Chase"));
    CHECK(e.find(i64{2}) == nullptr);
    CHECK(e.find(StringId("Flee")) == nullptr);
    CHECK(all_enums().size() >= 1);
}

TEST_CASE("duplicate registrations: equivalent records are ignored, collisions assert") {
    const TypeRecord& health = type_of<Health>();

    // Same record again (or an identical copy from another module): first stays authoritative.
    CHECK_FALSE(register_type(&health));
    TypeRecord copy = health;
    CHECK_FALSE(register_type(&copy));
    CHECK(find_type(health.id) == &health);

    // A different layout under the same name is a programmer error.
    TypeRecord bigger = health;
    bigger.size += 4;
    {
        test::ExpectAssert expect;
        CHECK_FALSE(register_type(&bigger));
    }

    // Two names hashing to one StringId (forced here) must never silently alias.
    TypeRecord impostor = health;
    impostor.name = "NotHealth";
    {
        test::ExpectAssert expect;
        CHECK_FALSE(register_type(&impostor));
    }
    CHECK(find_type(health.id) == &health);
}

TEST_CASE("register/unregister keeps the table sorted") {
    static constexpr FieldRecord kFields[1] = {};
    TypeRecord a;
    a.id = StringId("AAA_Temp");
    a.name = "AAA_Temp";
    a.size = 4;
    a.align = 4;
    a.fields = Span<const FieldRecord>(kFields, 0);

    const usize before = all().size();
    CHECK(register_type(&a));
    CHECK(all().size() == before + 1);
    CHECK(all()[0] == &a);
    CHECK(find_type(StringId("AAA_Temp")) == &a);
    unregister_type(&a);
    CHECK(all().size() == before);
    CHECK(find_type(StringId("AAA_Temp")) == nullptr);
}

TEST_CASE("field kind names") {
    CHECK(std::strcmp(field_kind_name(FieldKind::Vec3), "vec3") == 0);
    CHECK(std::strcmp(field_kind_name(FieldKind::EntityRef), "entity_ref") == 0);
}
