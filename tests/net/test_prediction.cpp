// M-NET2 step 3: client-side prediction, server reconciliation, misprediction smoothing.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/net/net.h"

using namespace mce;
using namespace mce::net;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Input {
    f32 move_x;
};
struct State {
    LocalTransform transform;
    f32 speed = 3.0f;
};

void step(void*, State& s, const Input& in, f32 dt) {
    const f32 move = std::isfinite(in.move_x) ? std::fmax(-1.0f, std::fmin(1.0f, in.move_x)) : 0.0f;
    s.transform.position.x += move * s.speed * dt;
}

Array<u8> packet_of(const Predictor<Input, State>& p) {
    ByteWriter w;
    Array<u8> out;
    if (!p.write_inputs(w)) return out;
    out.resize(w.size());
    std::memcpy(out.data(), w.data().data(), out.size());
    return out;
}

bool same_bits(f32 a, f32 b) { return std::memcmp(&a, &b, sizeof a) == 0; }

}  // namespace

TEST_CASE("net prediction: inputs travel with redundancy and the server runs each once, in order") {
    Predictor<Input, State> client;
    PredictionDesc d;
    d.redundancy = 4;
    client.init(d, step);
    client.reset(State{});
    InputReceiver<Input> server;
    Array<u32> ran;
    auto deliver = [&](const Array<u8>& bytes) {
        return server.receive(Span<const u8>(bytes.data(), bytes.size()),
                              [&](const Input& in) { ran.push_back(static_cast<u32>(in.move_x)); });
    };
    // Inputs 1..10 (move_x carries the number); packets 4, 5 and 6 are lost.
    Array<u8> stale;
    for (u32 i = 1; i <= 10; ++i) {
        CHECK(client.push(Input{static_cast<f32>(i)}) == i);
        const Array<u8> bytes = packet_of(client);
        if (i == 2) stale = bytes;
        if (i >= 4 && i <= 6) continue;
        deliver(bytes);
    }
    // Packet 7 carried 4..7, so nothing was lost; every input ran exactly once.
    REQUIRE(ran.size() == 10);
    for (u32 i = 0; i < 10; ++i) CHECK(ran[i] == i + 1);
    CHECK(server.last_seq() == 10);
    // A late duplicate runs nothing.
    CHECK(deliver(stale) == 0);
    CHECK(ran.size() == 10);
    // Without acks the client keeps resending the newest inputs.
    CHECK(client.stats().pending == 10);
}

TEST_CASE("net prediction: a gap wider than the redundancy skips the lost inputs") {
    Predictor<Input, State> client;
    PredictionDesc d;
    d.redundancy = 2;
    client.init(d, step);
    client.reset(State{});
    InputReceiver<Input> server;
    u32 ran = 0;
    for (u32 i = 1; i <= 6; ++i) {
        client.push(Input{1.0f});
        const Array<u8> bytes = packet_of(client);
        if (i == 1 || i == 6) ran += server.receive(Span<const u8>(bytes.data(), bytes.size()), [](const Input&) {});
    }
    CHECK(ran == 3);  // 1, then 5 and 6; 2..4 never arrived
    CHECK(server.last_seq() == 6);
}

TEST_CASE("net prediction: malformed input packets run nothing") {
    InputReceiver<Input> server;
    u32 ran = 0;
    auto run = [&](const ByteWriter& w) { return server.receive(w.data(), [&](const Input&) { ++ran; }); };
    ByteWriter w;
    net::detail::write_input_header(w, sizeof(Input), 5, 2);
    w.write_f32(1.0f);  // one input short
    CHECK(run(w) == 0);
    w.clear();
    net::detail::write_input_header(w, 8, 5, 1);  // another input type
    w.write_f32(1.0f);
    w.write_f32(1.0f);
    CHECK(run(w) == 0);
    w.clear();
    net::detail::write_input_header(w, sizeof(Input), 5, 0);  // empty
    CHECK(run(w) == 0);
    CHECK(ran == 0);
    CHECK(server.last_seq() == 0);
}

TEST_CASE("net prediction: when the server agrees, reconcile leaves the prediction bit for bit") {
    Predictor<Input, State> client;
    client.init(PredictionDesc{}, step);
    client.reset(State{});
    InputReceiver<Input> receiver;
    State server;
    // 30 ticks; the server sees every packet 6 ticks late and acks with its state.
    Array<Array<u8>> in_flight;
    for (u32 t = 0; t < 30; ++t) {
        client.push(Input{t % 7 == 0 ? -1.0f : 0.8f});
        in_flight.push_back(packet_of(client));
        if (t >= 6) {
            const Array<u8>& bytes = in_flight[t - 6];
            receiver.receive(Span<const u8>(bytes.data(), bytes.size()),
                             [&](const Input& in) { step(nullptr, server, in, kDt); });
            const f32 before = client.state().transform.position.x;
            client.reconcile(receiver.last_seq(), server);
            client.smooth(kDt);
            CHECK(same_bits(client.state().transform.position.x, before));
            CHECK(client.stats().pending == 6);
        }
    }
    CHECK(client.stats().corrections == 0);
    CHECK(same_bits(client.displayed().position.x, client.state().transform.position.x));
}

