// Mixer behavior through the Offline backend: the same callback body the device runs, pumped
// with a fake clock (audio-input-ui §6.2).

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "audio_test_util.h"
#include "test_support.h"

using namespace mce;
using namespace mce::audio;

TEST_CASE("a one-shot plays from the next block and its handle ends with the clip") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(1000, 0.5f);
    auto h = dev->register_clip(test::payload(clip));
    REQUIRE(h.has_value());

    PlayParams p;
    p.pan = -1.f;  // hard left: mono constant-power → L = 1, R = 0
    const AudioHandle v = dev->play(h.value(), p);
    REQUIRE_FALSE(v.is_null());
    CHECK(dev->is_playing(v));

    Array<f32> out;
    test::render(*dev, out, 1200, 400);
    CHECK(out[0] == doctest::Approx(0.5f));
    CHECK(out[1] == doctest::Approx(0.f).epsilon(1e-6));
    CHECK(out[999 * 2] == doctest::Approx(0.5f));
    CHECK(out[1000 * 2] == 0.f);  // ended exactly at the clip's last frame
    CHECK_FALSE(dev->is_playing(v));
    CHECK(dev->live_handles() == 0);
    CHECK(dev->stats().mix_sample == 1200);
}

TEST_CASE("pan law: mono is constant power, stereo is balance") {
    auto dev = test::offline_device();
    Array<f32> mono = test::dc_clip(4800, 1.f);
    Array<f32> stereo = test::dc_clip(4800 * 2, 0.25f);
    auto hm = dev->register_clip(test::payload(mono)).value();
    auto hs = dev->register_clip(test::payload(stereo, 2)).value();

    Array<f32> out;
    AudioHandle v = dev->play(hm);
    test::render(*dev, out, 480);
    CHECK(out[100] == doctest::Approx(std::sqrt(0.5f)));
    CHECK(out[101] == doctest::Approx(std::sqrt(0.5f)));
    dev->stop(v);
    test::render(*dev, out, 480);

    PlayParams right;
    right.pan = 0.5f;
    dev->play(hs, right);
    test::render(*dev, out, 480);
    CHECK(out[100] == doctest::Approx(0.125f));  // left attenuated by (1 - pan)
    CHECK(out[101] == doctest::Approx(0.25f));   // right untouched
}

TEST_CASE("gain changes ramp linearly: no zipper, no click") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 1.f);
    auto h = dev->register_clip(test::payload(clip, 1)).value();
    PlayParams p;
    p.pan = -1.f;
    const AudioHandle v = dev->play(h, p);
    Array<f32> out;
    test::render(*dev, out, 480);
    dev->set_handle_gain(v, -96.f);  // to silence
    test::render(*dev, out, 960, 960);
    CHECK(out[0] == doctest::Approx(1.f));
    f32 max_step = 0.f;
    for (u32 i = 1; i < 960; ++i) max_step = std::fmax(max_step, std::fabs(out[i * 2] - out[(i - 1) * 2]));
    CHECK(max_step <= 1.f / 480.f + 1e-5f);  // 10 ms ramp
    CHECK(out[479 * 2] == doctest::Approx(1.f / 480.f).epsilon(1e-3));
    CHECK(out[480 * 2] == 0.f);  // landed exactly on the target
    CHECK(dev->is_playing(v));   // gain 0 is still a live voice
}

TEST_CASE("stop fades out instead of cutting the waveform") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 1.f);
    auto h = dev->register_clip(test::payload(clip)).value();
    PlayParams p;
    p.pan = -1.f;
    const AudioHandle v = dev->play(h, p);
    Array<f32> out;
    test::render(*dev, out, 100);
    dev->stop(v);  // fade_s = 0 still ramps over 2 ms
    test::render(*dev, out, 200, 200);
    CHECK(out[0] > 0.98f);
    CHECK(out[95 * 2] < 0.02f);
    CHECK(out[96 * 2] == 0.f);
    CHECK_FALSE(dev->is_playing(v));
}

