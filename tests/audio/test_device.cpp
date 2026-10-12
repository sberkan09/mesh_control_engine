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

TEST_CASE("Device backend with silent_on_failure opens in the background and mixes meanwhile") {
    Array<f32> clip = test::dc_clip(48000, 0.1f);  // outlives the device: the voice still plays at the end
    AudioDesc d;
    d.backend = Backend::Device;
    d.driver = "dummy";
    auto dev = AudioDevice::create(d).value();
    CHECK(dev->backend() == Backend::Silent);  // create() never waits on the device

    auto h = dev->register_clip(test::payload(clip)).value();
    const AudioHandle v = dev->play(h);
    REQUIRE_FALSE(v.is_null());
    u64 clock = dev->mix_clock();
    for (u32 i = 0; i < 400 && dev->backend() != Backend::Device; ++i) {
        dev->update();  // Silent until the open lands: the clock keeps moving, one block per call
        if (dev->backend() == Backend::Silent) CHECK(dev->mix_clock() > clock);
        clock = dev->mix_clock();
        platform::sleep_ms(5);
    }
    REQUIRE(dev->backend() == Backend::Device);
    // The device thread took over the mixer: the same voice keeps playing past where Silent left it.
    for (u32 i = 0; i < 400 && dev->mix_clock() < clock + 4800; ++i) {
        platform::sleep_ms(5);
        dev->update();
    }
    CHECK(dev->mix_clock() >= clock + 4800);
    CHECK(dev->is_playing(v));
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

TEST_CASE("set_paused freezes the mix on the device and on Silent; resuming picks up where it stopped") {
    Array<f32> clip = test::dc_clip(480000, 0.1f);
    for (const Backend b : {Backend::Device, Backend::Silent}) {
        CAPTURE(static_cast<int>(b));
        AudioDesc d;
        d.backend = b;
        d.driver = "dummy";
        d.silent_on_failure = false;
        auto dev = AudioDevice::create(d).value();
        auto h = dev->register_clip(test::payload(clip)).value();
        const AudioHandle v = dev->play(h);
        for (u32 i = 0; i < 400 && dev->mix_clock() < 4800; ++i) {
            platform::sleep_ms(5);
            dev->update();
        }
        REQUIRE(dev->mix_clock() >= 4800);
        dev->set_paused(true);
        CHECK(dev->paused());
        platform::sleep_ms(30);  // a callback already in flight may still land
        const u64 frozen = dev->mix_clock();
        for (u32 i = 0; i < 20; ++i) {
            platform::sleep_ms(5);
            dev->update();
        }
        CHECK(dev->mix_clock() == frozen);
        CHECK(dev->is_playing(v));
        dev->set_paused(false);
        for (u32 i = 0; i < 400 && dev->mix_clock() < frozen + 4800; ++i) {
            platform::sleep_ms(5);
            dev->update();
        }
        CHECK(dev->mix_clock() >= frozen + 4800);
    }
}

TEST_CASE("audio_bridge: AudioSource play_on_start, live edits, PlaySfx, and removal") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 0.1f);
    const ClipHandle h = dev->register_clip(test::payload(clip)).value();

    World w;
    install(w, *dev);
    AudioSource src;
    src.clip_handle = h;
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
            ev.clip_handle = g_sfx_clip;
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

TEST_CASE("AudioDevice: finish_releases drains more releases than one callback takes (#145)") {
    auto dev = test::offline_device(1024);
    const Array<f32> pcm = test::dc_clip(64, 0.1f);
    Array<ClipHandle> clips;
    for (u32 i = 0; i < 300; ++i) {
        auto c = dev->register_clip(test::payload(pcm));
        REQUIRE(c);
        clips.push_back(c.value());
    }
    Array<f32> out;
    test::render(*dev, out, 480);
    for (ClipHandle c : clips) dev->release_clip(c);
    CHECK(dev->finish_releases());
    for (ClipHandle c : clips) CHECK_FALSE(dev->is_clip_registered(c));
    CHECK(dev->stats().mix_sample == 480);  // drained without mixing
}
