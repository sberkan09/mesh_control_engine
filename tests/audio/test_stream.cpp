// FR-AUD-040: streamed music loops gaplessly from the page ring; a late filler costs silence,
// never the timeline.

#include <cmath>

#include "audio_test_util.h"

using namespace mce;
using namespace mce::audio;

namespace {

// Stereo clip whose every sample is unique and exactly representable: L = i / 2^17, R = -L.
Array<f32> ramp_clip(u32 frames) {
    Array<f32> a;
    a.resize(static_cast<usize>(frames) * 2);
    for (u32 i = 0; i < frames; ++i) {
        a[i * 2] = static_cast<f32>(i) / 131072.f;
        a[i * 2 + 1] = -static_cast<f32>(i) / 131072.f;
    }
    return a;
}

u64 expected_frame(u64 t, u32 count, u32 loop_start) {
    return t < count ? t : loop_start + (t - count) % (count - loop_start);
}

}  // namespace

TEST_CASE("a looping stream with an intro plays gapless and bit-exact for 60 s") {
    constexpr u32 kFrames = 70'001;  // not a multiple of the page size: the loop seam moves around
    constexpr u32 kLoopStart = 12'345;
    auto dev = test::offline_device();
    Array<f32> clip = ramp_clip(kFrames);
    auto s = dev->register_stream(test::payload(clip, 2, kLoopStart));
    REQUIRE(s.has_value());
    PlayParams p;
    p.loop = true;
    p.priority = 0;
    p.bus = Bus_Music;
    REQUIRE_FALSE(dev->play(s.value(), p).is_null());

    constexpr u32 kTotal = 48000 * 60;
    Array<f32> block;
    block.resize(480 * 2);
    u64 mismatches = 0;
    for (u32 done = 0; done < kTotal; done += 480) {
        dev->render_offline(block.data(), 480);
        dev->update();  // the filler: once per "frame"
        for (u32 i = 0; i < 480; ++i) {
            const u64 src = expected_frame(done + i, kFrames, kLoopStart);
            if (block[i * 2] != clip[src * 2] || block[i * 2 + 1] != clip[src * 2 + 1]) ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK(dev->stats().underruns == 0);
}

TEST_CASE("a non-looping stream ends and frees its ring for the next play") {
    auto dev = test::offline_device();
    Array<f32> clip = ramp_clip(10'000);
    auto s = dev->register_stream(test::payload(clip, 2)).value();
    const AudioHandle v = dev->play(s);
    CHECK(dev->play(s).is_null());  // one voice per stream
    Array<f32> out;
    test::render(*dev, out, 12'000);
    CHECK_FALSE(dev->is_playing(v));
    CHECK(out[9'999 * 2] == clip[9'999 * 2]);
    CHECK(out[10'000 * 2] == 0.f);

    const AudioHandle again = dev->play(s);  // restarts from the top
    REQUIRE_FALSE(again.is_null());
    test::render(*dev, out, 480);
    CHECK(out[100 * 2] == clip[100 * 2]);
}

TEST_CASE("underrun: a stalled filler mixes silence, counts it, and keeps the timeline") {
    auto dev = test::offline_device();
    Array<f32> clip = ramp_clip(48000 * 4);
    auto s = dev->register_stream(test::payload(clip, 2)).value();
    PlayParams p;
    const AudioHandle v = dev->play(s, p);
    Array<f32> out;
    // No update() for 600 ms: the 400 ms primed ring runs dry.
    test::render(*dev, out, 28'800, 480, /*update=*/false);
    CHECK(dev->stats().underruns > 0);
    CHECK(out[1000 * 2] == clip[1000 * 2]);         // primed audio played
    CHECK(out[28'000 * 2] == 0.f);                  // then silence
    CHECK(dev->is_playing(v));                      // voice kept

    // Filler catches up; audio resumes at the right timeline position, not where it stalled.
    dev->update();
    Array<f32> more;
    test::render(*dev, more, 4800);
    const u32 t = 28'800 + 4000;
    CHECK(more[4000 * 2] == clip[t * 2]);
}

TEST_CASE("stream slots are limited and recycled on release") {
    auto dev = test::offline_device();
    Array<f32> clip = ramp_clip(1000);
    ClipHandle hs[kMaxStreams];
    for (u32 i = 0; i < kMaxStreams; ++i) hs[i] = dev->register_stream(test::payload(clip, 2)).value();
    CHECK(dev->register_stream(test::payload(clip, 2)).error().code == Errc::OutOfMemory);
    dev->release_clip(hs[0]);
    Array<f32> out;
    test::render(*dev, out, 480);
    CHECK(dev->register_stream(test::payload(clip, 2)).has_value());
}