TEST_CASE("net prediction: a misprediction is replayed at once and smoothed on screen") {
    Predictor<Input, State> client;
    PredictionDesc d;
    d.correction_rate = 10.0f;
    client.init(d, step);
    client.reset(State{});
    for (u32 t = 0; t < 10; ++t) client.push(Input{1.0f});  // predicted x = 10 * 3/60 = 0.5
    const f32 drawn_before = client.displayed().position.x;
    CHECK(drawn_before == doctest::Approx(0.5f));

    // The server ran inputs 1..4 but something only it knows pushed the player back by 0.3.
    State server;
    for (u32 t = 0; t < 4; ++t) step(nullptr, server, Input{1.0f}, kDt);
    server.transform.position.x -= 0.3f;
    client.reconcile(4, server);
    // The prediction moves at once (server state + inputs 5..10 replayed); the drawn one does not.
    CHECK(client.state().transform.position.x == doctest::Approx(0.2f));
    CHECK(client.stats().pending == 6);
    CHECK(client.stats().corrections == 1);
    CHECK(client.stats().last_error == doctest::Approx(0.3f));
    CHECK(client.displayed().position.x == doctest::Approx(drawn_before));

    // Each tick removes rate * dt of what is left: no jump, and gone after a while, exactly.
    f32 prev = client.displayed().position.x;
    for (u32 t = 0; t < 120; ++t) {
        client.smooth(kDt);
        const f32 x = client.displayed().position.x;
        CHECK(x <= prev);
        CHECK(prev - x < 0.06f);
        prev = x;
    }
    CHECK(client.stats().offset == 0.0f);
    CHECK(same_bits(client.displayed().position.x, client.state().transform.position.x));

    // A stale ack (older than one already reconciled) changes nothing.
    client.reconcile(2, State{});
    CHECK(client.state().transform.position.x == doctest::Approx(0.2f));
}

TEST_CASE("net prediction: an error beyond the snap distance snaps") {
    Predictor<Input, State> client;
    PredictionDesc d;
    d.snap_distance = 1.0f;
    client.init(d, step);
    client.reset(State{});
    client.push(Input{1.0f});
    State server;
    server.transform.position.x = 5.0f;  // teleported
    client.reconcile(1, server);
    CHECK(client.stats().snaps == 1);
    CHECK(same_bits(client.displayed().position.x, 5.0f));
}

TEST_CASE("net prediction: the replication client leaves a predicted entity to the game") {
    World server_world;
    ReplicationServer rs;
    LocalTransform at;
    at.position = {1.0f, 0.0f, 0.0f};
    const Entity mover = server_world.spawn_with(at);
    rs.assign(server_world, mover, 1);
    const Entity other = server_world.spawn_with(at);
    rs.assign(server_world, other, 1);

    World world;
    ReplicationClient rc;
    rc.set_interpolation(InterpolationDesc{});
    rc.set_predicted(1);
    CHECK(rc.is_predicted(1));
    auto send = [&](u32 tick, f32 x) {
        server_world.try_get_mut<LocalTransform>(mover)->position.x = x;
        server_world.try_get_mut<LocalTransform>(other)->position.x = x;
        ByteWriter w;
        rs.write_snapshot(server_world, tick, w);
        rc.apply(world.commands(), world, w.data());
        world.sync(Phase::FixedUpdate_Pre);
    };
    send(1, 1.0f);
    // The game draws entity 1 where its prediction is.
    LocalTransform predicted;
    predicted.position = {7.0f, 0.0f, 0.0f};
    world.commands().set(rc.find(1), predicted);
    world.sync(Phase::FixedUpdate_Pre);
    for (u32 t = 2; t <= 20; ++t) {
        const InterpolationStats s = rc.update(world.commands(), world, 1.0 / 60.0);
        world.sync(Phase::FixedUpdate_Pre);
        CHECK(s.predicted == 1);
        send(t, static_cast<f32>(t));
    }
    CHECK(world.try_get<LocalTransform>(rc.find(1))->position.x == 7.0f);  // untouched
    REQUIRE(rc.latest_transform(1));
    CHECK(rc.latest_transform(1)->position.x == 20.0f);                    // the server's word, for reconcile
    CHECK(world.try_get<LocalTransform>(rc.find(2))->position.x != 7.0f);  // the other one interpolates

    // Back to snapshots: it is interpolated again.
    rc.set_predicted(1, false);
    for (u32 t = 21; t <= 60; ++t) {
        rc.update(world.commands(), world, 1.0 / 60.0);
        world.sync(Phase::FixedUpdate_Pre);
        send(t, 20.0f);
    }
    CHECK(world.try_get<LocalTransform>(rc.find(1))->position.x == 20.0f);
}