TEST_CASE("voice stealing: priority, then quieter, then older; stolen handles fail cleanly") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 0.01f);
    auto h = dev->register_clip(test::payload(clip)).value();

    AudioHandle v[kMaxRealVoices];
    for (u32 i = 0; i < kMaxRealVoices; ++i) {
        PlayParams p;
        p.loop = true;
        p.priority = i == 3 ? 200 : 100;  // voice 3 is the least important
        p.gain_db = i == 5 ? -12.f : 0.f;
        v[i] = dev->play(h, p);
    }
    Array<f32> out;
    test::render(*dev, out, 480);
    CHECK(dev->stats().voices_real == kMaxRealVoices);

    PlayParams p;
    p.priority = 100;
    const AudioHandle a = dev->play(h, p);  // steals voice 3 (priority 200)
    test::render(*dev, out, 480);
    CHECK(dev->stats().steals == 1);
    CHECK_FALSE(dev->is_playing(v[3]));
    CHECK(dev->is_playing(a));

    const AudioHandle b = dev->play(h, p);  // equal priorities: quieter voice 5 goes
    test::render(*dev, out, 480);
    CHECK_FALSE(dev->is_playing(v[5]));

    const AudioHandle c = dev->play(h, p);  // equal priority + gain: the oldest (voice 0)
    test::render(*dev, out, 480);
    CHECK_FALSE(dev->is_playing(v[0]));
    CHECK(dev->is_playing(v[1]));
    CHECK(dev->stats().steals == 3);
    CHECK(dev->is_playing(b));
    CHECK(dev->is_playing(c));

    // A stolen handle's index is reused, but the old handle never aliases the new voice.
    const AudioHandle d = dev->play(h, p);
    test::render(*dev, out, 480);
    CHECK(dev->is_playing(d));
    CHECK_FALSE(dev->is_playing(v[3]));
    dev->stop(v[3]);  // stale: silent no-op
    test::render(*dev, out, 480);
    CHECK(dev->is_playing(d));
}

TEST_CASE("less important sounds go virtual and are promoted when a voice frees") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000 * 4, 0.01f);
    auto h = dev->register_clip(test::payload(clip)).value();
    AudioHandle v[kMaxRealVoices];
    for (u32 i = 0; i < kMaxRealVoices; ++i) {
        PlayParams p;
        p.priority = 50;
        v[i] = dev->play(h, p);
    }
    PlayParams low;
    low.priority = 200;
    const AudioHandle q = dev->play(h, low);
    Array<f32> out;
    test::render(*dev, out, 480);
    CHECK(dev->stats().voices_virtual == 1);
    CHECK(dev->stats().steals == 0);
    CHECK(dev->is_playing(q));  // virtual voices are live handles

    dev->stop(v[0]);
    test::render(*dev, out, 960);
    CHECK(dev->stats().voices_virtual == 0);
    CHECK(dev->stats().voices_real == kMaxRealVoices);
    CHECK(dev->is_playing(q));
}

TEST_CASE("protected music is never stolen by sound effects") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 0.01f);
    auto h = dev->register_clip(test::payload(clip)).value();
    PlayParams music;
    music.priority = 0;
    music.loop = true;
    music.bus = Bus_Music;
    music.gain_db = -30.f;  // quietest and oldest: the first steal candidate if unprotected
    const AudioHandle m = dev->play(h, music);
    PlayParams sfx;
    sfx.priority = 40;
    for (u32 i = 0; i < kMaxRealVoices + 4; ++i) dev->play(h, sfx);
    Array<f32> out;
    test::render(*dev, out, 960);
    CHECK(dev->is_playing(m));
    CHECK(dev->stats().steals == 5);  // 7 SFX fit beside the music; the other 5 steal SFX
}

TEST_CASE("bus gains apply and ramp; the master bus scales everything") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 1.f);
    auto h = dev->register_clip(test::payload(clip)).value();
    PlayParams p;
    p.pan = -1.f;
    p.loop = true;
    dev->play(h, p);
    dev->set_bus_gain(Bus_SFX, linear_to_db(0.5f));
    dev->set_bus_gain(Bus_Master, linear_to_db(0.5f));
    Array<f32> out;
    test::render(*dev, out, 960);
    CHECK(out[900 * 2] == doctest::Approx(0.25f).epsilon(1e-4));
    CHECK(dev->bus_gain_db(Bus_SFX) == doctest::Approx(-6.0206f).epsilon(1e-3));
}

TEST_CASE("FIFO overflow: play returns null, nothing corrupts, every handle comes back") {
    auto dev = test::offline_device(16);
    Array<f32> clip = test::dc_clip(100, 0.1f);
    auto h = dev->register_clip(test::payload(clip)).value();
    Array<f32> out;
    test::render(*dev, out, 16);  // drain the RegisterClip

    u32 ok = 0, nulls = 0;
    for (u32 i = 0; i < 40; ++i) (dev->play(h).is_null() ? nulls : ok)++;
    CHECK(ok == 16);
    CHECK(nulls == 24);
    CHECK(dev->stats().commands_dropped == 24);
    test::render(*dev, out, 4800);
    CHECK(dev->live_handles() == 0);
    CHECK(dev->stats().voices_real == 0);
}

