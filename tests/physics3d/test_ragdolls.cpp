// World3D ragdolls (M-GAMEPLAY3D step 3): refused descriptions, a stick figure that falls, holds
// together and settles, the swing limit, impulses, queries that report the owner, and determinism.

#include <cmath>

#include "helpers.h"

using namespace phys3_test;

namespace {

// A stick figure standing on y = 0: pelvis (sphere), spine (capsule), head (sphere), two legs
// (capsules hanging from the pelvis). 5 parts, 70 kg.
Array<RagdollPart3D> figure(vec3 base, f32 swing = 0.6f) {
    Array<RagdollPart3D> parts;
    RagdollPart3D pelvis;
    pelvis.shape = ShapeType3D::Sphere;
    pelvis.radius = 0.15f;
    pelvis.position = base + vec3{0, 1.0f, 0};
    pelvis.mass = 15.0f;
    parts.push_back(pelvis);
    RagdollPart3D spine;
    spine.parent = 0;
    spine.radius = 0.12f;
    spine.half_height = 0.2f;
    spine.position = base + vec3{0, 1.4f, 0};
    spine.pivot = base + vec3{0, 1.1f, 0};
    spine.twist_axis = {0, 1, 0};
    spine.swing = swing;
    spine.mass = 30.0f;
    parts.push_back(spine);
    RagdollPart3D head;
    head.parent = 1;
    head.shape = ShapeType3D::Sphere;
    head.radius = 0.11f;
    head.position = base + vec3{0, 1.85f, 0};
    head.pivot = base + vec3{0, 1.72f, 0};
    head.mass = 5.0f;
    parts.push_back(head);
    for (f32 side : {-0.12f, 0.12f}) {
        RagdollPart3D leg;
        leg.parent = 0;
        leg.radius = 0.08f;
        leg.half_height = 0.35f;
        leg.position = base + vec3{side, 0.5f, 0};
        leg.pivot = base + vec3{side, 0.92f, 0};
        leg.twist_axis = {0, -1, 0};
        leg.swing = swing;
        leg.mass = 10.0f;
        parts.push_back(leg);
    }
    return parts;
}

RagdollDesc3D owner(u32 i) {
    RagdollDesc3D d;
    d.owner = ent(i);
    return d;
}

Array<BodyState3D> states(const World3D& w, u32 r) {
    Array<BodyState3D> s;
    s.resize(w.ragdoll_part_count(r));
    REQUIRE(w.ragdoll_state(r, s.span()));
    return s;
}

void steps(World3D& w, int n) {
    for (int i = 0; i < n; ++i) w.step(kDt);
}

}  // namespace

TEST_CASE("ragdolls: refused descriptions, lifecycle, ids not reused") {
    World3D w;
    add_floor(w);
    const u32 bodies = w.stats().bodies;
    CHECK(w.create_ragdoll(owner(1), {}) == 0);
    Array<RagdollPart3D> parts = figure({});
    parts[0].parent = 0;  // the root has no parent
    CHECK(w.create_ragdoll(owner(1), parts.span()) == 0);
    parts = figure({});
    parts[2].parent = 3;  // parents come first
    CHECK(w.create_ragdoll(owner(1), parts.span()) == 0);
    parts = figure({});
    parts[1].radius = 0.0f;
    CHECK(w.create_ragdoll(owner(1), parts.span()) == 0);
    parts = figure({});
    parts[3].mass = 0.0f;
    CHECK(w.create_ragdoll(owner(1), parts.span()) == 0);
    parts = figure({});
    parts[1].shape = ShapeType3D::Mesh;
    CHECK(w.create_ragdoll(owner(1), parts.span()) == 0);
    CHECK(w.stats().bodies == bodies);

    parts = figure({});
    const u32 r = w.create_ragdoll(owner(1), parts.span());
    REQUIRE(r != 0);
    CHECK(w.has_ragdoll(r));
    CHECK(w.ragdoll_part_count(r) == 5);
    CHECK(w.stats().ragdolls == 1);
    CHECK(w.stats().bodies == bodies + 5);
    w.destroy_ragdoll(r);
    CHECK_FALSE(w.has_ragdoll(r));
    CHECK(w.ragdoll_part_count(r) == 0);
    CHECK(w.stats().ragdolls == 0);
    CHECK(w.stats().bodies == bodies);
    const u32 again = w.create_ragdoll(owner(1), parts.span());
    CHECK(again != 0);
    CHECK(again != r);
    CHECK_FALSE(w.has_ragdoll(r));
}

