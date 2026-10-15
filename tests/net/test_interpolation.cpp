// M-NET2 step 2: snapshot interpolation through a jitter buffer, with an extrapolation limit.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

constexpr f64 kDt = 1.0 / 60.0;

LocalTransform at_x(f32 x) {
    LocalTransform t;
    t.position = {x, 0.0f, 0.0f};
    return t;
}

// A server world with one replicated entity at x, as snapshot bytes for `tick`.
struct Source {
    World world;
    ReplicationServer server;
    Entity e;
    Source() {
        e = world.spawn_with(at_x(0.0f));
        server.assign(world, e, 1);
    }
    Array<u8> snapshot(u32 tick, f32 x) {
        world.try_get_mut<LocalTransform>(e)->position.x = x;
        ByteWriter w;
        server.write_snapshot(world, tick, w);
        Array<u8> out;
        out.resize(w.data().size());
        std::memcpy(out.data(), w.data().data(), out.size());
        return out;
    }
};

struct Mirror {
    World world;
    ReplicationClient client;
    void apply(const Array<u8>& bytes) {
        client.apply(world.commands(), world, Span<const u8>(bytes.data(), bytes.size()));
        world.sync(Phase::FixedUpdate_Pre);
    }
    InterpolationStats update(f64 dt) {
        const InterpolationStats s = client.update(world.commands(), world, dt);
        world.sync(Phase::FixedUpdate_Pre);
        return s;
    }
    f32 x() const {
        const Entity e = client.find(1);
        return world.alive(e) ? world.try_get<LocalTransform>(e)->position.x : -1000.0f;
    }
};

}  // namespace

TEST_CASE("net interpolation: the render time blends the two snapshots around it") {
    Source src;
    Mirror m;
    InterpolationDesc d;
    d.delay_s = 10.0 / 60.0;  // ten ticks behind
    m.client.set_interpolation(d);
    REQUIRE(m.client.interpolating());

    // Snapshots every 10 ticks, x = tick; each arrives on its own tick (no latency).
    for (u32 tick = 0; tick <= 40; ++tick) {
        m.update(tick == 0 ? 0.0 : kDt);
        if (tick % 10 == 0) m.apply(src.snapshot(tick, static_cast<f32>(tick)));
    }
    // Client time 40 ticks, render time 30 ticks: exactly the tick-30 snapshot.
    CHECK(m.x() == doctest::Approx(30.0));
    for (u32 i = 0; i < 5; ++i) m.update(kDt);
    // Render time 35: halfway between 30 and 40, with nothing new arrived.
    CHECK(m.x() == doctest::Approx(35.0).epsilon(1e-4));
    const InterpolationStats s = m.client.interpolation_stats();
    CHECK(s.interpolated == 1);
    CHECK(s.delay_s == doctest::Approx(10.0 / 60.0));
}

TEST_CASE("net interpolation: past the newest snapshot it extrapolates up to the limit, then holds it exactly") {
    Source src;
    Mirror m;
    InterpolationDesc d;
    d.delay_s = 2.0 / 60.0;
    d.max_extrapolation_s = 6.5 / 60.0;
    m.client.set_interpolation(d);
    m.update(0.0);
    m.apply(src.snapshot(0, 0.0f));
    for (u32 tick = 1; tick <= 10; ++tick) {
        m.update(kDt);
        m.apply(src.snapshot(tick, static_cast<f32>(tick)));  // 1 unit per tick
    }
    CHECK(m.x() == doctest::Approx(8.0));  // two ticks behind
    // The server goes quiet. Render time 10 is the newest; past it the entity moves on at 1 unit
    // per tick for up to 6.5 ticks.
    for (u32 i = 0; i < 6; ++i) m.update(kDt);  // render time 14
    CHECK(m.x() == doctest::Approx(14.0).epsilon(1e-4));
    CHECK(m.client.interpolation_stats().extrapolated == 1);
    for (u32 i = 0; i < 2; ++i) m.update(kDt);  // 16
    CHECK(m.x() == doctest::Approx(16.0).epsilon(1e-4));
    m.update(kDt);  // 17: past the limit, back to the newest snapshot bit for bit
    CHECK(m.x() == 10.0f);
    CHECK(m.client.interpolation_stats().held == 1);
}

