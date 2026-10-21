// The v0.5 mixer (ROADMAP M6, ADR-0026): 48 voices, sample-accurate play_at, the bus graph,
// per-bus biquad EQ, the look-ahead limiter and 3D spatialization — all through the Offline
// backend, i.e. the exact callback body the device runs (audio-input-ui §6.2).

#include <cmath>

#include "audio_test_util.h"
#include "mce/audio/components.h"
#include "test_support.h"

using namespace mce;
using namespace mce::audio;

namespace {

// Mono ramp clip: frame i holds (i + 1) / n, so any output sample names the clip frame it came from.
Array<f32> ramp_clip(u32 frames) {
    Array<f32> a;
    a.resize(frames);
    for (u32 i = 0; i < frames; ++i) a[i] = static_cast<f32>(i + 1) / static_cast<f32>(frames);
    return a;
}

Array<f32> sine_clip(u32 frames, f64 hz, f32 amp) {
    Array<f32> a;
    a.resize(frames);
    for (u32 i = 0; i < frames; ++i)
        a[i] = amp * static_cast<f32>(std::sin(2.0 * 3.14159265358979323846 * hz * i / kSampleRate));
    return a;
}

u32 first_nonzero(const Array<f32>& out, u32 channel = 0) {
    for (u32 i = 0; i * 2 + channel < out.size(); ++i)
        if (out[i * 2 + channel] != 0.f) return i;
    return ~0u;
}

f32 peak(const Array<f32>& out, u32 from, u32 to, u32 channel = 0) {
    f32 m = 0.f;
    for (u32 i = from; i < to; ++i) m = std::fmax(m, std::fabs(out[i * 2 + channel]));
    return m;
}

}  // namespace

TEST_CASE("FR-AUD-020 v0.5: 48 real voices; the 49th steals the least important without a click") {
    static_assert(kMaxRealVoices == 48);
    auto dev = test::offline_device();
    Array<f32> clip = test::dc_clip(48000, 0.01f);
    auto h = dev->register_clip(test::payload(clip)).value();
    AudioHandle v[kMaxRealVoices];
    for (u32 i = 0; i < kMaxRealVoices; ++i) {
        PlayParams p;
        p.loop = true;
        p.pan = -1.f;
        p.priority = i == 17 ? 220 : 100;
        v[i] = dev->play(h, p);
    }
    Array<f32> out;
    test::render(*dev, out, 960);
    CHECK(dev->stats().voices_real == 48);
    CHECK(out[900 * 2] == doctest::Approx(0.48f).epsilon(1e-4));

    PlayParams p;
    p.priority = 100;
    p.pan = -1.f;
    const AudioHandle n49 = dev->play(h, p);
    Array<f32> after;
    test::render(*dev, after, 960);
    CHECK(dev->stats().steals == 1);
    CHECK_FALSE(dev->is_playing(v[17]));
    CHECK(dev->is_playing(n49));
    CHECK(dev->stats().voices_real == 48);
    // The stolen voice fades over 5 ms while the newcomer starts: no step larger than one voice.
    f32 max_step = std::fabs(after[0] - out[959 * 2]);
    for (u32 i = 1; i < 960; ++i) max_step = std::fmax(max_step, std::fabs(after[i * 2] - after[(i - 1) * 2]));
    CHECK(max_step <= 0.01f + 1e-5f);
}

TEST_CASE("play_at lands on the requested mixer frame, whatever the callback size") {
    for (u32 block : {480u, 37u, 1024u}) {
        CAPTURE(block);
        auto dev = test::offline_device();
        Array<f32> clip = ramp_clip(2000);
        auto h = dev->register_clip(test::payload(clip)).value();
        Array<f32> out;
        test::render(*dev, out, 1000, block);
        REQUIRE(dev->mix_clock() == 1000);

        PlayParams p;
        p.pan = -1.f;
        const u64 at = dev->mix_clock() + 1123;  // mid-block for every block size above
        const AudioHandle v = dev->play_at(h, at, p);
        REQUIRE_FALSE(v.is_null());
        CHECK(dev->is_playing(v));
        test::render(*dev, out, 4000, block);
        CHECK(first_nonzero(out) == 1123);
        CHECK(out[1123 * 2] == doctest::Approx(clip[0]));
        CHECK(out[(1123 + 500) * 2] == doctest::Approx(clip[500]));
        CHECK(out[(1123 + 1999) * 2] == doctest::Approx(clip[1999]));
        CHECK(out[(1123 + 2000) * 2] == 0.f);
        CHECK_FALSE(dev->is_playing(v));
    }
}

