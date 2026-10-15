// M-NET2 step 4: lag compensation for hit checks (rewound hit boxes).

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

LocalTransform at(f32 x, f32 y = 0.0f) {
    LocalTransform t;
    t.position = {x, y, 0.0f};
    return t;
}

// A server world with one target (net id 1) moving 0.1 along x per tick, recorded every tick.
struct Range {
    World world;
    ReplicationServer server;
    LagCompensator lag;
    Entity target;
    explicit Range(f64 max_rewind_s = 0.25) {
        LagCompensationDesc d;
        d.max_rewind_s = max_rewind_s;
        lag.init(d);
        target = world.spawn_with(at(0.0f), HitBox{{0.25f, 0.25f, 0.25f}, {}});
        server.assign(world, target, 1);
    }
    void run(u32 first, u32 last) {
        for (u32 tick = first; tick <= last; ++tick) {
            world.try_get_mut<LocalTransform>(target)->position.x = static_cast<f32>(tick) * 0.1f;
            lag.record(world, tick);
        }
    }
    // A shot straight up (+y) from below x.
    bool shoot(f64 view_tick, f32 x, RewindHit& hit) const {
        return lag.raycast(view_tick, {x, -5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 100.0f, hit);
    }
};

}  // namespace

TEST_CASE("net lag compensation: rays against oriented, scaled boxes") {
    HitBox box{{1.0f, 1.0f, 1.0f}, {}};
    f32 d = 0.0f;
    CHECK(ray_hits_box(at(5.0f), box, {0, 0, 0}, {1, 0, 0}, 100.0f, d));
    CHECK(d == doctest::Approx(4.0f));
    CHECK_FALSE(ray_hits_box(at(5.0f), box, {0, 0, 0}, {1, 0, 0}, 3.5f, d));  // out of range
    CHECK_FALSE(ray_hits_box(at(5.0f, 1.5f), box, {0, 0, 0}, {1, 0, 0}, 100.0f, d));
    CHECK(ray_hits_box(at(0.0f), box, {0, 0, 0}, {1, 0, 0}, 100.0f, d));  // starting inside
    CHECK(d == 0.0f);
    // Rotated 45 degrees about z: the corner points at the ray, so it is hit √2 out instead of 1.
    LocalTransform r = at(5.0f);
    r.rotation = math::quat_from_axis_angle({0, 0, 1}, 0.78539816f);
    CHECK(ray_hits_box(r, box, {0, 0, 0}, {1, 0, 0}, 100.0f, d));
    CHECK(d == doctest::Approx(5.0f - std::sqrt(2.0f)).epsilon(1e-4));
    // Scaled up, with an offset that scales with it.
    LocalTransform s = at(5.0f);
    s.scale = {2.0f, 2.0f, 2.0f};
    CHECK(ray_hits_box(s, HitBox{{1, 1, 1}, {0, 1, 0}}, {0, 3.5f, 0}, {1, 0, 0}, 100.0f, d));  // centre y 2, half 2
    CHECK(d == doctest::Approx(3.0f));
}

TEST_CASE("net lag compensation: a shot at where the client saw the target hits when rewound, misses at present") {
    Range range;
    range.run(1, 30);
    CHECK(range.lag.newest_tick() == 30);
    RewindHit hit;
    // The client drew tick 20.5: the target at x 2.05.
    REQUIRE(range.shoot(20.5, 2.05f, hit));
    CHECK(hit.net_id == 1);
    CHECK(hit.entity == range.target);
    CHECK(hit.distance == doctest::Approx(4.75f));
    CHECK(hit.tick == doctest::Approx(20.5));
    // Checked against the server's present (tick 30, x 3.0) the same shot misses.
    CHECK_FALSE(range.shoot(30.0, 2.05f, hit));
    CHECK(range.shoot(30.0, 3.0f, hit));
    // The shooter is never hit by its own shot.
    CHECK_FALSE(range.lag.raycast(20.5, {2.05f, -5.0f, 0.0f}, {0, 1, 0}, 100.0f, hit, 1));
}

TEST_CASE("net lag compensation: the rewind is limited, and a view from the future is the present") {
    Range range(0.1);  // 6 ticks at 60 Hz
    range.run(1, 30);
    CHECK(range.lag.clamp_view(10.0) == doctest::Approx(24.0));
    CHECK(range.lag.clamp_view(40.0) == doctest::Approx(30.0));
    CHECK(range.lag.clamp_view(std::numeric_limits<f64>::quiet_NaN()) == doctest::Approx(30.0));
    RewindHit hit;
    CHECK_FALSE(range.shoot(10.0, 1.0f, hit));  // too old: checked at tick 24, x 2.4
    CHECK(range.shoot(10.0, 2.4f, hit));
    CHECK(hit.tick == doctest::Approx(24.0));
    // The ring holds only what the rewind can reach.
    CHECK(range.lag.frames() <= 8);
}

