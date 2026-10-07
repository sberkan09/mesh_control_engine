#include <doctest/doctest.h>

#include "mce/ecs/world.h"

using namespace mce;

namespace {
struct Damage {
    u32 target;
    f32 amount;
};

u32 g_seen_post = 0, g_seen_pre = 0;
void produce(Frame& f) {
    f.events().push(Damage{1, 2.0f});
    f.events().push(Damage{2, 3.0f});
}
void consume_post(Frame& f) { g_seen_post += static_cast<u32>(f.events().read<Damage>().size()); }
void consume_pre(Frame& f) { g_seen_pre += static_cast<u32>(f.events().read<Damage>().size()); }
}  // namespace

TEST_CASE("events: double-buffered, swapped at the producer phase's sync point (FR-ENT-030)") {
    World w;
    w.register_event<Damage>(Phase::FixedUpdate_Physics);
    w.add_system(Phase::FixedUpdate_Pre, "consume_pre", &consume_pre);
    w.add_system(Phase::FixedUpdate_Physics, "produce", &produce);
    w.add_system(Phase::FixedUpdate_Post, "consume_post", &consume_post);
    g_seen_post = g_seen_pre = 0;

    w.tick(1.0 / 60, 1);
    CHECK(g_seen_post == 2);  // same tick, after the producer phase
    CHECK(g_seen_pre == 0);   // before the producer phase: nothing yet
    w.tick(1.0 / 60, 2);
    CHECK(g_seen_pre == 2);   // tick-1 events, seen exactly once before the next swap
    CHECK(g_seen_post == 4);  // tick-2 events
}

TEST_CASE("events: readers never see a partially written list") {
    World w;
    w.register_event<Damage>(Phase::FixedUpdate_Pre);
    auto* q = w.event_queue<Damage>();
    REQUIRE(q);
    EventAccess ev(w);
    ev.push(Damage{1, 1});
    CHECK(ev.read<Damage>().empty());  // still in the back buffer
    ev.push(Damage{2, 1});
    w.sync(Phase::FixedUpdate_Pre);
    CHECK(ev.read<Damage>().size() == 2);
    ev.push(Damage{3, 1});
    CHECK(ev.read<Damage>().size() == 2);  // the readable list does not change mid-phase
    w.sync(Phase::FixedUpdate_Pre);
    REQUIRE(ev.read<Damage>().size() == 1);
    CHECK(ev.read<Damage>()[0].target == 3);
}

TEST_CASE("event registry records counts, producers and consumers") {
    World w;
    w.register_event<Damage>(Phase::FixedUpdate_Physics);
    w.add_system(Phase::FixedUpdate_Physics, "produce", &produce);
    w.add_system(Phase::FixedUpdate_Post, "consume_post", &consume_post);
    w.tick(1.0 / 60, 1);
    w.tick(1.0 / 60, 2);
    Array<EventInfo> infos;
    w.event_registry(infos);
    REQUIRE(infos.size() == 1);
    CHECK(infos[0].total_pushed == 4);
    CHECK(infos[0].producer_phase == Phase::FixedUpdate_Physics);
    REQUIRE(infos[0].producers.size() == 1);
    CHECK(infos[0].producers[0] == "produce");
    REQUIRE(infos[0].consumers.size() == 1);
    CHECK(infos[0].consumers[0] == "consume_post");
    CHECK(infos[0].name.find("Damage") != StringView::npos);
}
