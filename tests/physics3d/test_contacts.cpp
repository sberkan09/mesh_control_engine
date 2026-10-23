// 3D contact events (M-CONTACTEVENTS step 1): the seam. The listener comes in step 2.

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