TEST_CASE("net lag compensation: transforms between ticks, and entities that came or went") {
    Range range;
    range.run(1, 10);
    LocalTransform t;
    HitBox box;
    REQUIRE(range.lag.transform_at(1, 4.25, t, &box));
    CHECK(t.position.x == doctest::Approx(0.425f));
    CHECK(box.half_extents.x == 0.25f);
    CHECK_FALSE(range.lag.transform_at(2, 4.0, t));

    // A second target appears at tick 11; the first is gone from tick 12.
    const Entity late = range.world.spawn_with(at(9.0f), HitBox{});
    range.server.assign(range.world, late, 1);
    range.lag.record(range.world, 11);
    range.world.destroy(range.target);
    range.lag.record(range.world, 12);
    REQUIRE(range.lag.transform_at(2, 10.5, t));  // only in tick 11: where it appeared
    CHECK(t.position.x == 9.0f);
    REQUIRE(range.lag.transform_at(1, 11.5, t));  // only in tick 11: where it was last
    CHECK(t.position.x == doctest::Approx(1.0f));
    CHECK_FALSE(range.lag.transform_at(1, 12.0, t));
    // Entities without a HitBox take no part.
    range.world.spawn_with(at(20.0f));
    range.lag.record(range.world, 13);
    RewindHit hit;
    CHECK_FALSE(range.lag.raycast(13.0, {20.0f, -5.0f, 0.0f}, {0, 1, 0}, 100.0f, hit));
}

TEST_CASE("net lag compensation: over a 100 ms link, shots at the drawn target hit only when rewound") {
    SimNetwork network(5);
    LinkConditions c;
    c.latency_ms = 100;
    c.jitter_ms = 30;
    c.loss = 0.05f;
    network.set_default_conditions(c);
    SimEndpoint& server_end = network.add_endpoint(1, 1);
    SimEndpoint& client_end = network.add_endpoint(1, 1);
    client_end.connect(server_end);

    Range range(0.5);
    World client_world;
    ReplicationClient client;
    client.set_interpolation(InterpolationDesc{});

    u32 shots = 0, rewound = 0, present = 0;
    u64 clock_ms = 0;
    for (u32 tick = 1; tick <= 600; ++tick) {
        // Server: move 3 units/s, record, send.
        range.world.try_get_mut<LocalTransform>(range.target)->position.x = static_cast<f32>(tick) * 0.05f;
        range.lag.record(range.world, tick);
        ByteWriter w;
        range.server.write_snapshot(range.world, tick, w);
        server_end.broadcast(0, w.data(), Delivery::Sequenced);
        // Client: apply, draw, and every 20 ticks fire at the drawn target (it reaches the server
        // instantly here; the check depends only on the view tick it carries).
        for (Event ev; client_end.poll(ev);) {
            if (ev.kind == EventKind::Received) {
                client.apply(client_world.commands(), client_world, ev.data);
                client_world.sync(Phase::FixedUpdate_Pre);
            }
        }
        for (Event ev; server_end.poll(ev);) {
        }
        client.update(client_world.commands(), client_world, 1.0 / 60.0);
        client_world.sync(Phase::FixedUpdate_Pre);
        const Entity seen = client.find(1);
        if (tick > 120 && tick % 20 == 0 && client_world.alive(seen)) {
            const f32 x = client_world.try_get<LocalTransform>(seen)->position.x;
            RewindHit hit;
            ++shots;
            if (range.shoot(client.drawn_tick(1), x, hit)) ++rewound;
            if (range.shoot(static_cast<f64>(tick), x, hit)) ++present;
        }
        const u64 next_ms = static_cast<u64>(tick) * 1000 / 60;
        network.advance(static_cast<u32>(next_ms - clock_ms));
        clock_ms = next_ms;
    }
    MESSAGE("shots " << shots << ": rewound hits " << rewound << ", unrewound hits " << present);
    CHECK(shots == 24);
    CHECK(rewound == shots);
    CHECK(present == 0);  // the target moved ≥ 0.4 (more than its half width) since the client's view
}
