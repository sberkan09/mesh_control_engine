// The real device path (SDL "dummy" driver: no sound card needed) and the ECS bridge.

#include <cmath>

#include "audio_test_util.h"
#include "mce/audio/components.h"
#include "mce/platform/platform.h"

using namespace mce;
using namespace mce::audio;

namespace {
ClipHandle g_sfx_clip;
}

TEST_CASE("Device backend pulls from the mixer on the platform audio thread") {
    AudioDesc d;
    d.backend = Backend::Device;
    d.driver = "dummy";
    d.silent_on_failure = false;
    auto r = AudioDevice::create(d);
    REQUIRE(r.has_value());
    auto dev = std::move(r).value();
    CHECK(dev->backend() == Backend::Device);

    Array<f32> clip = test::dc_clip(4800, 0.1f);
    auto h = dev->register_clip(test::payload(clip)).value();
    const AudioHandle v = dev->play(h);
    REQUIRE_FALSE(v.is_null());
    // Wait (bounded) for the device to mix past the clip and report the voice finished.
    for (u32 i = 0; i < 400 && (dev->is_playing(v) || dev->stats().mix_sample < 4800); ++i) {
        platform::sleep_ms(5);
        dev->update();
    }
    CHECK(dev->stats().mix_sample >= 4800);
    CHECK_FALSE(dev->is_playing(v));
}

TEST_CASE("Silent backend: no device, commands still flow and voices still finish") {
    AudioDesc d;
    d.backend = Backend::Silent;
    auto dev = AudioDevice::create(d).value();
    Array<f32> clip = test::dc_clip(2000, 0.1f);
    auto h = dev->register_clip(test::payload(clip)).value();
    const AudioHandle v = dev->play(h);
    for (u32 i = 0; i < 10; ++i) dev->update();
    CHECK_FALSE(dev->is_playing(v));
}

TEST_CASE("audio_bridge: AudioSource play_on_start, live edits, PlaySfx, and removal") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 0.1f);
    const ClipHandle h = dev->register_clip(test::payload(clip)).value();

    World w;
    install(w, *dev);
    AudioSource src;
    src.clip = h;
    src.loop = true;
    src.play_on_start = true;
    const Entity e = w.spawn_with(src);
    const Entity quiet = w.spawn_with(AudioSource{});  // play_on_start = false: stays silent

    const f64 dt = 1.0 / 60.0;
    w.tick(dt, 1);
    const AudioSource* s = w.try_get<AudioSource>(e);
    REQUIRE(s != nullptr);
    CHECK(s->started);
    CHECK(dev->is_playing(s->handle));
    CHECK(w.try_get<AudioSource>(quiet)->handle.is_null());

    // A gameplay edit is forwarded once, then nothing more is sent.
    const u64 before = dev->command_hash();
    w.try_get_mut<AudioSource>(e)->gain_db = -6.f;
    w.tick(dt, 2);
    const u64 after_edit = dev->command_hash();
    CHECK(after_edit != before);
    w.tick(dt, 3);
    CHECK(dev->command_hash() == after_edit);

    // One-shot via event.
    g_sfx_clip = h;
    w.add_system(Phase::FixedUpdate_Pre, "shoot", [](Frame& f) {
        if (f.tick == 4) {
            PlaySfx ev;
            ev.clip = g_sfx_clip;
            f.events().push(ev);
        }
    });
    w.tick(dt, 4);
    Array<f32> out;
    test::render(*dev, out, 480);
    CHECK(dev->stats().voices_real == 2);

    // Destroying the entity stops its looping voice (50 ms fade).
    const AudioHandle looping = s->handle;
    w.destroy(e);
    test::render(*dev, out, 4800);
    CHECK_FALSE(dev->is_playing(looping));
}
