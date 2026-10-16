// M-AUDIO2 4: sidechain ducking of the stock buses, the per-bus trim stage, and mixer snapshots
// (AudioSnapshot) blended over a fade by gameplay or the pause, outside the hashed sim state.

#include <cmath>

#include "audio_test_util.h"
#include "mce/audio/components.h"

using namespace mce;
using namespace mce::audio;

namespace {

// Left channel at `frame`.
f32 at(const Array<f32>& out, u32 frame) { return out[frame * 2]; }

}  // namespace

TEST_CASE("ducking: Music drops by depth while the Voice key plays, then releases; the key is read pre-fader") {
    auto dev = test::offline_device();
    const Array<f32> music = test::dc_clip(4800, 0.4f);
    Array<f32> line = test::dc_clip(96000, 0.f);
    for (u32 i = 24000; i < 48000; ++i) line[i] = (i / 24) % 2 ? 0.5f : -0.5f;  // a 1 kHz square: zero mean, loud
    PlayParams mp;
    mp.bus = Bus_Music;
    mp.loop = true;
    dev->play(dev->register_clip(test::payload(music)).value(), mp);
    PlayParams vp;
    vp.bus = Bus_User0;
    dev->play(dev->register_clip(test::payload(line)).value(), vp);
    dev->set_bus_gain(Bus_User0, -96.f);  // the voice itself is silent: the output is the music alone
    DuckDesc d;
    d.enabled = true;
    d.key = Bus_User0;
    d.depth_db = -12.f;
    d.attack_ms = 10.f;
    d.release_ms = 100.f;
    REQUIRE(dev->set_bus_duck(Bus_Music, d));
    CHECK(dev->bus_duck(Bus_Music).enabled);

    Array<f32> out;
    test::render(*dev, out, 120000);
    const f32 base = at(out, 20000);
    CHECK(base == doctest::Approx(0.4f * std::sqrt(0.5f)).epsilon(1e-4));
    CHECK(at(out, 40000) == doctest::Approx(base * db_to_linear(-12.f)).epsilon(0.01));
    CHECK(at(out, 115000) == doctest::Approx(base).epsilon(0.01));  // released
    // The gain moves smoothly: no step bigger than the attack allows.
    f32 max_step = 0.f;
    for (u32 i = 1; i < 120000; ++i) max_step = std::fmax(max_step, std::fabs(at(out, i) - at(out, i - 1)));
    CHECK(max_step < base * 0.01f);

    // Turned off, the duck lets the bus back up instead of jumping; then it is bit-exact unducked.
    auto dev2 = test::offline_device();
    dev2->play(dev2->register_clip(test::payload(music)).value(), mp);
    Array<f32> loud = test::dc_clip(48000, 0.5f);
    dev2->play(dev2->register_clip(test::payload(loud)).value(), vp);
    REQUIRE(dev2->set_bus_duck(Bus_Music, d));
    dev2->set_bus_gain(Bus_User0, -96.f);
    Array<f32> a, b;
    test::render(*dev2, a, 9600);
    CHECK(at(a, 9000) == doctest::Approx(base * db_to_linear(-12.f)).epsilon(0.01));
    d.enabled = false;
    REQUIRE(dev2->set_bus_duck(Bus_Music, d));
    test::render(*dev2, b, 48000);
    CHECK(at(b, 1) < base * 0.3f);
    CHECK(at(b, 47000) == base);

    // Bad routes are refused.
    d.enabled = true;
    CHECK_FALSE(dev->set_bus_duck(Bus_Master, d));
    CHECK_FALSE(dev->set_bus_duck(Bus_User0, d));
    d.key = Bus_Music;
    CHECK_FALSE(dev->set_bus_duck(Bus_Music, d));
    d.key = Bus_Master;
    CHECK_FALSE(dev->set_bus_duck(Bus_SFX, d));
}

TEST_CASE("bus trim: a ramped gain after the bus gain, on any bus and Master") {
    auto dev = test::offline_device();
    const Array<f32> clip = test::dc_clip(48000, 0.5f);
    dev->play(dev->register_clip(test::payload(clip)).value());
    dev->set_bus_trim(Bus_SFX, -6.f);
    dev->set_bus_trim(Bus_Master, -6.f);
    CHECK(dev->bus_trim_db(Bus_SFX) == -6.f);
    Array<f32> out;
    test::render(*dev, out, 4800);
    CHECK(at(out, 4000) == doctest::Approx(0.5f * std::sqrt(0.5f) * db_to_linear(-12.f)).epsilon(1e-4));
    CHECK(at(out, 100) > at(out, 4000));  // ramped, not stepped
    const u64 h = dev->command_hash();
    dev->set_bus_trim(Bus_SFX, -6.f);
    CHECK(dev->command_hash() == h);
}

