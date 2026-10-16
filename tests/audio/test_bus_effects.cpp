// M-AUDIO2 3: bus low-pass and high-pass that glide to new cutoffs, Freeverb reverb units fed by
// post-fader bus sends, the audio.reverb option, and the scene components the bridge applies.

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

// RMS of the left channel over [from, to).
f32 rms(const Array<f32>& out, u32 from, u32 to) {
    f64 sum = 0.0;
    for (u32 i = from; i < to; ++i) sum += static_cast<f64>(out[i * 2]) * out[i * 2];
    return static_cast<f32>(std::sqrt(sum / (to - from)));
}

bool same(const Array<f32>& a, const Array<f32>& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

// Plays `clip` on the SFX bus, sets the low/high-pass, renders `frames`.
Array<f32> filtered(const Array<f32>& clip, f32 lowpass, f32 highpass, u32 frames) {
    auto dev = test::offline_device();
    BusFilterDesc d;
    d.lowpass_hz = lowpass;
    d.highpass_hz = highpass;
    dev->set_bus_filter(Bus_SFX, d);
    dev->play(dev->register_clip(test::payload(clip)).value());
    Array<f32> out;
    test::render(*dev, out, frames);
    return out;
}

// A 100-frame burst on SFX sending to reverb 0, rendered for `frames`. Returns the output.
Array<f32> burst_into_reverb(const ReverbDesc& r, f32 send_db, u32 frames, u32 block = 480) {
    auto dev = test::offline_device();
    REQUIRE(dev->set_reverb(0, r));
    REQUIRE(dev->set_bus_send(Bus_SFX, 0, send_db));
    Array<f32> clip = sine(100, 1000.0, 0.5f);
    dev->play(dev->register_clip(test::payload(clip)).value());
    Array<f32> out;
    test::render(*dev, out, frames, block);
    return out;
}

}  // namespace

TEST_CASE("bus filters: a low-pass cuts highs and keeps lows; a high-pass the reverse") {
    const u32 n = 24000;
    const Array<f32> high = sine(n, 8000.0, 0.5f), low = sine(n, 100.0, 0.5f);
    const f32 dry_high = rms(filtered(high, 0.f, 0.f, n), 12000, n);
    const f32 dry_low = rms(filtered(low, 0.f, 0.f, n), 12000, n);
    REQUIRE(dry_high > 0.2f);
    // 500 Hz is four octaves under 8 kHz: about -48 dB at 12 dB/octave (well past the glide).
    CHECK(rms(filtered(high, 500.f, 0.f, n), 12000, n) < dry_high * db_to_linear(-40.f));
    CHECK(rms(filtered(low, 500.f, 0.f, n), 12000, n) == doctest::Approx(dry_low).epsilon(0.03));
    CHECK(rms(filtered(low, 0.f, 2000.f, n), 12000, n) < dry_low * db_to_linear(-40.f));
    CHECK(rms(filtered(high, 0.f, 2000.f, n), 12000, n) == doctest::Approx(dry_high).epsilon(0.03));
}

TEST_CASE("bus filters: a cutoff glides there without a click, and off is bit-exact dry again") {
    const u32 n = 48000;
    const Array<f32> clip = sine(n, 200.0, 0.5f);
    auto dev = test::offline_device();
    dev->play(dev->register_clip(test::payload(clip)).value());
    Array<f32> a, b, c;
    test::render(*dev, a, 4800);
    BusFilterDesc d;
    d.lowpass_hz = 150.f;
    dev->set_bus_filter(Bus_SFX, d);
    CHECK(dev->bus_filter(Bus_SFX).lowpass_hz == 150.f);
    test::render(*dev, b, 9600);
    // The 200 Hz sine moves at most ~0.0093 between samples; a cutoff that rang would jump further.
    f32 max_step = 0.f;
    for (u32 i = 1; i < 9600; ++i) max_step = std::fmax(max_step, std::fabs(b[i * 2] - b[(i - 1) * 2]));
    CHECK(max_step < 0.0105f);
    CHECK(rms(b, 6000, 9600) < rms(a, 1000, 4800) * 0.8f);  // 150 Hz is under the tone: it is quieter

    dev->set_bus_filter(Bus_SFX, BusFilterDesc{});
    test::render(*dev, c, 9600);
    // Glided back open and bypassed: the same samples the clip gives with no filter at all.
    const Array<f32> ref = filtered(clip, 0.f, 0.f, 4800 + 9600 + 9600);
    for (u32 i = 6000; i < 9600; ++i) {
        CHECK(c[i * 2] == ref[(14400 + i) * 2]);
        if (c[i * 2] != ref[(14400 + i) * 2]) break;
    }
    // Out-of-range cutoffs clamp; NaN is off.
    d.lowpass_hz = 90000.f;
    d.highpass_hz = std::nanf("");
    dev->set_bus_filter(Bus_SFX, d);
    CHECK(dev->bus_filter(Bus_SFX).lowpass_hz == 20000.f);
    CHECK(dev->bus_filter(Bus_SFX).highpass_hz == 0.f);
}