TEST_CASE("play_at: a late request starts mid-clip; stopping before the start is silent") {
    auto dev = test::offline_device();
    Array<f32> clip = ramp_clip(4000);
    auto h = dev->register_clip(test::payload(clip)).value();
    Array<f32> out;
    test::render(*dev, out, 960);

    PlayParams p;
    p.pan = -1.f;
    dev->play_at(h, dev->mix_clock() - 300, p);  // 300 frames late
    test::render(*dev, out, 480);
    CHECK(out[0] == doctest::Approx(clip[300]));  // already 300 frames in, still aligned to its clock

    const AudioHandle w = dev->play_at(h, dev->mix_clock() + 2000, p);
    test::render(*dev, out, 480);
    CHECK(dev->is_playing(w));
    dev->stop(w);
    test::render(*dev, out, 480);
    CHECK_FALSE(dev->is_playing(w));
    // Nothing of the cancelled voice ever reached the output (the late voice is still going).
    Array<f32> tail;
    test::render(*dev, tail, 4800);
    CHECK(first_nonzero(tail) == 0);  // the late voice
    CHECK(tail[3500 * 2] == 0.f);     // ...which ended at its clip's end; no second voice appeared

    // Scheduled plays enter the command hash; immediate plays hash as before (stable replays).
    auto hash_of = [&](u64 at) {
        auto d = test::offline_device();
        auto c = d->register_clip(test::payload(clip)).value();
        d->play_at(c, at);
        return d->command_hash();
    };
    CHECK(hash_of(0) != hash_of(5000));
    CHECK(hash_of(5000) == hash_of(5000));
}

TEST_CASE("FR-AUD-030: the bus graph matches an offline reference mix within -60 dB") {
    auto dev = test::offline_device();
    // Graph: User0(4) -> SFX(2) -> Master; User1(5) -> User0(4); Music(1) -> Master.
    REQUIRE(dev->set_bus_parent(Bus_User0, Bus_SFX));
    REQUIRE(dev->set_bus_parent(Bus_User0 + 1, Bus_User0));
    CHECK(dev->bus_parent(Bus_User0 + 1) == Bus_User0);
    CHECK_FALSE(dev->set_bus_parent(Bus_SFX, Bus_User0));  // parents must have a lower id
    CHECK_FALSE(dev->set_bus_parent(Bus_Master, Bus_SFX));
    CHECK_FALSE(dev->set_bus_parent(kBusCount, Bus_Master));

    Array<f32> a = sine_clip(48000, 220.0, 0.3f), b = sine_clip(48000, 330.0, 0.3f), c = sine_clip(48000, 440.0, 0.3f);
    auto ha = dev->register_clip(test::payload(a)).value();
    auto hb = dev->register_clip(test::payload(b)).value();
    auto hc = dev->register_clip(test::payload(c)).value();
    const f32 g_u0 = 0.5f, g_u1 = 0.8f, g_sfx = 0.7f, g_music = 0.9f, g_master = 0.6f;
    dev->set_bus_gain(Bus_User0, linear_to_db(g_u0));
    dev->set_bus_gain(Bus_User0 + 1, linear_to_db(g_u1));
    dev->set_bus_gain(Bus_SFX, linear_to_db(g_sfx));
    dev->set_bus_gain(Bus_Music, linear_to_db(g_music));
    dev->set_bus_gain(Bus_Master, linear_to_db(g_master));
    Array<f32> out;
    test::render(*dev, out, 960);  // let the gain ramps land before any voice starts

    PlayParams pa, pb, pc;
    pa.bus = Bus_User0 + 1;
    pb.bus = Bus_Music;
    pc.bus = Bus_SFX;
    pa.pan = pb.pan = pc.pan = -1.f;
    dev->play(ha, pa);
    dev->play(hb, pb);
    dev->play(hc, pc);
    test::render(*dev, out, 9600);

    f32 max_err = 0.f;
    for (u32 i = 0; i < 9600; ++i) {
        const f32 ref = g_master * (a[i] * g_u1 * g_u0 * g_sfx + c[i] * g_sfx + b[i] * g_music);
        max_err = std::fmax(max_err, std::fabs(out[i * 2] - ref));
        CHECK(out[i * 2 + 1] == doctest::Approx(0.f).epsilon(1e-6));
    }
    CHECK(max_err < 1e-3f);  // -60 dB
}

