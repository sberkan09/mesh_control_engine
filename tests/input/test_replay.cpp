// FR-RUN-070: the recorded post-quantization stream replays byte-identically.

#include <doctest/doctest.h>

#include <cstring>

#include "input_test_util.h"
#include "mce/input/replay.h"

using namespace mce;
using namespace mce::input;
using platform::Key;

TEST_CASE("60 s of recorded input replays identically through sample_for_tick") {
    InputRecorder live;
    {
        InputSystem in;
        test::setup_default(in);
        in.set_recorder(&live);
        const Key keys[] = {Key::A, Key::D, Key::Space, Key::J};
        for (u64 t = 1; t <= 3600; ++t) {
            const u64 at = in.tick_close_us(t - 1) + 500;
            if (t % 7 == 0) in.push_event(test::key_event(keys[(t / 7) % 4], (t / 7) % 2 == 0, at));
            if (t % 11 == 0) in.push_event(test::mouse_move(static_cast<f32>(t % 640), static_cast<f32>(t % 480), at));
            if (t % 97 == 0) in.push_event(test::simple_event(platform::EventType::TextInput, at, static_cast<u32>('a' + (t % 26))));
            in.sample_for_tick(t);
        }
    }
    REQUIRE(live.ticks() == 3600);
    MESSAGE("replay stream: " << live.bytes().size() / 3600 << " B/tick");

    // Replay into a fresh system with a different map loaded and live events arriving: the
    // sim must not be able to tell.
    InputRecorder replayed;
    {
        InputSystem in;
        test::setup_default(in);
        InputReplayer player(live.bytes());
        in.set_replayer(&player);
        in.set_recorder(&replayed);
        for (u64 t = 1; t <= 3600; ++t) {
            in.push_event(test::key_event(Key::W, t % 2 == 0, in.tick_close_us(t - 1) + 10));
            in.sample_for_tick(t);
        }
        CHECK(player.done());
    }
    REQUIRE(replayed.bytes().size() == live.bytes().size());
    CHECK(std::memcmp(replayed.bytes().data(), live.bytes().data(), live.bytes().size()) == 0);
}

TEST_CASE("replayer rejects corrupt streams") {
    InputRecorder rec;
    InputState s;
    s.tick = 1;
    rec.record(s);
    Array<byte> bytes;
    bytes.resize(rec.bytes().size());
    std::memcpy(bytes.data(), rec.bytes().data(), bytes.size());
    bytes[0] = byte{0};
    InputReplayer bad(bytes.span());
    InputState out;
    CHECK_FALSE(bad.next(out));

    InputReplayer truncated(Span<const byte>(rec.bytes().data(), rec.bytes().size() - 1));
    CHECK_FALSE(truncated.next(out));
}