TEST_CASE("AudioSnapshot: fades in and out, blends gain and filters, and the pause one leaves the sim hash alone") {
    auto dev = test::offline_device();
    World w;
    install(w, *dev);
    AudioSnapshot pause;
    pause.bus = AudioBus::Master;
    pause.while_paused = true;
    pause.fade_s = 0.5f;
    pause.gain_db = -6.f;
    pause.lowpass_hz = 500.f;
    w.spawn_with(pause);
    AudioSnapshot cave;
    cave.bus = AudioBus::SFX;
    cave.fade_s = 0.f;
    cave.highpass_hz = 200.f;
    const Entity c = w.spawn_with(cave);
    AudioBusEffects base;
    base.bus = AudioBus::Music;
    base.lowpass_hz = 4000.f;
    w.spawn_with(base);
    AudioSnapshot muffle;
    muffle.bus = AudioBus::Music;
    muffle.active = true;
    muffle.fade_s = 1.f;
    muffle.lowpass_hz = 1000.f;
    w.spawn_with(muffle);

    const u64 before = w.state_hash();
    update_mix(w, 0.25f, true);  // the host, a quarter second into the pause
    CHECK(w.state_hash() == before);
    CHECK(dev->bus_trim_db(Bus_Master) == doctest::Approx(-3.f));
    // Halfway in octaves between open (20 kHz) and 500 Hz.
    CHECK(dev->bus_filter(Bus_Master).lowpass_hz == doctest::Approx(std::sqrt(20000.f * 500.f)).epsilon(1e-4));
    // A quarter of the way from the bus's own 4 kHz to the snapshot's 1 kHz (two octaves).
    CHECK(dev->bus_filter(Bus_Music).lowpass_hz == doctest::Approx(4000.f * std::pow(0.25f, 0.25f)).epsilon(1e-4));
    CHECK(dev->bus_filter(Bus_SFX).highpass_hz == 0.f);  // the cave is not active

    update_mix(w, 0.5f, true);
    CHECK(dev->bus_trim_db(Bus_Master) == doctest::Approx(-6.f));
    CHECK(dev->bus_filter(Bus_Master).lowpass_hz == doctest::Approx(500.f));

    // Resumed: the bridge's ticks fade the pause snapshot out; gameplay turns the cave on at once.
    w.try_get_mut<AudioSnapshot>(c)->active = true;
    const f64 dt = 0.25;
    w.tick(dt, 1);
    CHECK(dev->bus_trim_db(Bus_Master) == doctest::Approx(-3.f));
    CHECK(dev->bus_filter(Bus_SFX).highpass_hz == doctest::Approx(200.f));
    w.tick(dt, 2);
    CHECK(dev->bus_trim_db(Bus_Master) == 0.f);
    CHECK(dev->bus_filter(Bus_Master).lowpass_hz == 0.f);  // back to off, not to "20 kHz"
    w.tick(dt, 3);
    w.tick(dt, 4);
    CHECK(dev->bus_filter(Bus_Music).lowpass_hz == doctest::Approx(1000.f));

    // Removing a snapshot gives the bus back.
    w.destroy(c);
    w.tick(dt, 5);
    CHECK(dev->bus_filter(Bus_SFX).highpass_hz == 0.f);
}

TEST_CASE("audio_bridge: AudioDuck drives the ducker; removing it switches it off") {
    auto dev = test::offline_device();
    World w;
    install(w, *dev);
    AudioDuck duck;
    duck.depth_db = -18.f;
    const Entity e = w.spawn_with(duck);
    AudioDuck bad;
    bad.bus = AudioBus::Voice;  // only Music, SFX and UI duck: ignored
    w.spawn_with(bad);
    w.tick(1.0 / 60.0, 1);
    CHECK(dev->bus_duck(Bus_Music).enabled);
    CHECK(dev->bus_duck(Bus_Music).key == Bus_User0);
    CHECK(dev->bus_duck(Bus_Music).depth_db == -18.f);
    w.destroy(e);
    w.tick(1.0 / 60.0, 2);
    CHECK_FALSE(dev->bus_duck(Bus_Music).enabled);
}
