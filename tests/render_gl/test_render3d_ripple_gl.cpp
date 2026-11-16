// M-RAINFX 7b against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: rain ripples are rings
// that tilt the puddles' normal, from one uniform branch (strength 0, or no puddle under the fragment, is today's
// frame; the goldens hold it). The clock is Scene3D::set_time (the scene's), never the wall clock. One frame per
// render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "fixture3d.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool pixel_differs(const Array<u8>& a, const Array<u8>& b, usize p) {
    return a[p * 4] != b[p * 4] || a[p * 4 + 1] != b[p * 4 + 1] || a[p * 4 + 2] != b[p * 4 + 2];
}

// Pixels whose color differs between the two frames.
u32 changed_pixels(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize p = 0; p < a.size() / 4; ++p) n += pixel_differs(a, b, p);
    return n;
}

// Mean |luma difference| over the whole frame, of full scale.
f64 frame_change(const Array<u8>& a, const Array<u8>& b) {
    f64 s = 0.0;
    for (usize i = 0; i < a.size(); i += 4) {
        const f64 la = 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
        const f64 lb = 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
        s += std::fabs(la - lb);
    }
    return s / (f64(a.size()) / 4.0 * 255.0);
}

// Wide rings on every cell: the most ring pixels at the fixture's 160 x 120.
Ripples test_ripples() {
    Ripples r = ripple_plaza_ripples(1.0f);
    r.density = 1.0f;
    r.ring_width = 0.6f;
    return r;
}

WetSurfaces puddled(f32 puddles = 0.5f) {
    WetSurfaces w = wet_plaza_wet(1.0f, puddles);
    w.puddle_scale = 1.0f;
    return w;
}

// Looking down on the plaza's ground (the sky's zenith in the puddles), no bloom (a puddle's glint would glow onto
// the ground around it: not what is measured).
struct TopDown {
    Fixture3D f;
    TopDown() { f.pitch_deg = 89.0f; }
    Array<u8> draw(const Ripples& r, const WetSurfaces& w, f32 time, bool shadows = true) {
        f.render(0.0f, shadows, [&](Scene3D& s) {
            ripple_plaza_scene(s, r, w, time, f.world_offset);
            PostFx post = s.post();
            post.bloom = false;
            s.set_post(post);
        });
        return f.pixels;
    }
};

}  // namespace

