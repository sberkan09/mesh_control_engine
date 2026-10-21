// Change detection (v0.5, FR-ENT-040): per-row added/changed ticks, query filters, reader ticks
// per system, and the wrapping tick comparison.

#include <doctest/doctest.h>

#include "mce/ecs/world.h"
#include "test_support.h"

using namespace mce;

namespace {
struct Pos {
    f32 x = 0, y = 0;
};
struct Vel {
    f32 x = 0, y = 0;
};
struct Tag {};

struct Seen {
    u32 changed = 0;
    u32 added = 0;
};
}  // namespace

TEST_CASE("change detection: direct writes stamp ticks; since() filters rows") {
    World w;
    Entity a = w.spawn_with(Pos{1, 1});
    Entity b = w.spawn_with(Pos{2, 2});
    const u32 t0 = w.change_tick();
    CHECK(w.is_added<Pos>(a, 0));
    CHECK_FALSE(w.is_changed<Pos>(a, t0));

    w.set(b, Pos{3, 3});
    CHECK(w.is_changed<Pos>(b, t0));
    CHECK_FALSE(w.is_changed<Pos>(a, t0));
    CHECK_FALSE(w.is_added<Pos>(b, t0));  // changed, not added
    CHECK(w.query<const Pos&>().changed<Pos>().since(t0).count() == 1);

    const u32 t1 = w.change_tick();
    w.try_get_mut<Pos>(a)->x = 9;  // try_get_mut declares the write
    CHECK(w.is_changed<Pos>(a, t1));
    const u32 t2 = w.change_tick();
    Pos* kept = w.try_get_mut<Pos>(a);
    const u32 t3 = w.change_tick();
    kept->y = 7;  // a retained pointer is invisible...
    CHECK_FALSE(w.is_changed<Pos>(a, t3));
    w.touch<Pos>(a);  // ...until touched
    CHECK(w.is_changed<Pos>(a, t3));
    CHECK(w.is_changed<Pos>(a, t2));

    // Adding a second component keeps the first one's ticks and stamps the new one as added.
    const u32 t4 = w.change_tick();
    w.add(a, Vel{1, 0});
    CHECK(w.is_added<Vel>(a, t4));
    CHECK_FALSE(w.is_changed<Pos>(a, t4));
    CHECK(w.query<const Pos&>().added<Vel>().since(t4).count() == 1);
    CHECK(w.query<const Pos&>().added<Pos>().since(t4).count() == 0);
}

TEST_CASE("change detection: swap-remove and archetype moves carry ticks with their rows") {
    World w;
    Entity a = w.spawn_with(Pos{1, 0});
    Entity b = w.spawn_with(Pos{2, 0});
    Entity c = w.spawn_with(Pos{3, 0});
    const u32 t0 = w.change_tick();
    w.set(c, Pos{4, 0});
    w.destroy(a);  // c moves into a's row
    CHECK(w.is_changed<Pos>(c, t0));
    CHECK_FALSE(w.is_changed<Pos>(b, t0));
    w.add(b, Tag{});  // b moves tables
    CHECK_FALSE(w.is_changed<Pos>(b, t0));
    CHECK(w.query<const Pos&>().changed<Pos>().since(t0).count() == 1);
}

TEST_CASE("change detection: a system sees writes made after its previous run, not its own") {
    World w;
    w.set_resource(Seen{});
    Entity a = w.spawn_with(Pos{}, Vel{1, 0});
    Entity b = w.spawn_with(Pos{}, Vel{0, 0});
    (void)b;
    // Pre: reader of changed Pos. Physics: moves only entities with nonzero velocity.
    w.add_system(Phase::FixedUpdate_Pre, "reader", [](Frame& f) {
        Seen& s = f.resource<Seen>();
        s.changed = static_cast<u32>(f.query<const Pos&>().changed<Pos>().count());
        s.added = static_cast<u32>(f.query<const Pos&>().added<Pos>().count());
    });
    w.add_system(Phase::FixedUpdate_Physics, "mover", [](Frame& f) {
        f.query<const Vel&>().each([&](Entity e, const Vel& v) {
            if (v.x != 0.0f) f.world().try_get_mut<Pos>(e)->x += v.x;
        });
    });
    w.tick(1.0 / 60, 0);
    CHECK(w.resource<Seen>().changed == 2);  // first run: everything is new
    CHECK(w.resource<Seen>().added == 2);
    w.tick(1.0 / 60, 1);
    CHECK(w.resource<Seen>().changed == 1);  // only the mover's write since the last run
    CHECK(w.resource<Seen>().added == 0);
    w.set(a, Vel{0, 0});
    w.tick(1.0 / 60, 2);
    CHECK(w.resource<Seen>().changed == 1);  // last tick's move is still newer than the last read
    w.tick(1.0 / 60, 3);
    CHECK(w.resource<Seen>().changed == 0);
}