TEST_CASE("net interpolation: over a jittery, lossy link the mirror moves smoothly; raw snapshots stutter") {
    // One entity moving at constant speed, snapshots every tick over SimNetwork.
    auto run = [](bool interpolate, f64& delay_out) {
        SimNetwork network(11);
        LinkConditions c;
        c.latency_ms = 60;
        c.jitter_ms = 40;
        c.loss = 0.1f;
        network.set_default_conditions(c);
        SimEndpoint& server = network.add_endpoint(1, 1);
        SimEndpoint& client = network.add_endpoint(1, 1);
        const PeerId to_server = client.connect(server);
        Source src;
        Mirror m;
        if (interpolate) m.client.set_interpolation(InterpolationDesc{});
        f64 jerk = 0.0;
        f32 x0 = 0.0f, x1 = 0.0f;
        u32 samples = 0, counted = 0;
        u64 clock_ms = 0;
        for (u32 tick = 1; tick <= 600; ++tick) {
            Array<u8> snap = src.snapshot(tick, static_cast<f32>(tick) * 0.1f);
            server.broadcast(0, Span<const u8>(snap.data(), snap.size()), Delivery::Sequenced);
            for (Event ev; client.poll(ev);) {
                if (ev.kind != EventKind::Received) continue;
                Array<u8> bytes;
                bytes.resize(ev.data.size());
                std::memcpy(bytes.data(), ev.data.data(), bytes.size());
                m.apply(bytes);
            }
            for (Event ev; server.poll(ev);) {
            }
            m.update(kDt);
            const u64 next_ms = static_cast<u64>(tick) * 1000 / 60;
            network.advance(static_cast<u32>(next_ms - clock_ms));
            clock_ms = next_ms;
            if (tick > 120 && client.is_connected(to_server)) {  // after the buffer settles
                const f32 x = m.x();
                if (samples >= 2) {
                    jerk += std::fabs(static_cast<f64>(x - x1 - (x1 - x0)));
                    ++counted;
                }
                x0 = x1;
                x1 = x;
                ++samples;
            }
        }
        delay_out = m.client.interpolation_stats().delay_s;
        return counted ? jerk / counted : 0.0;
    };
    f64 delay = 0.0, unused = 0.0;
    const f64 raw = run(false, unused);
    const f64 smooth = run(true, delay);
    MESSAGE("mean |second difference| per tick: raw " << raw << ", interpolated " << smooth << ", buffer " << delay
                                                      << " s");
    CHECK(raw > 0.01);
    CHECK(smooth < raw / 10.0);
    CHECK(delay > 0.05);   // above the floor: it measured the jitter
    CHECK(delay <= 0.35);  // within the ceiling
}

TEST_CASE("net interpolation: the adaptive buffer grows with arrival jitter") {
    auto buffer_for = [](u32 jitter_ms) {
        SimNetwork network(3);
        LinkConditions c;
        c.latency_ms = 40;
        c.jitter_ms = jitter_ms;
        network.set_default_conditions(c);
        SimEndpoint& server = network.add_endpoint(1, 1);
        SimEndpoint& client = network.add_endpoint(1, 1);
        client.connect(server);
        Source src;
        Mirror m;
        m.client.set_interpolation(InterpolationDesc{});
        u64 clock_ms = 0;
        for (u32 tick = 1; tick <= 300; ++tick) {
            Array<u8> snap = src.snapshot(tick, 0.0f);
            server.broadcast(0, Span<const u8>(snap.data(), snap.size()), Delivery::Unreliable);
            for (Event ev; client.poll(ev);) {
                if (ev.kind != EventKind::Received) continue;
                Array<u8> bytes;
                bytes.resize(ev.data.size());
                std::memcpy(bytes.data(), ev.data.data(), bytes.size());
                m.apply(bytes);
            }
            m.update(kDt);
            const u64 next_ms = static_cast<u64>(tick) * 1000 / 60;
            network.advance(static_cast<u32>(next_ms - clock_ms));
            clock_ms = next_ms;
        }
        return m.client.interpolation_stats().delay_s;
    };
    const f32 calm = buffer_for(0), rough = buffer_for(80);
    CHECK(calm == doctest::Approx(0.05));  // the floor: one tick apart, no jitter
    CHECK(rough > calm + 0.05f);
}