TEST_CASE("FR-AUD-060: RBJ biquad coefficients match the cookbook responses") {
    BiquadDesc lp;
    lp.type = BiquadDesc::Type::LowPass;
    lp.freq_hz = 1000.f;
    const BiquadCoeffs c = biquad_coeffs(lp);
    CHECK(biquad_magnitude(c, 1.f) == doctest::Approx(1.f).epsilon(1e-4));
    CHECK(biquad_magnitude(c, 1000.f) == doctest::Approx(std::sqrt(0.5f)).epsilon(1e-3));  // -3 dB at fc, Q = 1/sqrt2
    CHECK(biquad_magnitude(c, 8000.f) < 0.02f);

    BiquadDesc pk;
    pk.type = BiquadDesc::Type::Peaking;
    pk.freq_hz = 2000.f;
    pk.q = 2.f;
    pk.gain_db = 9.f;
    CHECK(linear_to_db(biquad_magnitude(biquad_coeffs(pk), 2000.f)) == doctest::Approx(9.f).epsilon(1e-3));
    CHECK(linear_to_db(biquad_magnitude(biquad_coeffs(pk), 50.f)) == doctest::Approx(0.f).epsilon(0.05));

    BiquadDesc ls;
    ls.type = BiquadDesc::Type::LowShelf;
    ls.freq_hz = 200.f;
    ls.gain_db = -12.f;
    CHECK(linear_to_db(biquad_magnitude(biquad_coeffs(ls), 5.f)) == doctest::Approx(-12.f).epsilon(0.01));
    CHECK(linear_to_db(biquad_magnitude(biquad_coeffs(ls), 15000.f)) == doctest::Approx(0.f).epsilon(0.05));
    BiquadDesc hs = ls;
    hs.type = BiquadDesc::Type::HighShelf;
    hs.freq_hz = 5000.f;
    hs.gain_db = 6.f;
    CHECK(linear_to_db(biquad_magnitude(biquad_coeffs(hs), 20000.f)) == doctest::Approx(6.f).epsilon(0.02));

    BiquadDesc hp;
    hp.type = BiquadDesc::Type::HighPass;
    hp.freq_hz = 500.f;
    CHECK(biquad_magnitude(biquad_coeffs(hp), 10.f) < 1e-3f);
    CHECK(biquad_magnitude(biquad_coeffs(hp), 500.f) == doctest::Approx(std::sqrt(0.5f)).epsilon(1e-3));
}

TEST_CASE("FR-AUD-060: bus EQ impulse and sine sweep match the coefficient model") {
    BiquadDesc pk;
    pk.type = BiquadDesc::Type::Peaking;
    pk.freq_hz = 1000.f;
    pk.q = 1.f;
    pk.gain_db = 6.f;
    const BiquadCoeffs k = biquad_coeffs(pk);

    // Impulse response: the mixer's TDF-II run equals the difference equation run in double.
    {
        auto dev = test::offline_device();
        REQUIRE(dev->set_bus_eq(Bus_SFX, 0, pk));
        Array<f32> imp = test::dc_clip(256, 0.f);
        imp[0] = 0.5f;  // b0 > 1 here: stay clear of the master hard clip
        PlayParams p;
        p.pan = -1.f;
        dev->play(dev->register_clip(test::payload(imp)).value(), p);
        Array<f32> out;
        test::render(*dev, out, 256);
        f64 x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (u32 i = 0; i < 256; ++i) {
            const f64 x = imp[i];
            const f64 y = k.b0 * x + k.b1 * x1 + k.b2 * x2 - k.a1 * y1 - k.a2 * y2;
            x2 = x1, x1 = x, y2 = y1, y1 = y;
            CHECK(std::fabs(out[i * 2] - y) < 1e-6);
        }
    }
    // Swept sines: steady-state amplitude through the bus equals |H(f)|.
    for (f32 hz : {100.f, 500.f, 1000.f, 3000.f, 12000.f}) {
        CAPTURE(hz);
        auto dev = test::offline_device();
        REQUIRE(dev->set_bus_eq(Bus_SFX, 2, pk));  // any band slot
        Array<f32> s = sine_clip(24000, hz, 0.25f);
        PlayParams p;
        p.pan = -1.f;
        dev->play(dev->register_clip(test::payload(s)).value(), p);
        Array<f32> out;
        test::render(*dev, out, 24000);
        const f32 amp = peak(out, 12000, 24000);  // well past the transient
        CHECK(amp == doctest::Approx(0.25f * biquad_magnitude(k, hz)).epsilon(0.01));
    }
    // Off bypasses the band again.
    auto dev = test::offline_device();
    REQUIRE(dev->set_bus_eq(Bus_SFX, 0, pk));
    BiquadDesc off;
    REQUIRE(dev->set_bus_eq(Bus_SFX, 0, off));
    CHECK_FALSE(dev->set_bus_eq(Bus_SFX, kEqBands, pk));
    Array<f32> dc = test::dc_clip(4800, 0.5f);
    PlayParams p;
    p.pan = -1.f;
    dev->play(dev->register_clip(test::payload(dc)).value(), p);
    Array<f32> out;
    test::render(*dev, out, 1000);
    CHECK(out[10 * 2] == doctest::Approx(0.5f));
}

