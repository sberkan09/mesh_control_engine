// M-AUDIO2 5: a low-pass per voice, AudioOcclusion (a ray the host provides: gain and low-pass
// fading over 100 ms) and AudioDoppler (pitch from both movements along the line between them).

#include <cmath>

#include "audio_test_util.h"
#include "mce/audio/components.h"

using namespace mce;
using namespace mce::audio;

namespace {

Array<f32> sine(u32 frames, f64 hz, f32 amp) {
    Array<f32> a;
    a.resize(frames);
    for (u32 i = 0; i < frames; ++i) a[i] = amp * static_cast<f32>(std::sin(2.0 * 3.14159265358979 * hz * i / kSampleRate));
    return a;
}

f32 rms(const Array<f32>& out, u32 from, u32 to) {
    f64 sum = 0.0;
    for (u32 i = from; i < to; ++i) sum += static_cast<f64>(out[i * 2]) * out[i * 2] + static_cast<f64>(out[i * 2 + 1]) * out[i * 2 + 1];
    return static_cast<f32>(std::sqrt(sum / (2 * (to - from))));
}

bool g_blocked = false;
u32 g_rays = 0;
bool fake_ray(World&, Entity, Entity, math::vec3, math::vec3, u16) {
    ++g_rays;
    return g_blocked;
}

}  // namespace

TEST_CASE("voice low-pass: cuts one voice's highs, leaves its bus alone, and off is bit-exact again") {
    const Array<f32> high = sine(48000, 8000.0, 0.5f);
    auto dev = test::offline_device();
    auto ref = test::offline_device();
    const ClipHandle a = dev->register_clip(test::payload(high)).value();
    const ClipHandle b = ref->register_clip(test::payload(high)).value();
    const AudioHandle h = dev->play(a);
    ref->play(b);
    dev->set_handle_lowpass(h, 500.f);
    Array<f32> out, dry;
    test::render(*dev, out, 24000);
    test::render(*ref, dry, 24000);
    CHECK(rms(out, 12000, 24000) < rms(dry, 12000, 24000) * db_to_linear(-40.f));
    const u64 hash = dev->command_hash();
    dev->set_handle_lowpass(h, 500.f);  // unchanged: nothing sent
    CHECK(dev->command_hash() == hash);
    dev->set_handle_lowpass(h, 0.f);
    test::render(*dev, out, 12000);
    test::render(*ref, dry, 12000);
    for (u32 i = 9000; i < 12000; ++i) {
        CHECK(out[i * 2] == dry[i * 2]);
        if (out[i * 2] != dry[i * 2]) break;
    }
}

TEST_CASE("AudioOcclusion: a blocked ray fades the source down and muffles it over 100 ms, and back") {
    auto dev = test::offline_device();
    AttenuationCurve curve;
    curve.min_distance = 100.f;  // no distance falloff in this test
    curve.max_distance = 200.f;
    REQUIRE(dev->set_attenuation(1, curve));
    const Array<f32> tone = sine(48000, 6000.0, 0.5f);
    World w;
    install(w, *dev);
    w.register_components<LocalTransform>();
    w.resource<AudioRef>().occlusion = &fake_ray;
    w.spawn_with(AudioListener{}, LocalTransform{});
    AudioSource src;
    src.clip_handle = dev->register_clip(test::payload(tone)).value();
    src.loop = true;
    src.play_on_start = true;
    LocalTransform at;
    at.position = {0.f, 0.f, -5.f};
    const Entity e = w.spawn_with(src, AudioSpatial{}, AudioOcclusion{}, at);
    w.try_get_mut<AudioSpatial>(e)->attenuation_id = 1;

    const f64 dt = 1.0 / 60.0;
    g_blocked = false;
    w.tick(dt, 1);
    Array<f32> out;
    test::render(*dev, out, 4800);
    const f32 clear = rms(out, 2400, 4800);
    CHECK(clear > 0.1f);
    CHECK(g_rays == 1);

    g_blocked = true;
    w.tick(dt, 2);
    CHECK(w.try_get<AudioOcclusion>(e)->amount == doctest::Approx(1.f / 6.f).epsilon(1e-4));
    for (u64 t = 3; t < 10; ++t) w.tick(dt, t);
    CHECK(w.try_get<AudioOcclusion>(e)->amount == 1.f);
    test::render(*dev, out, 9600);
    // -12 dB from the gain and most of the rest from the 800 Hz low-pass on a 6 kHz tone.
    CHECK(rms(out, 4800, 9600) < clear * db_to_linear(-30.f));
    const u64 h = dev->command_hash();
    w.tick(dt, 10);
    CHECK(dev->command_hash() == h);  // settled: nothing sent

    g_blocked = false;
    for (u64 t = 11; t < 20; ++t) w.tick(dt, t);
    CHECK(w.try_get<AudioOcclusion>(e)->amount == 0.f);
    test::render(*dev, out, 9600);
    CHECK(rms(out, 4800, 9600) == doctest::Approx(clear).epsilon(0.01));
}

TEST_CASE("AudioDoppler: closing raises the pitch, receding lowers it, from either side's movement") {
    auto dev = test::offline_device();
    const Array<f32> tone = test::dc_clip(48000, 0.1f);
    World w;
    install(w, *dev);
    w.register_components<LocalTransform>();
    const Entity ear = w.spawn_with(AudioListener{}, LocalTransform{});
    AudioSource src;
    src.clip_handle = dev->register_clip(test::payload(tone)).value();
    src.loop = true;
    src.play_on_start = true;
    LocalTransform at;
    at.position = {0.f, 0.f, -100.f};
    const Entity e = w.spawn_with(src, AudioSpatial{}, AudioDoppler{}, at);

    const f64 dt = 1.0 / 60.0;
    const f32 speed = 34.3f;  // a tenth of the speed of sound
    w.tick(dt, 1);
    CHECK(w.try_get<AudioDoppler>(e)->applied == 1.f);
    u64 t = 2;
    const auto move = [&](Entity who, f32 dz) {
        w.try_get_mut<LocalTransform>(who)->position.z += dz;
        w.tick(dt, t++);
    };
    move(e, speed * static_cast<f32>(dt));  // toward the listener
    CHECK(w.try_get<AudioDoppler>(e)->applied == doctest::Approx(343.f / (343.f - speed)).epsilon(1e-3));
    move(e, -speed * static_cast<f32>(dt));  // away
    CHECK(w.try_get<AudioDoppler>(e)->applied == doctest::Approx(343.f / (343.f + speed)).epsilon(1e-3));
    move(ear, -speed * static_cast<f32>(dt));  // the listener walks toward it
    CHECK(w.try_get<AudioDoppler>(e)->applied == doctest::Approx((343.f + speed) / 343.f).epsilon(1e-3));
    w.tick(dt, t++);  // nobody moves
    CHECK(w.try_get<AudioDoppler>(e)->applied == 1.f);
    // Factor 0 turns it off.
    w.try_get_mut<AudioDoppler>(e)->factor = 0.f;
    move(e, speed * static_cast<f32>(dt));
    CHECK(w.try_get<AudioDoppler>(e)->applied == 1.f);
}