TEST_CASE("change detection: mutable query items stamp only the rows they visit") {
    World w;
    for (int i = 0; i < 10; ++i) w.spawn_with(Pos{f32(i), 0}, Vel{});
    Entity odd = w.spawn_with(Pos{100, 0});
    const u32 t0 = w.change_tick();
    w.query<Pos&>().with<Vel>().each([](Pos& p) { p.y = 1; });
    CHECK(w.query<const Pos&>().changed<Pos>().since(t0).count() == 10);
    CHECK_FALSE(w.is_changed<Pos>(odd, t0));
    const u32 t1 = w.change_tick();
    w.query<const Pos&>().each([](const Pos&) {});  // const access never stamps
    CHECK(w.query<const Pos&>().changed<Pos>().since(t1).count() == 0);
}

TEST_CASE("change detection: Commands stamp at the sync point; Frame helpers use the reader tick") {
    World w;
    struct Flags {
        bool added_seen = false;
        bool changed_seen = false;
    };
    w.set_resource(Flags{});
    w.set_resource(Entity{});
    w.add_system(Phase::FixedUpdate_Pre, "spawner", [](Frame& f) {
        if (f.tick == 0) f.resource<Entity>() = f.commands().spawn(Pos{5, 5});
        if (f.tick == 2) f.commands().set(f.resource<Entity>(), Pos{6, 6});
    });
    w.add_system(Phase::FixedUpdate_Post, "watcher", [](Frame& f) {
        const Entity e = f.resource<Entity>();
        Flags& fl = f.resource<Flags>();
        fl.added_seen = f.is_added<Pos>(e);
        fl.changed_seen = f.is_changed<Pos>(e);
    });
    w.tick(1.0 / 60, 0);
    CHECK(w.resource<Flags>().added_seen);
    w.tick(1.0 / 60, 1);
    CHECK_FALSE(w.resource<Flags>().added_seen);
    CHECK_FALSE(w.resource<Flags>().changed_seen);
    w.tick(1.0 / 60, 2);
    CHECK(w.resource<Flags>().changed_seen);
    CHECK_FALSE(w.resource<Flags>().added_seen);
}

TEST_CASE("change detection: ticks survive counter wraparound") {
    World w;
    w.set_resource(Seen{});
    Entity a = w.spawn_with(Pos{});
    w.spawn_with(Pos{});
    w.add_system(Phase::FixedUpdate_Post, "reader", [](Frame& f) {
        f.resource<Seen>().changed = static_cast<u32>(f.query<const Pos&>().changed<Pos>().count());
    });
    w.tick(1.0 / 60, 0);
    CHECK(w.resource<Seen>().changed == 2);
    w.debug_set_change_tick(0xFFFFFFF0u);  // a few ticks before wrapping
    for (u64 t = 1; t < 40; ++t) {
        if (t % 3 == 0) w.set(a, Pos{f32(t), 0});
        w.tick(1.0 / 60, t);
        CHECK(w.resource<Seen>().changed == (t % 3 == 0 ? 1u : 0u));
    }
    CHECK(w.change_tick() < 0x1000u);  // wrapped
    // Ages beyond the window clamp instead of aliasing as "new".
    const u32 t0 = w.change_tick();
    w.debug_set_change_tick(t0 + (3u << 30) + 100u);
    w.tick(1.0 / 60, 40);
    CHECK(w.resource<Seen>().changed == 0);
}