TEST_CASE("FR-AUD-060: the look-ahead limiter holds a +6 dB hot input under its ceiling, without clicks") {
    auto dev = test::offline_device();
    LimiterDesc ld;
    ld.enabled = true;
    ld.ceiling_db = -1.f;
    ld.release_ms = 50.f;
    dev->set_limiter(ld);
    const f32 ceiling = db_to_linear(-1.f);

    // 0.25 s quiet, 0.5 s at +6 dBFS (amplitude 2), 1 s quiet again.
    const u32 n = 84000;
    Array<f32> s = sine_clip(n, 440.0, 0.25f);
    for (u32 i = 12000; i < 36000; ++i) s[i] *= 8.f;
    PlayParams p;
    p.pan = -1.f;
    dev->play(dev->register_clip(test::payload(s)).value(), p);
    Array<f32> out;
    test::render(*dev, out, n + kLimiterLookahead);

    const u32 lat = kLimiterLookahead - 1;
    CHECK(peak(out, 0, n + lat) <= ceiling + 1e-6f);             // never above the ceiling
    CHECK(peak(out, 13000 + lat, 36000 + lat) > ceiling * 0.97f);  // and actually uses it
    CHECK(dev->stats().limiter_gain <= 1.f);
    // Quiet input passes untouched (delayed by the look-ahead).
    CHECK(out[(6000 + lat) * 2] == doctest::Approx(s[6000]).epsilon(1e-5));
    // Released: a second after the burst the gain is back at unity.
    CHECK(out[(80000 + lat) * 2] == doctest::Approx(s[80000]).epsilon(1e-3).scale(1e-6));
    // No click: the gain ramps across the look-ahead, so the output never jumps by more than the
    // hot sine itself can between two samples (2 * 2pi * 440 / 48000 ≈ 0.115).
    f32 max_step = 0.f;
    for (u32 i = 1; i < n; ++i) max_step = std::fmax(max_step, std::fabs(out[i * 2] - out[(i - 1) * 2]));
    CHECK(max_step < 0.12f);
}

TEST_CASE("FR-AUD-050: attenuation models and listener-relative pan") {
    AttenuationCurve inv;  // Inverse, 1..50, rolloff 1
    CHECK(attenuation_gain(inv, 0.5f) == doctest::Approx(1.f));
    CHECK(attenuation_gain(inv, 4.f) == doctest::Approx(0.25f));
    CHECK(attenuation_gain(inv, 500.f) == doctest::Approx(1.f / 50.f));  // clamped at max
    AttenuationCurve lin;
    lin.model = AttenuationCurve::Model::Linear;
    lin.min_distance = 2.f;
    lin.max_distance = 12.f;
    CHECK(attenuation_gain(lin, 7.f) == doctest::Approx(0.5f));
    CHECK(attenuation_gain(lin, 20.f) == doctest::Approx(0.f));
    AttenuationCurve ex;
    ex.model = AttenuationCurve::Model::Exponential;
    ex.rolloff = 2.f;
    CHECK(attenuation_gain(ex, 4.f) == doctest::Approx(1.f / 16.f));

    Listener l;  // at the origin, looking down -Z, +X is right
    CHECK(spatialize(l, inv, {10.f, 0.f, 0.f}).pan == doctest::Approx(1.f));
    CHECK(spatialize(l, inv, {-10.f, 0.f, 0.f}).pan == doctest::Approx(-1.f));
    CHECK(spatialize(l, inv, {0.f, 0.f, -10.f}).pan == doctest::Approx(0.f));
    CHECK(spatialize(l, inv, {0.5f, 0.f, 0.f}).pan == doctest::Approx(0.5f));  // faded inside min_distance
    l.forward = {0.f, 0.f, 1.f};  // turned around: sides swap
    CHECK(spatialize(l, inv, {10.f, 0.f, 0.f}).pan == doctest::Approx(-1.f));
    l.gain_db = linear_to_db(0.5f);
    CHECK(spatialize(l, inv, {0.f, 0.f, 4.f}).gain == doctest::Approx(0.125f));
}