TEST_CASE("ragdolls: a figure pushed over falls, holds together and settles on the floor") {
    World3D w;
    add_floor(w);
    const Array<RagdollPart3D> parts = figure({});
    const u32 r = w.create_ragdoll(owner(7), parts.span());
    REQUIRE(r != 0);
    w.ragdoll_impulse(r, 1, {40.0f, 0, 0});  // a shove at the chest
    CHECK(w.ragdoll_active(r));
    steps(w, 300);
    CHECK_FALSE(w.ragdoll_active(r));  // asleep
    const Array<BodyState3D> s = states(w, r);
    for (const BodyState3D& b : s) {
        CHECK(b.entity == ent(7));
        CHECK(b.position.y > 0.0f);
        CHECK(b.position.y < 0.5f);  // lying down
    }
    CHECK(s[1].position.x > 0.3f);  // fell the way it was pushed
    // Held together: each joint's pivot, carried by both bodies, is still one point.
    for (usize i = 1; i < parts.size(); ++i) {
        const RagdollPart3D& p = parts[i];
        const RagdollPart3D& q = parts[static_cast<usize>(p.parent)];
        const vec3 in_child = math::rotate(math::conjugate(p.rotation), p.pivot - p.position);
        const vec3 in_parent = math::rotate(math::conjugate(q.rotation), p.pivot - q.position);
        const vec3 a = s[i].position + math::rotate(s[i].rotation, in_child);
        const vec3 b = s[static_cast<usize>(p.parent)].position + math::rotate(s[static_cast<usize>(p.parent)].rotation, in_parent);
        CHECK(math::length(a - b) < 0.02f);
    }
}

TEST_CASE("ragdolls: the swing limit holds the spine on the pelvis") {
    // The pelvis is held up by the floor's corner; the spine leans over the edge as far as its limit.
    for (f32 swing : {0.3f, 1.0f}) {
        World3D w;
        add_floor(w);
        Array<RagdollPart3D> parts = figure({0, 2.0f, 0}, swing);
        parts.resize(2);  // pelvis and spine
        parts[0].shape = ShapeType3D::Box;
        parts[0].half = {0.3f, 0.15f, 0.3f};
        parts[0].mass = 200.0f;  // heavy: stays put
        parts[0].position = {0, 0.15f, 0};
        parts[1].pivot = {0.3f, 0.15f, 0};
        parts[1].position = {0.3f, 0.6f, 0};
        parts[1].twist_axis = {0, 1, 0};
        parts[1].mass = 5.0f;
        const u32 r = w.create_ragdoll(owner(1), parts.span());
        REQUIRE(r != 0);
        w.ragdoll_impulse(r, 1, {10.0f, 0, 0});
        f32 widest = 0.0f;
        for (int i = 0; i < 180; ++i) {
            w.step(kDt);
            const Array<BodyState3D> s = states(w, r);
            const vec3 up = math::rotate(math::conjugate(s[0].rotation), math::rotate(s[1].rotation, vec3{0, 1, 0}));
            widest = math::max(widest, std::acos(math::clamp(up.y, -1.0f, 1.0f)));
        }
        CHECK(widest > swing * 0.8f);
        CHECK(widest < swing + 0.1f);
    }
}

TEST_CASE("ragdolls: an impulse spread over the parts moves them alike; queries report the owner") {
    World3D w;
    w.set_gravity({0, 0, 0});
    const u32 r = w.create_ragdoll(owner(9), figure({0, 5, 0}).span());
    REQUIRE(r != 0);
    w.ragdoll_impulse(r, -1, {70.0f, 0, 0});  // 70 kg: 1 m/s each
    w.step(kDt);
    for (const BodyState3D& b : states(w, r)) CHECK(b.linear_velocity.x == doctest::Approx(1.0f).epsilon(0.02));
    const auto hit = w.raycast({-5, 6.4f, 0}, {1, 0, 0}, 20.0f);
    REQUIRE(hit);
    CHECK(hit->entity == ent(9));
    QueryFilter3D f;
    Entity self[] = {ent(9)};
    f.exclude = Span<const Entity>(self, 1);
    CHECK_FALSE(w.raycast({-5, 6.4f, 0}, {1, 0, 0}, 20.0f, f));
}

TEST_CASE("ragdolls: two ragdolls collide with each other; the replay hash holds") {
    // A one-part ragdoll (a crate) dropped onto another comes to rest on it.
    auto crate = [](vec3 at) {
        Array<RagdollPart3D> p;
        p.emplace_back();
        p[0].shape = ShapeType3D::Box;
        p[0].half = {0.5f, 0.25f, 0.5f};
        p[0].position = at;
        p[0].mass = 20.0f;
        return p;
    };
    World3D w;
    add_floor(w);
    const u32 low = w.create_ragdoll(owner(1), crate({0, 0.25f, 0}).span());
    const u32 high = w.create_ragdoll(owner(2), crate({0, 1.5f, 0}).span());
    REQUIRE(low != 0);
    REQUIRE(high != 0);
    steps(w, 120);
    CHECK(states(w, high)[0].position.y == doctest::Approx(0.75f).epsilon(0.02));

    auto run = [] {
        World3D v;
        add_floor(v);
        REQUIRE(v.create_ragdoll(owner(1), figure({}).span()) != 0);
        const u32 b = v.create_ragdoll(owner(2), figure({0.05f, 2.2f, 0}).span());  // dropped onto the first
        REQUIRE(b != 0);
        v.ragdoll_impulse(b, 0, {5.0f, 0, 0});
        steps(v, 240);
        return v.hash_state(0);
    };
    CHECK(run() == run());
}

TEST_CASE("ragdolls: a world that never had one keeps its hash") {
    World3D a, b;
    add_floor(a);
    add_floor(b);
    const u32 r = b.create_ragdoll(owner(1), figure({0, 10, 0}).span());
    b.destroy_ragdoll(r);
    CHECK(a.hash_state(0) != b.hash_state(0));  // b had one
    World3D c;
    add_floor(c);
    CHECK(a.hash_state(0) == c.hash_state(0));
}