TEST_CASE("reverb: a send leaves a decaying tail; no send, an off unit or audio.reverb off leave none") {
    ReverbDesc r;
    r.enabled = true;
    r.room_size = 0.8f;
    const u32 n = 96000;
    const Array<f32> wet = burst_into_reverb(r, 0.f, n);
    const f32 early = rms(wet, 4800, 24000), late = rms(wet, 72000, 96000);
    CHECK(early > 1e-3f);
    CHECK(late > 0.f);
    CHECK(late < early * 0.5f);
    // The same render twice, and the same whatever the callback size: fixed order, per-sample state.
    CHECK(same(wet, burst_into_reverb(r, 0.f, n)));
    CHECK(same(wet, burst_into_reverb(r, 0.f, n, 128)));

    CHECK(rms(burst_into_reverb(r, -96.f, n), 4800, n) == 0.f);
    ReverbDesc off = r;
    off.enabled = false;
    CHECK(rms(burst_into_reverb(off, 0.f, n), 4800, n) == 0.f);
    // A quieter send is a quieter tail (6 dB down, within the float noise).
    CHECK(rms(burst_into_reverb(r, -6.f, n), 4800, 24000) == doctest::Approx(early * db_to_linear(-6.f)).epsilon(1e-3));
    // Width 0: a mono return.
    ReverbDesc mono = r;
    mono.width = 0.f;
    const Array<f32> m = burst_into_reverb(mono, 0.f, 24000);
    for (u32 i = 4800; i < 24000; i += 101) CHECK(m[i * 2] == doctest::Approx(m[i * 2 + 1]).epsilon(1e-6));

    auto dev = test::offline_device();
    CHECK_FALSE(dev->set_bus_send(Bus_Master, 0, 0.f));  // Master cannot send
    CHECK_FALSE(dev->set_bus_send(Bus_SFX, kReverbUnits, 0.f));
    CHECK_FALSE(dev->set_reverb(kReverbUnits, r));
    REQUIRE(dev->set_reverb(0, r));
    REQUIRE(dev->set_bus_send(Bus_SFX, 0, 0.f));
    dev->set_reverb_allowed(false);  // what audio.reverb = false does
    CHECK(dev->reverb(0).enabled);   // the request is kept for when it is allowed again
    Array<f32> clip = sine(100, 1000.0, 0.5f);
    dev->play(dev->register_clip(test::payload(clip)).value());
    Array<f32> out;
    test::render(*dev, out, 24000);
    CHECK(rms(out, 4800, 24000) == 0.f);
    dev->set_reverb_allowed(true);
    dev->play(dev->register_clip(test::payload(clip)).value());
    test::render(*dev, out, 24000);
    CHECK(rms(out, 4800, 24000) > 1e-3f);
}

TEST_CASE("bus effects send only changes; the command hash moves only when they do") {
    auto dev = test::offline_device();
    const u64 h0 = dev->command_hash();
    dev->set_bus_filter(Bus_SFX, BusFilterDesc{});
    REQUIRE(dev->set_bus_send(Bus_SFX, 0, -96.f));
    REQUIRE(dev->set_reverb(0, ReverbDesc{}));
    dev->set_limiter(LimiterDesc{});
    CHECK(dev->command_hash() == h0);  // all defaults: nothing sent
    BusFilterDesc d;
    d.lowpass_hz = 1000.f;
    dev->set_bus_filter(Bus_SFX, d);
    const u64 h1 = dev->command_hash();
    CHECK(h1 != h0);
    dev->set_bus_filter(Bus_SFX, d);
    CHECK(dev->command_hash() == h1);
}

TEST_CASE("audio_bridge: AudioBusEffects, AudioReverb and AudioLimiter drive the mixer; removing them resets it") {
    auto dev = test::offline_device();
    World w;
    install(w, *dev);
    AudioBusEffects fx;
    fx.bus = AudioBus::Music;
    fx.lowpass_hz = 800.f;
    fx.reverb_unit = 1;
    fx.reverb_send_db = -6.f;
    const Entity a = w.spawn_with(fx);
    AudioReverb rv;
    rv.unit = 1;
    rv.room_size = 0.9f;
    const Entity b = w.spawn_with(rv);
    AudioLimiter lim;
    lim.ceiling_db = -3.f;
    const Entity c = w.spawn_with(lim);
    AudioBusEffects other = fx;
    other.lowpass_hz = 300.f;
    w.spawn_with(other);  // a second entity for the same bus: the first wins

    const f64 dt = 1.0 / 60.0;
    w.tick(dt, 1);
    CHECK(dev->bus_filter(Bus_Music).lowpass_hz == 800.f);
    CHECK(dev->bus_send_db(Bus_Music, 1) == -6.f);
    CHECK(dev->bus_send_db(Bus_Music, 0) == -96.f);
    CHECK(dev->reverb(1).enabled);
    CHECK(dev->reverb(1).room_size == doctest::Approx(0.9f));
    CHECK(dev->limiter().enabled);
    CHECK(dev->limiter().ceiling_db == -3.f);
    const u64 h = dev->command_hash();
    w.tick(dt, 2);
    CHECK(dev->command_hash() == h);  // nothing changed, nothing sent

    // A keyed field (a timeline, gameplay) reaches the mixer on the next tick.
    w.try_get_mut<AudioBusEffects>(a)->lowpass_hz = 1200.f;
    w.tick(dt, 3);
    CHECK(dev->bus_filter(Bus_Music).lowpass_hz == 1200.f);

    w.destroy(b);
    w.destroy(c);
    w.tick(dt, 4);
    CHECK_FALSE(dev->reverb(1).enabled);
    CHECK_FALSE(dev->limiter().enabled);
    w.destroy_all();
    w.tick(dt, 5);
    CHECK(dev->bus_filter(Bus_Music).lowpass_hz == 0.f);
    CHECK(dev->bus_send_db(Bus_Music, 1) == -96.f);
}