TEST_CASE("FR-AUD-050: a 3D voice attenuates per its curve as the listener retreats") {
    auto dev = test::offline_device();
    AttenuationCurve curve;
    curve.model = AttenuationCurve::Model::Inverse;
    curve.min_distance = 2.f;
    curve.max_distance = 100.f;
    curve.rolloff = 1.5f;
    REQUIRE(dev->set_attenuation(3, curve));
    CHECK_FALSE(dev->set_attenuation(0, curve));
    CHECK_FALSE(dev->set_attenuation(kMaxAttenuationCurves, curve));

    Array<f32> dc = test::dc_clip(48000 * 4, 0.8f);
    auto h = dev->register_clip(test::payload(dc)).value();
    PlayParams p;
    p.loop = true;
    p.attenuation_id = 3;
    p.position = {0.f, 0.f, -1.f};  // straight ahead: centred, equal power on both sides
    const AudioHandle v = dev->play(h, p);
    Array<f32> out;
    for (f32 back : {0.f, 3.f, 8.f, 20.f, 60.f, 150.f}) {
        CAPTURE(back);
        Listener l;
        l.position = {0.f, 0.f, back};
        dev->set_listener(l);
        test::render(*dev, out, 960);  // ramp (480) lands inside the window
        const f32 expected = 0.8f * attenuation_gain(curve, back + 1.f) * std::sqrt(0.5f);
        CHECK(out[900 * 2] == doctest::Approx(expected).epsilon(1e-3));
        CHECK(out[900 * 2 + 1] == doctest::Approx(expected).epsilon(1e-3));
    }
    // An unchanged listener sends nothing.
    const u64 hsh = dev->command_hash();
    dev->set_listener(dev->listener());
    CHECK(dev->command_hash() == hsh);
    // Moving the source to the right pans it; gain edits keep the attenuation.
    Listener l;
    dev->set_listener(l);
    dev->set_handle_position(v, {30.f, 0.f, 0.f});
    dev->set_handle_gain(v, linear_to_db(0.5f));
    test::render(*dev, out, 960);
    CHECK(out[900 * 2] == doctest::Approx(0.f).epsilon(1e-6));
    CHECK(out[900 * 2 + 1] == doctest::Approx(0.8f * 0.5f * attenuation_gain(curve, 30.f)).epsilon(1e-3));
}

TEST_CASE("audio_bridge: AudioListener follows its transform and 3D sources follow theirs") {
    auto dev = test::offline_device();
    AttenuationCurve curve;
    curve.min_distance = 1.f;
    REQUIRE(dev->set_attenuation(1, curve));
    Array<f32> dc = test::dc_clip(48000, 0.5f);
    const ClipHandle h = dev->register_clip(test::payload(dc)).value();

    World w;
    install(w, *dev);
    w.register_components<LocalTransform>();
    LocalTransform cam;
    cam.position = {0.f, 0.f, 10.f};
    const Entity ear = w.spawn_with(AudioListener{}, cam);
    AudioSource src;
    src.clip_handle = h;
    src.loop = true;
    src.play_on_start = true;
    AudioSpatial spatial;
    spatial.attenuation_id = 1;
    LocalTransform at;
    at.position = {0.f, 0.f, 6.f};  // 4 in front of the listener
    const Entity e = w.spawn_with(src, spatial, at);

    const f64 dt = 1.0 / 60.0;
    w.tick(dt, 1);
    CHECK(dev->listener().position.z == doctest::Approx(10.f));
    Array<f32> out;
    test::render(*dev, out, 960);
    CHECK(out[900 * 2] == doctest::Approx(0.5f * 0.25f * std::sqrt(0.5f)).epsilon(1e-3));

    // The source walks to the listener's right; the bridge forwards the move.
    w.try_get_mut<LocalTransform>(e)->position = {4.f, 0.f, 10.f};
    w.tick(dt, 2);
    test::render(*dev, out, 960);
    CHECK(out[900 * 2] == doctest::Approx(0.f).epsilon(1e-6));
    CHECK(out[900 * 2 + 1] == doctest::Approx(0.5f * 0.25f).epsilon(1e-3));

    // The listener turns 180° about Y: the source is now on its left.
    w.try_get_mut<LocalTransform>(ear)->rotation = math::quat{0.f, 1.f, 0.f, 0.f};
    w.tick(dt, 3);
    test::render(*dev, out, 960);
    CHECK(out[900 * 2] == doctest::Approx(0.5f * 0.25f).epsilon(1e-3));
    CHECK(out[900 * 2 + 1] == doctest::Approx(0.f).epsilon(1e-5));
}

