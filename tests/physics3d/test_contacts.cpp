// 3D contact events (M-CONTACTEVENTS step 1): the seam. The listener comes in step 2.

#include <algorithm>
#include <cmath>

#include "helpers.h"

using namespace phys3_test;

TEST_CASE("contacts: the event is 48 bytes and a fresh world reports none") {
    static_assert(sizeof(ContactEvent3D) == 48);
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 2, 0}, {0.5f, 0.5f, 0.5f}));
    for (int i = 0; i < 60; ++i) w.step(kDt);
    CHECK(w.contact_events().size() == 0);
}

TEST_CASE("contacts: opting in is a no-op for an unknown entity") {
    World3D w;
    add_floor(w);
    CHECK(w.set_contact_events(ent(1000), 2.0f));
    CHECK_FALSE(w.set_contact_events(ent(77), 2.0f));
    CHECK(w.set_contact_events(ent(1000), -1.0f));
}

namespace {

// Steps `n` times and returns every event reported, in order.
Array<ContactEvent3D> run(World3D& w, int n) {
    Array<ContactEvent3D> all;
    for (int i = 0; i < n; ++i) {
        w.step(kDt);
        for (const ContactEvent3D& e : w.contact_events()) all.push_back(e);
    }
    return all;
}

}  // namespace

TEST_CASE("contacts: a box dropped on the floor reports one hit with its speed and normal") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 3.5f, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(w.set_contact_events(ent(1), 2.0f));
    const Array<ContactEvent3D> ev = run(w, 180);  // lands, may settle for a long while: still one
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].a == ent(1));
    CHECK(ev[0].b == ent(1000));
    CHECK(math::near(ev[0].normal, {0, -1, 0}, 1e-2f));  // from a (the box) to b (the floor)
    CHECK(ev[0].approach_speed == doctest::Approx(std::sqrt(2.0f * 9.81f * 3.0f)).epsilon(0.15));
    CHECK(ev[0].impulse > 0.0f);
}

TEST_CASE("contacts: no opt-in, or a threshold above the speed, reports nothing") {
    {
        World3D w;
        add_floor(w);
        REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 3.5f, 0}, {0.5f, 0.5f, 0.5f}));
        CHECK(run(w, 120).size() == 0);
    }
    {
        World3D w;
        add_floor(w);
        REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 3.5f, 0}, {0.5f, 0.5f, 0.5f}));
        REQUIRE(w.set_contact_events(ent(1), 50.0f));
        CHECK(run(w, 120).size() == 0);
    }
}

TEST_CASE("contacts: the floor opting in is enough, and a switched-off body is silent again") {
    World3D w;
    add_floor(w);
    REQUIRE(add_box(w, ent(1), BodyType3D::Dynamic, {0, 3.5f, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(w.set_contact_events(ent(1000), 2.0f));
    CHECK(run(w, 120).size() == 1);

    World3D off;
    add_floor(off);
    REQUIRE(add_box(off, ent(1), BodyType3D::Dynamic, {0, 3.5f, 0}, {0.5f, 0.5f, 0.5f}));
    REQUIRE(off.set_contact_events(ent(1), 2.0f));
    REQUIRE(off.set_contact_events(ent(1), -1.0f));
    CHECK(run(off, 120).size() == 0);
}

TEST_CASE("contacts: events come sorted by pair whatever the creation order, and the cap holds") {
    const auto build = [](World3D& w) {
        add_floor(w);
        for (u32 i = 5; i >= 1; --i) {  // created in reverse
            REQUIRE(add_box(w, ent(i), BodyType3D::Dynamic, {static_cast<f32>(i) * 3.0f, 3.5f, 0}, {0.5f, 0.5f, 0.5f}));
            REQUIRE(w.set_contact_events(ent(i), 2.0f));
        }
    };
    World3D w;
    build(w);
    bool saw_five = false;
    for (int i = 0; i < 120; ++i) {
        w.step(kDt);
        const Span<const ContactEvent3D> ev = w.contact_events();
        saw_five = saw_five || ev.size() == 5;
        for (usize k = 1; k < ev.size(); ++k) CHECK(ev[k - 1].a.index < ev[k].a.index);
    }
    CHECK(saw_five);

    PhysicsConfig3D cfg;
    cfg.max_contact_events_per_tick = 2;
    World3D capped(cfg);
    build(capped);
    usize most = 0;
    for (int i = 0; i < 120; ++i) {
        capped.step(kDt);
        most = std::max(most, capped.contact_events().size());
    }
    CHECK(most == 2);
}

TEST_CASE("contacts: two runs report the same events and keep the same hash") {
    const auto go = [](u64& hash) {
        World3D w;
        add_floor(w);
        for (u32 i = 1; i <= 3; ++i) {
            REQUIRE(add_box(w, ent(i), BodyType3D::Dynamic, {static_cast<f32>(i) * 2.0f, 2.0f + static_cast<f32>(i), 0}, {0.5f, 0.5f, 0.5f}));
            REQUIRE(w.set_contact_events(ent(i), 1.0f));
        }
        Array<ContactEvent3D> ev = run(w, 240);
        hash = w.hash_state(1);
        return ev;
    };
    u64 h1 = 0, h2 = 0;
    const Array<ContactEvent3D> a = go(h1);
    const Array<ContactEvent3D> b = go(h2);
    REQUIRE(a.size() == b.size());
    CHECK(a.size() >= 3);
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(a[i].a == b[i].a);
        CHECK(a[i].b == b[i].b);
        CHECK(a[i].approach_speed == b[i].approach_speed);
    }
    CHECK(h1 == h2);
}