TEST_CASE("render3d_gl: ripples change only puddle pixels; the ground around the puddles is byte-identical") {
    TopDown t;
    const Array<u8> no_puddles = t.draw(Ripples{}, puddled(0.0f), 1.0f);
    const Array<u8> still = t.draw(Ripples{}, puddled(), 1.0f);
    const Array<u8> rippled = t.draw(test_ripples(), puddled(), 1.0f);
    // A puddle pixel: the puddles changed it. The puddle's soft rim also has fragments whose cover is too small to
    // move the 8-bit color but not too small for a ring's tilt, so a pixel next to a puddle pixel counts as the rim.
    const auto is_puddle = [&](i64 x, i64 y) {
        return x >= 0 && y >= 0 && x < i64(kW) && y < i64(kH) && pixel_differs(no_puddles, still, usize(y * kW + x));
    };
    u32 puddle = 0, in_puddle = 0, rim = 0, outside = 0;
    for (i64 y = 0; y < i64(kH); ++y)
        for (i64 x = 0; x < i64(kW); ++x) {
            const bool in = is_puddle(x, y);
            puddle += in;
            if (!pixel_differs(still, rippled, usize(y * kW + x))) continue;
            bool near = false;
            for (i64 oy = -1; oy <= 1; ++oy)
                for (i64 ox = -1; ox <= 1; ++ox) near = near || is_puddle(x + ox, y + oy);
            (in ? in_puddle : near ? rim : outside) += 1;
        }
    MESSAGE(puddle << " puddle pixels of " << kW * kH << "; ripples changed " << in_puddle << " of them, " << rim
                   << " on their rim and " << outside << " others");
    CHECK(puddle > 1000);  // measured 4563
    CHECK(outside == 0);
    CHECK(rim * 100 < puddle);  // measured 6
    CHECK(in_puddle > puddle / 4);  // measured 2540
    CHECK(t.f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: strength 0, a dry or puddle-less frame, enabled false and the Low tier draw no ripples") {
    TopDown t;
    const Ripples r = test_ripples();
    // Ripples need puddles: with none under them the frame is the ripple-less one byte for byte.
    CHECK(same(t.draw(r, WetSurfaces{}, 1.0f), t.draw(Ripples{}, WetSurfaces{}, 1.0f)));
    CHECK(same(t.draw(r, puddled(0.0f), 1.0f), t.draw(Ripples{}, puddled(0.0f), 1.0f)));
    WetSurfaces no_puddles = puddled();
    no_puddles.puddles_enabled = false;
    CHECK(same(t.draw(r, no_puddles, 1.0f), t.draw(Ripples{}, no_puddles, 1.0f)));
    // With puddles: strength 0 (everything else set), enabled false and Low switch the rings off.
    const Array<u8> still = t.draw(Ripples{}, puddled(), 1.0f);
    CHECK_FALSE(same(t.draw(r, puddled(), 1.0f), still));
    Ripples zero = r;
    zero.strength = 0.0f;
    CHECK(same(t.draw(zero, puddled(), 1.0f), still));
    Ripples disabled = r;
    disabled.enabled = false;
    CHECK(same(t.draw(disabled, puddled(), 1.0f), still));
    const Ripples low = apply_quality(quality_settings(QualityTier::Low), r);
    CHECK(low.strength == 0.0f);
    CHECK(same(t.draw(low, puddled(), 1.0f), still));
    CHECK(t.f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: ripples animate with the scene clock and are deterministic at a given time") {
    TopDown t;
    const Ripples r = test_ripples();
    const Array<u8> a = t.draw(r, puddled(), 2.0f);
    CHECK(same(t.draw(r, puddled(), 2.0f), a));  // the same time twice: the same frame
    const Array<u8> later = t.draw(r, puddled(), 2.25f);
    const u32 moved = changed_pixels(a, later);
    // The clock alone changes nothing without ripples (the control).
    const u32 still_moved = changed_pixels(t.draw(Ripples{}, puddled(), 2.0f), t.draw(Ripples{}, puddled(), 2.25f));
    // speed scales the clock: speed 2 at t is speed 1 at 2t.
    Ripples fast = r;
    fast.speed = 2.0f;
    const Array<u8> fast_at_1 = t.draw(fast, puddled(), 1.0f);
    const u32 speed_vs_time = changed_pixels(fast_at_1, a);
    // The rings repeat every 60 s of speed-scaled time (every drop period divides it): t + 60 draws t's rings.
    const u32 wrapped = changed_pixels(t.draw(r, puddled(), 62.0f), a);
    MESSAGE("0.25 s later " << moved << " pixels changed (without ripples " << still_moved << "); speed 2 at 1 s vs "
                            << "speed 1 at 2 s: " << speed_vs_time << "; 62 s vs 2 s: " << wrapped);
    CHECK(moved > 200);  // measured 3209 of the 4563 puddle pixels
    CHECK(still_moved == 0);  // measured 0
    CHECK(speed_vs_time * 20 < moved);  // float rounding of the scaled clock only: measured 0
    CHECK(wrapped * 20 < moved);  // measured 0
    CHECK(t.f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: ripples stay put across a 1000 m floating-origin shift") {
    TopDown t;
    const Ripples r = test_ripples();
    // No shadows: their fit snaps to world-space texels, which a 1000 m move does not keep (not what is tested).
    const Array<u8> home = t.draw(r, puddled(), 1.0f, false);
    const Array<u8> still_home = t.draw(Ripples{}, puddled(), 1.0f, false);
    const vec3 shift{1000.0f, 0.0f, 1000.0f};
    t.f.world_offset = -shift;  // the world moved by -shift ...
    const Array<u8> unannounced = t.draw(r, puddled(), 1.0f, false);  // ... without telling the renderer
    t.f.render(0.0f, false, [&](Scene3D& s) {
        ripple_plaza_scene(s, r, puddled(), 1.0f, t.f.world_offset);
        PostFx post = s.post();
        post.bloom = false;
        s.set_post(post);
        s.set_origin_shift(shift);  // ... and told (once: Forward3D keeps the origin): it carries it into the drops
    });
    const Array<u8> shifted = t.f.pixels;
    // The puddled frame without ripples across the same move: what 1000 m does to the rest of the frame, the baseline.
    const Array<u8> still_shifted = t.draw(Ripples{}, puddled(), 1.0f, false);
    t.f.world_offset = {0.0f, 0.0f, 0.0f};
    const f64 moved = frame_change(home, shifted), control = frame_change(home, unannounced);
    const f64 baseline = frame_change(still_home, still_shifted);
    MESSAGE("frame change after the shift " << moved * 100.0 << "% (without ripples: " << baseline * 100.0
                                            << "%), without set_origin_shift " << control * 100.0 << "%");
    CHECK(moved < baseline + 0.001);  // measured 0.0207% (without ripples 0.0200%), without the shift 5.50%
    CHECK(control > 10.0 * moved);
    CHECK(t.f.device->validation_error_count() == 0);
}