TEST_CASE("M-VIEWS 4: several listeners hear a 3D voice from the nearest one, or mixed") {
    AttenuationCurve lin;
    lin.model = AttenuationCurve::Model::Linear;
    lin.min_distance = 1.f;
    lin.max_distance = 21.f;
    Listener ears[2];
    ears[1].position = {100.f, 0.f, 0.f};  // the second player, far to the right
    const Span<const Listener> both(ears, 2);
    // A sound 5 m right of the first ear: the first ear is nearest, it pans hard right.
    const Spatial near1 = spatialize(both, ListenerMix::Nearest, lin, {5.f, 0.f, 0.f});
    CHECK(near1.gain == doctest::Approx(0.8f));
    CHECK(near1.pan == doctest::Approx(1.f));
    // 3 m left of the second ear: the second hears it, panned left.
    const Spatial near2 = spatialize(both, ListenerMix::Nearest, lin, {97.f, 0.f, 0.f});
    CHECK(near2.gain == doctest::Approx(0.9f));
    CHECK(near2.pan == doctest::Approx(-1.f));
    CHECK(spatialize(both, ListenerMix::First, lin, {97.f, 0.f, 0.f}).gain == doctest::Approx(0.f));
    // Mixed, between two ears 10 m apart: the louder ear's gain, the pans weighted by gain.
    ears[1].position = {10.f, 0.f, 0.f};
    const Spatial mixed = spatialize(both, ListenerMix::Mixed, lin, {3.f, 0.f, 0.f});
    CHECK(mixed.gain == doctest::Approx(0.9f));  // 3 m from the first ear (7 m from the second: 0.7)
    CHECK(mixed.pan == doctest::Approx((0.9f * 1.f + 0.7f * -1.f) / 1.6f));
    CHECK(spatialize(Span<const Listener>{}, ListenerMix::Mixed, lin, {}).gain == doctest::Approx(1.f));
}

TEST_CASE("audio_bridge: AudioListenerMix makes every AudioListener an ear") {
    auto dev = test::offline_device();
    World w;
    install(w, *dev);
    w.register_components<LocalTransform>();
    LocalTransform a, b;
    a.position = {-5.f, 0.f, 0.f};
    b.position = {5.f, 0.f, 0.f};
    w.spawn_with(AudioListener{}, a);
    w.spawn_with(AudioListener{}, b);
    w.tick(1.0 / 60.0, 1);
    CHECK(dev->listeners().size() == 1);  // no AudioListenerMix: the first listener only
    CHECK(dev->listener().position.x == doctest::Approx(-5.f));
    const Entity mix = w.spawn_with(AudioListenerMix{});
    w.tick(1.0 / 60.0, 1);
    REQUIRE(dev->listeners().size() == 2);
    CHECK(dev->listener_mix() == ListenerMix::Nearest);
    CHECK(dev->listeners()[1].position.x == doctest::Approx(5.f));
    w.try_get_mut<AudioListenerMix>(mix)->mix = 2;
    w.tick(1.0 / 60.0, 1);
    CHECK(dev->listener_mix() == ListenerMix::Mixed);
    w.destroy(mix);
    w.tick(1.0 / 60.0, 1);
    CHECK(dev->listeners().size() == 1);
    CHECK(dev->listener_mix() == ListenerMix::First);
}