TEST_CASE("released clips stop their voices; stale clip handles are no-ops") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 0.1f);
    auto h = dev->register_clip(test::payload(clip)).value();
    PlayParams p;
    p.loop = true;
    const AudioHandle v = dev->play(h, p);
    Array<f32> out;
    test::render(*dev, out, 480);
    dev->release_clip(h);
    CHECK(dev->is_clip_registered(h));  // until the mixer acknowledges
    CHECK(dev->play(h).is_null());      // but no new voices meanwhile
    test::render(*dev, out, 480);
    CHECK_FALSE(dev->is_clip_registered(h));
    CHECK_FALSE(dev->is_playing(v));
    CHECK(dev->play(h).is_null());

    auto h2 = dev->register_clip(test::payload(clip)).value();  // reuses the slot, new generation
    CHECK(h2.index == h.index);
    CHECK_FALSE(h2 == h);
}

TEST_CASE("clip validation") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(100, 0.1f);
    ClipPayload p = test::payload(clip);
    p.sample_rate = 44100;
    CHECK(dev->register_clip(p).error().code == Errc::Unsupported);
    p = test::payload(clip);
    p.channels = 6;
    CHECK(dev->register_clip(p).error().code == Errc::Unsupported);
    p = test::payload(clip);
    p.loop_start = 100;
    CHECK(dev->register_clip(p).error().code == Errc::InvalidArgument);
}

TEST_CASE("FR-AUD-010: the callback body never touches the heap") {
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(4800, 0.1f);
    auto h = dev->register_clip(test::payload(clip)).value();
    auto s = dev->register_stream(test::payload(clip)).value();
    Array<f32> out;
    out.resize(4096 * 2);
    const usize violations = mem::heap_rt_violations();
    const usize live = mem::heap_live_allocations();
    PlayParams loop;
    loop.loop = true;
    dev->play(s, loop);
    for (u32 i = 0; i < 200; ++i) {
        dev->play(h);
        dev->render_offline(out.data(), 1 + (i * 37) % 4096);  // odd callback sizes
        dev->update();
    }
    CHECK(mem::heap_rt_violations() == violations);
    CHECK(mem::heap_live_allocations() == live);
}

TEST_CASE("SPSC under a real second thread: commands and replies stay consistent") {
    auto dev = test::offline_device(256);
    Array<f32> clip = test::dc_clip(300, 0.05f);
    auto h = dev->register_clip(test::payload(clip)).value();

    std::atomic<bool> stop{false};
    std::thread audio_thread([&] {
        f32 buf[256 * 2];
        while (!stop.load(std::memory_order_acquire)) dev->render_offline(buf, 256);
    });
    u32 played = 0;
    for (u32 i = 0; i < 20000; ++i) {
        PlayParams p;
        p.priority = static_cast<u8>(i % 250);
        p.pan = static_cast<f32>(i % 21) / 10.f - 1.f;
        const AudioHandle v = dev->play(h, p);
        if (!v.is_null()) ++played;
        if (i % 3 == 0) dev->stop(v);
        if (i % 5 == 0) dev->set_handle_gain(v, -6.f);
        dev->update();
    }
    // Let the mixer finish everything, then join.
    for (u32 spin = 0; spin < 2000 && dev->live_handles() > 0; ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        dev->update();
    }
    stop.store(true, std::memory_order_release);
    audio_thread.join();
    dev->update();
    CHECK(played > 1000);
    CHECK(dev->live_handles() == 0);
}

TEST_CASE("command hash is a deterministic function of the command sequence") {
    auto run = [](f32 gain) {
        auto dev = test::offline_device();
        Array<f32> clip = test::dc_clip(100, 0.1f);
        auto h = dev->register_clip(test::payload(clip)).value();
        PlayParams p;
        p.gain_db = gain;
        const AudioHandle v = dev->play(h, p);
        dev->set_handle_pan(v, 0.25f);
        dev->stop(v, 0.1f);
        return dev->command_hash();
    };
    CHECK(run(-3.f) == run(-3.f));
    CHECK(run(-3.f) != run(-4.f));
}
