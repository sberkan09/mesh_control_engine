// M-LENSRAIN 42c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: rain on the camera
// lens, one fullscreen pass from the HDR frame into the post-fx ping-pong target before bloom (Forward3D::
// record_lens_rain, kLensRainFS). Disabled, amount 0 or NaN, the Low tier, r.lens_rain 0 and a render-to-texture view
// draw the frame without the pass bit for bit; a wet lens changes pixels only inside the drop discs a CPU replica of
// the shader's layout predicts (built on lens_rain_hash, so the GLSL hash port is pinned against the C++ one through
// the pixels), and every predicted drop shows; the same seed and time give the same frame, another seed another; the
// drops fall down the screen (measured in pixels, independent of the replica), 5 ms and the clock's wrap barely move
// them; trails lie above their drops; a drop shows the far side of its centre, with a sampled radius that grows to the
// rim (a split-colour sky); a drop that starts to exist fades in instead of popping; amount 1 covers a quarter of the
// view, clearly more than 0.3; the glass blur softens a hard edge by a bounded amount; with TAA the next frame's
// history holds no drops; Medium draws one grid, High two; two views of different sizes keep their own targets. One 160
// x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/lens_rain.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize p) {
    return a[p * 4] == b[p * 4] && a[p * 4 + 1] == b[p * 4 + 1] && a[p * 4 + 2] == b[p * 4 + 2];
}

int px_diff(const Array<u8>& a, const Array<u8>& b, usize p) {
    int m = 0;
    for (usize c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c])));
    return m;
}

u32 changed_px(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += same_px(a, b, p) ? 0u : 1u;
    return n;
}

// Pixels where some channel moved by more than `step` 8-bit levels (a 1-level flip from a sub-pixel move does not
// count).
u32 moved_px(const Array<u8>& a, const Array<u8>& b, int step) {
    u32 n = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) n += px_diff(a, b, p) > step ? 1u : 0u;
    return n;
}

f64 luma(const Array<u8>& px, usize p) { return 0.2126 * px[p * 4] + 0.7152 * px[p * 4 + 1] + 0.0722 * px[p * 4 + 2]; }

constexpr f32 kAspect = f32(kW) / f32(kH);

// The pixel's centre in view heights from the bottom left (kLensRainFS's p: the fullscreen uv times (aspect, 1));
// read_pixels is top row first, v = 0 is the bottom row.
math::vec2 pos_of(usize p) {
    const usize x = p % kW, y = p / kW;
    return {(f32(x) + 0.5f) / f32(kW) * kAspect, 1.0f - (f32(y) + 0.5f) / f32(kH)};
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
};

struct CvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    CvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~CvarScope() { cv->reset(); }
    CvarScope(const CvarScope&) = delete;
    CvarScope& operator=(const CvarScope&) = delete;
};

enum class Backdrop { Box, Split, Gradient };

// One frame of the fixture's box scene, the split sky or the gradient sky with `rain` (null: off) at scene time `time`.
Shot draw(Fixture3D& f, const LensRain* rain, f32 time = 0.0f, Backdrop back = Backdrop::Box,
          const std::function<void(Scene3D&)>& more = {}) {
    if (back == Backdrop::Split) lens_rain_split_camera(f);
    if (back == Backdrop::Gradient) lens_rain_gradient_camera(f);
    f.render(0.0f, true, [&](Scene3D& s) {
        if (back == Backdrop::Split) lens_rain_split_sky(s);
        if (back == Backdrop::Gradient) lens_rain_gradient_sky(s);
        lens_rain_scene(s, rain, time);
        if (more) more(s);
    });
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats()};
}

// ---- a CPU replica of kLensRainFS's drop layout (float for float, same order), from lens_rain_frame and
// lens_rain_hash. If the shader's hash or layout drifted from it, the "only inside the discs" and "every disc shows"
// checks below fail. The direction of the fall is checked from the pixels alone (a bug copied into the replica would
// not hide there).
struct Drop {
    math::vec2 c;     // centre, view heights from the bottom left
    f32 r = 0.0f;     // radius, view heights
    f32 fade = 0.0f;  // the shader's weight: appear (e) * the cycle's fade in and out
    f32 e = 0.0f;     // amount * density - u: the drop exists where > 0
    f32 big = 0.0f;   // the per-cell size, 0..1 (trails above 0.5)
    f32 top = 0.0f;   // where its cycle started (view heights, global)
    f32 n = 0.0f;     // whole fall cycles per 600 s
    f32 phase = 0.0f;
    u32 cyc = 0;
    math::vec2 cell0;  // the cell's lower left corner (view heights)
    f32 cell = 0.0f;
};

f32 unit12(u32 h) { return f32(h & 0xfffu) / 4096.0f; }

f32 smooth(f32 e0, f32 e1, f32 x) {
    const f32 t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::vector<Drop> cpu_drops(const LensRain& setting, f32 time, u32 layers) {
    const LensRainFrame fr = lens_rain_frame(setting, time, kAspect);
    std::vector<Drop> out;
    if (!(fr.amount > 0.0f)) return out;
    const f64 t = f64(fr.time), period = 600.0;  // forward3d.cpp kLensRainPeriod
    f32 share = f32((t - period * std::floor(t / period)) / period);
    if (!(share >= 0.0f && share < 1.0f)) share = 0.0f;
    const f32 fall = f32(f64(fr.fall_speed) * period / 60.0);
    for (u32 l = 0; l < layers && l < 2; ++l) {
        const f32 cell = 0.22f * fr.size * (l == 0 ? 1.0f : 0.62f);
        const math::vec2 shift = l == 0 ? math::vec2{0.0f, 0.0f} : math::vec2{0.37f, 0.53f};
        const u32 lseed = fr.seed + l * 0x632be5abu;
        const u32 nx = u32(std::floor(kAspect / cell + shift.x)), ny = u32(std::floor(1.0f / cell + shift.y));
        for (u32 gy = 0; gy <= ny; ++gy) {
            for (u32 gx = 0; gx <= nx; ++gx) {
                const u32 h0 = lens_rain_hash(gx, gy, lseed);
                const f32 e = fr.amount * fr.density - unit12(h0);
                if (e <= 0.0f) continue;
                const u32 h2 = lens_rain_hash(h0, 0xffffffffu, lseed);
                const f32 big = f32(h2 & 0xffu) / 255.0f;
                const f32 rfull = (0.18f + 0.22f * big) * cell;
                const f32 rr = rfull * (0.55f + 0.45f * fr.amount);
                const f32 off0 = unit12(h0 >> 12u);
                f32 n = std::floor(fall * (0.5f + f32(h0 >> 24u) / 256.0f) / (cell - 2.0f * rfull) + 0.5f);
                if (fall > 0.0f) n = std::max(n, 1.0f);
                f32 phase;
                u32 cyc;
                if (n > 0.5f) {
                    const f32 x = share * n + off0;
                    phase = x - std::floor(x);
                    cyc = u32(x) % u32(n);
                } else {
                    phase = 0.1f + 0.8f * off0;
                    cyc = 0u;
                }
                const u32 h1 = lens_rain_hash(h0, cyc, lseed);
                const f32 top = cell - rr;
                const math::vec2 local{rr + (cell - 2.0f * rr) * unit12(h1 >> 8u), top + (rr - top) * phase};
                Drop d;
                d.cell0 = {(f32(gx) - shift.x) * cell, (f32(gy) - shift.y) * cell};
                d.cell = cell;
                d.c = {d.cell0.x + local.x, d.cell0.y + local.y};
                d.r = rr;
                d.e = e;
                d.big = big;
                d.top = d.cell0.y + top;
                d.n = n;
                d.phase = phase;
                d.cyc = cyc;
                d.fade = smooth(0.0f, 0.05f, e) * smooth(0.0f, 0.1f, phase) * (1.0f - smooth(0.9f, 1.0f, phase));
                out.push_back(d);
            }
        }
    }
    return out;
}

f32 dist(math::vec2 a, math::vec2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

// How many discs (grown by `margin`, view heights) hold `pos`; `which` gets the last one.
u32 discs_at(const std::vector<Drop>& drops, math::vec2 pos, f32 margin, usize* which = nullptr) {
    u32 n = 0;
    for (usize i = 0; i < drops.size(); ++i) {
        if (dist(pos, drops[i].c) < drops[i].r + margin) {
            ++n;
            if (which) *which = i;
        }
    }
    return n;
}

bool on_screen(const Drop& d, f32 margin) {
    return d.c.x - d.r > margin && d.c.x + d.r < kAspect - margin && d.c.y - d.r > margin &&
           d.c.y + d.r < 1.0f - margin;
}

usize pixel_at(math::vec2 pos) {
    const i32 x = i32(std::floor(pos.x / kAspect * f32(kW))), y = i32(std::floor((1.0f - pos.y) * f32(kH)));
    return usize(std::clamp(y, 0, i32(kH) - 1)) * kW + usize(std::clamp(x, 0, i32(kW) - 1));
}

u32 tier_layers() { return quality_settings(quality_tier()).lens_rain_layers; }

constexpr f32 kPx = 1.0f / f32(kH);  // one pixel, in view heights

bool blue(const Array<u8>& px, usize p) { return int(px[p * 4 + 2]) > int(px[p * 4]) + 40; }
bool red(const Array<u8>& px, usize p) { return int(px[p * 4]) > int(px[p * 4 + 2]) + 40; }

}  // namespace

TEST_CASE("lens rain: off, amount 0 or NaN draw the frame without the pass bit for bit") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr);
    CHECK_FALSE(plain.stats.lens_rain);
    CHECK(plain.stats.lens_rain_amount == 0.0f);
    LensRain off = lens_rain_setting();
    off.enabled = false;
    off.amount = 1.0f;
    off.blur = 1.0f;
    LensRain dry = lens_rain_setting();
    dry.amount = 0.0f;
    dry.blur = 1.0f;
    LensRain nan = lens_rain_setting();
    nan.amount = std::nanf("");
    for (const LensRain* r : {&off, &dry, &nan}) {
        const Shot s = draw(f, r, 3.0f);
        CHECK_FALSE(s.stats.lens_rain);
        CHECK(s.stats.lens_rain_amount == 0.0f);
        CHECK(same(s.px, plain.px));
    }
}

TEST_CASE("lens rain: a wet lens changes pixels inside the CPU layout's drop discs only, and every disc shows") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr, 0.0f, Backdrop::Gradient);
    const LensRain rain = lens_rain_setting();  // no blur, no trails: only the drops change pixels
    const Shot wet = draw(f, &rain, 2.0f, Backdrop::Gradient);
    REQUIRE(wet.stats.lens_rain);
    CHECK(wet.stats.lens_rain_amount == doctest::Approx(0.8f));
    const std::vector<Drop> drops = cpu_drops(rain, 2.0f, tier_layers());
    REQUIRE(drops.size() > 10);
    const f32 margin = 1.5f * kPx;  // the shader's 1.5-row edge smoothing lies inside the disc; a pixel of slack
    u32 changed = 0, outside = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        if (same_px(wet.px, plain.px, p)) continue;
        ++changed;
        outside += discs_at(drops, pos_of(p), margin) == 0 ? 1u : 0u;
    }
    MESSAGE(drops.size() << " drops predicted, " << changed << " pixels changed, " << outside << " outside every disc");
    CHECK(changed > 1000);
    CHECK(outside == 0);
    // Every clearly visible predicted drop (faded in, at least 2.5 px across, its centre on screen: the grid's last row
    // and column run past the frame's border) changed a pixel near its centre: on the gradient the mirror changes any
    // pixel off the drop's middle row, so a missing one would be a drop the GPU put elsewhere.
    u32 visible = 0, shown = 0;
    for (const Drop& d : drops) {
        if (d.fade < 0.5f || d.r < 2.5f * kPx) continue;
        if (!(d.c.x > 0.0f && d.c.x < kAspect && d.c.y > 0.0f && d.c.y < 1.0f)) continue;
        ++visible;
        bool any = false;
        for (usize p = 0; p < usize(kW) * kH && !any; ++p)
            any = dist(pos_of(p), d.c) < d.r * 0.9f && !same_px(wet.px, plain.px, p);
        shown += any ? 1u : 0u;
    }
    MESSAGE(shown << " of " << visible << " visible predicted drops changed pixels");
    CHECK(visible > 5);
    CHECK(shown == visible);
}

TEST_CASE(
    "lens rain: the same seed and time give the same frame, another seed another; time moves the drops smoothly") {
    Fixture3D f;
    const LensRain rain = lens_rain_setting();
    const Shot a = draw(f, &rain, 1.0f);
    const Shot b = draw(f, &rain, 1.0f);
    CHECK(same(a.px, b.px));
    // Still drops (fall_speed 0) do not depend on the clock.
    const Shot later = draw(f, &rain, 250.0f);
    CHECK(same(a.px, later.px));
    LensRain other = rain;
    other.seed = 6;
    const Shot c = draw(f, &other, 1.0f);
    MESSAGE("seed 5 vs 6: " << changed_px(a.px, c.px) << " pixels differ");
    CHECK(changed_px(a.px, c.px) > 200);

    // Falling drops: 5 ms apart the frame barely moves (no flicker), across the clock's 600 s wrap neither (no pop),
    // 20 s apart it is another field. A sub-pixel move flips the last 8-bit level along the drops' edges (hundreds of
    // pixels at a strong mirror); count the pixels that moved by more than 4 levels.
    LensRain falling = rain;
    falling.refraction = 1.0f;  // the default: at 2 the rim reads 8 radii away, so a sub-pixel move shows more
    falling.fall_speed = 2.0f;
    falling.trail = 1.0f;
    const Shot t0 = draw(f, &falling, 30.0f);
    const Shot t1 = draw(f, &falling, 30.005f);
    const Shot w0 = draw(f, &falling, 599.995f);
    const Shot w1 = draw(f, &falling, 600.0f);
    const Shot t2 = draw(f, &falling, 50.0f);
    const u32 step = moved_px(t0.px, t1.px, 4), wrap = moved_px(w0.px, w1.px, 4), far = moved_px(t0.px, t2.px, 4);
    MESSAGE("pixels moved by more than 4 levels: 5 ms " << step << ", across the wrap " << wrap << ", 20 s " << far);
    CHECK(step < usize(kW) * kH / 100);
    CHECK(wrap < usize(kW) * kH / 100);
    CHECK(far > 10 * (step > wrap ? step : wrap) + 200);
}

TEST_CASE("lens rain: a falling drop moves down the screen at about fall_speed") {
    // One grid (Medium), no trails, the gradient sky. The drop is chosen with the replica, but its movement is measured
    // from the pixels alone: the centroid of the changed pixels in its cell, in read_pixels rows (row 0 at the top),
    // must grow, so a fall drawn upward fails whatever the replica says.
    CvarScope scope("r.quality", "medium");
    Fixture3D f;
    LensRain rain = lens_rain_setting();
    rain.fall_speed = 1.0f;
    const f32 t0 = 40.0f;
    const std::vector<Drop> drops = cpu_drops(rain, t0, 1);
    const Drop* pick = nullptr;
    for (const Drop& d : drops) {
        if (d.n < 0.5f || d.phase < 0.15f || d.phase > 0.45f || d.e < 0.05f || d.r < 4.0f * kPx) continue;
        if (!on_screen(d, 2.0f * kPx) || d.cell0.x < 0.0f || d.cell0.x + d.cell > kAspect) continue;
        if (!pick || d.r > pick->r) pick = &d;
    }
    REQUIRE(pick != nullptr);
    const f32 t1 = t0 + 0.35f * 600.0f / pick->n;  // a third of a cycle on: the same cycle, phase + 0.35
    const std::vector<Drop> later = cpu_drops(rain, t1, 1);
    const Drop* moved = nullptr;
    for (const Drop& d : later)
        if (d.cell0.x == pick->cell0.x && d.cell0.y == pick->cell0.y) moved = &d;
    REQUIRE(moved != nullptr);
    REQUIRE(moved->cyc == pick->cyc);
    const Shot plain = draw(f, nullptr, 0.0f, Backdrop::Gradient);
    auto centroid_row = [&](const Shot& s) {
        f64 sum = 0.0;
        u32 n = 0;
        for (usize p = 0; p < usize(kW) * kH; ++p) {
            const math::vec2 q = pos_of(p);
            if (q.x < pick->cell0.x || q.x > pick->cell0.x + pick->cell || q.y < pick->cell0.y ||
                q.y > pick->cell0.y + pick->cell || same_px(s.px, plain.px, p))
                continue;
            const usize row = p / kW;  // the pixel's row (integer on purpose)
            sum += f64(row);
            ++n;
        }
        CHECK(n > 10);
        return n ? sum / n : 0.0;
    };
    const f64 r0 = centroid_row(draw(f, &rain, t0, Backdrop::Gradient)),
              r1 = centroid_row(draw(f, &rain, t1, Backdrop::Gradient));
    // At fall_speed 1 a drop falls about one view height a minute (times its 0.5..1.5 variation, rounded to whole
    // cycles): over t1 - t0 seconds, (t1 - t0) / 60 view heights, i.e. that many kH rows.
    const f64 expect = f64(0.35f * (pick->top - pick->cell0.y - pick->r)) * kH;
    const f64 nominal = f64(t1 - t0) / 60.0 * kH;
    MESSAGE("drop r " << pick->r * kH << " px, n " << pick->n << ": centroid row " << r0 << " -> " << r1 << " (replica "
                      << expect << " rows, fall_speed alone " << nominal << ")");
    CHECK(r1 - r0 > 0.5 * expect);
    CHECK(r1 - r0 > 1.0);
    CHECK(expect > 0.4 * nominal);  // the replica's speed is fall_speed's within the 0.5..1.5 variation
    CHECK(expect < 1.6 * nominal);
}

TEST_CASE("lens rain: trails lie above their drops, within 0.35 of a radius to either side") {
    Fixture3D f;
    LensRain rain = lens_rain_setting();
    rain.fall_speed = 1.0f;
    rain.amount = 1.0f;
    rain.size = 2.0f;  // trails are a third of a drop wide: big drops give them a few pixels
    rain.trail = 1.0f;
    LensRain dry_trail = rain;
    dry_trail.trail = 0.0f;
    const f32 t = 33.0f;
    const Shot with = draw(f, &rain, t, Backdrop::Gradient);
    const Shot without = draw(f, &dry_trail, t, Backdrop::Gradient);
    const std::vector<Drop> drops = cpu_drops(rain, t, tier_layers());
    const f32 m = 1.5f * kPx;
    u32 changed = 0, stray = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        if (same_px(with.px, without.px, p)) continue;
        ++changed;
        const math::vec2 q = pos_of(p);
        bool ok = false;
        for (const Drop& d : drops)
            ok = ok || (d.n > 0.5f && d.big > 0.5f && q.y > d.c.y - m && q.y < d.top + m &&
                        std::fabs(q.x - d.c.x) < 0.35f * d.r + m);
        stray += ok ? 0u : 1u;
    }
    MESSAGE(changed << " trail pixels, " << stray << " not above a big falling drop");
    CHECK(changed > 15);
    CHECK(stray == 0);
}

TEST_CASE(
    "lens rain: a drop over the horizon shows the far side of its centre, the sampled radius growing to the rim") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr, 0.0f, Backdrop::Split);
    // The split frame: blue above v = 0.5, red below.
    CHECK(blue(plain.px, (kH / 4) * kW + kW / 2));
    CHECK(red(plain.px, (3 * kH / 4) * kW + kW / 2));
    // Refraction 1 (the default) reaches at most 1.25 radii across the centre: big drops (size 2.5), so the part of a
    // disc beyond the sky's soft horizon band is wide enough to judge. Refraction 2 reaches 8 radii: default drops.
    for (const auto& [refraction, size] : {std::pair{1.0f, 2.5f}, std::pair{2.0f, 1.0f}}) {
        INFO("refraction " << refraction);
        LensRain rain = lens_rain_setting();
        rain.refraction = refraction;
        rain.size = size;
        const std::vector<Drop> drops = cpu_drops(rain, 0.0f, tier_layers());

        const Shot wet = draw(f, &rain, 0.0f, Backdrop::Split);
        REQUIRE(wet.stats.lens_rain);
        // A flipped pixel: blue in the plain frame and red now (or red and blue now). For refraction > 2/3 the drop
        // shows centre + d * (1 - 1.5 r) * (1 + 1.5 r s^2), across the centre: red above the horizon comes from below
        // the centre, so the pixel lies above its drop's centre; blue below the horizon, below it. A plain
        // magnification (no mirror) would put every flipped pixel on the other side of its centre. And the sampled
        // radius grows to the rim: walking outward from a flipped pixel along its ray from the centre, every pixel
        // stays flipped (a profile that folds back would show the centre's colour again in the outer ring).
        const f32 inset = 1.5f * kPx;
        u32 flipped = 0, judged = 0, wrong = 0, walked = 0, folds = 0;
        for (usize p = 0; p < usize(kW) * kH; ++p) {
            const bool to_red = blue(plain.px, p) && red(wet.px, p), to_blue = red(plain.px, p) && blue(wet.px, p);
            if (!to_red && !to_blue) continue;
            ++flipped;
            usize k = 0;
            const math::vec2 q = pos_of(p);
            if (discs_at(drops, q, inset, &k) != 1 || drops[k].fade < 0.99f) continue;  // overlaps, soft drops: skip
            if (dist(q, drops[k].c) > drops[k].r - inset) continue;
            ++judged;
            const Drop& d = drops[k];
            const bool above = q.y > d.c.y;
            wrong += (to_red ? above : !above) ? 0u : 1u;
            const f32 s = dist(q, d.c);
            if (s < 1e-4f) continue;
            const math::vec2 dir{(q.x - d.c.x) / s, (q.y - d.c.y) / s};
            for (f32 w = s + kPx; w < d.r - inset; w += kPx) {
                const math::vec2 o{d.c.x + dir.x * w, d.c.y + dir.y * w};
                if (discs_at(drops, o, inset) != 1) break;
                const usize op = pixel_at(o);
                ++walked;
                folds += (to_red ? red(wet.px, op) : blue(wet.px, op)) ? 0u : 1u;
            }
        }
        MESSAGE("refraction " << refraction << ": " << flipped << " pixels show the other side of the horizon, "
                              << judged << " judged, " << wrong << " wrong; " << walked << " walked outward, " << folds
                              << " fold back");
        CHECK(flipped > 10);
        CHECK(judged > 5);
        CHECK(wrong == 0);
        CHECK(folds == 0);
    }

    // Without refraction a drop reads its own pixel (only the rim brightens): no colour crosses the horizon.
    LensRain flat = lens_rain_setting();
    flat.refraction = 0.0f;
    const Shot still = draw(f, &flat, 0.0f, Backdrop::Split);
    REQUIRE(still.stats.lens_rain);
    u32 crossed = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p)
        crossed += ((blue(plain.px, p) && red(still.px, p)) || (red(plain.px, p) && blue(still.px, p))) ? 1u : 0u;
    CHECK(crossed == 0);
    CHECK(changed_px(still.px, plain.px) > 0);  // the rims
}

TEST_CASE("lens rain: a drop that starts to exist fades in instead of popping") {
    // The replica picks a cell whose drop starts to exist between two nearby amounts (its u lies between them); the
    // drop's own pixels (away from every other disc) barely change from just below to just above (appear 0 -> 0.03),
    // and clearly do once the amount is 0.06 past it.
    Fixture3D f;
    LensRain rain = lens_rain_setting();
    rain.amount = 1.0f;
    const std::vector<Drop> all = cpu_drops(rain, 0.0f, tier_layers());
    const Drop* first = nullptr;
    for (const Drop& d : all) {
        const f32 u = 1.0f - d.e;
        if (u < 0.35f || u > 0.75f || d.r < 5.0f * kPx || !on_screen(d, 2.0f * kPx)) continue;
        if (!first || d.r > first->r) first = &d;
    }
    REQUIRE(first != nullptr);
    const f32 u = 1.0f - first->e;
    // Its geometry at the amount the comparison draws it fully (the radius and the x position follow the amount).
    LensRain wetter = rain;
    wetter.amount = u + 0.06f;
    const std::vector<Drop> others = cpu_drops(wetter, 0.0f, tier_layers());
    auto same_cell = [&](const Drop& d) {
        return d.cell == first->cell && d.cell0.x == first->cell0.x && d.cell0.y == first->cell0.y;
    };
    const Drop* pick = nullptr;
    for (const Drop& d : others)
        if (same_cell(d)) pick = &d;
    REQUIRE(pick != nullptr);
    auto shot_at = [&](f32 amount) {
        LensRain r = rain;
        r.amount = amount;
        return draw(f, &r, 0.0f, Backdrop::Gradient);
    };
    const Shot below = shot_at(u - 0.005f), above = shot_at(u + 0.005f), clear = shot_at(u + 0.06f);
    int step_max = 0, clear_max = 0;
    u32 pixels = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const math::vec2 q = pos_of(p);
        if (dist(q, pick->c) > pick->r * 0.9f) continue;
        bool alone = true;  // no other disc (at the larger amount's radii) reaches it
        for (const Drop& d : others) alone = alone && (same_cell(d) || dist(q, d.c) > d.r + 1.5f * kPx);
        if (!alone) continue;
        ++pixels;
        step_max = std::max(step_max, px_diff(below.px, above.px, p));
        clear_max = std::max(clear_max, px_diff(below.px, clear.px, p));
    }
    MESSAGE("drop at u " << u << ", " << pixels << " pixels: max change " << step_max << " levels over 0.01 of amount, "
                         << clear_max << " over 0.065");
    CHECK(pixels > 20);
    CHECK(step_max <= 12);
    CHECK(clear_max > 3 * std::max(step_max, 4));
}

TEST_CASE("lens rain: amount 1 covers a quarter of the view on High, clearly more than 0.3") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr, 0.0f, Backdrop::Gradient);
    LensRain light = lens_rain_setting();
    light.amount = 0.3f;
    LensRain full = lens_rain_setting();
    full.amount = 1.0f;
    const f64 total = f64(kW) * kH;
    const f64 c_light = changed_px(draw(f, &light, 1.0f, Backdrop::Gradient).px, plain.px) / total;
    const f64 c_full = changed_px(draw(f, &full, 1.0f, Backdrop::Gradient).px, plain.px) / total;
    // The replica's disc area at amount 1 (overlaps counted once), for reference.
    const std::vector<Drop> drops = cpu_drops(full, 1.0f, tier_layers());
    u32 inside = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) inside += discs_at(drops, pos_of(p), 0.0f) > 0 ? 1u : 0u;
    MESSAGE("changed share: amount 0.3 " << c_light << ", amount 1 " << c_full << " (" << tier_layers()
                                         << " grids; the replica's discs cover " << inside / total << ")");
    CHECK(c_light > 0.0);
    CHECK(c_full > 2.0 * c_light);
    if (tier_layers() >= 2) CHECK(c_full >= 0.25);
    {
        CvarScope scope("r.quality", "medium");  // one grid
        const f64 c_one = changed_px(draw(f, &full, 1.0f, Backdrop::Gradient).px, plain.px) / total;
        MESSAGE("one grid at amount 1: " << c_one);
        CHECK(c_one >= 0.18);
    }
}

TEST_CASE("lens rain: the glass blur softens a hard edge by a bounded amount and leaves flat areas as they are") {
    Fixture3D f;
    const Shot plain = draw(f, nullptr);
    LensRain glass = lens_rain_setting();
    glass.amount = 1.0f;
    glass.density = 0.0f;  // no drop at all: only the glass between drops
    glass.blur = 1.0f;
    const Shot soft = draw(f, &glass, 1.0f);
    CHECK(soft.stats.lens_rain);
    // The steepest step between neighbours (the box's silhouette on the ground) gets gentler, and so does the sum of
    // all steps.
    auto steps = [](const Array<u8>& px, f64& sum) {
        f64 worst = 0.0;
        sum = 0.0;
        for (usize y = 0; y + 1 < kH; ++y) {
            for (usize x = 0; x + 1 < kW; ++x) {
                const usize p = y * kW + x;
                const f64 g =
                    std::max(std::fabs(luma(px, p + 1) - luma(px, p)), std::fabs(luma(px, p + kW) - luma(px, p)));
                worst = std::max(worst, g);
                sum += g;
            }
        }
        return worst;
    };
    f64 sum_plain = 0.0, sum_soft = 0.0;
    const f64 g_plain = steps(plain.px, sum_plain), g_soft = steps(soft.px, sum_soft);
    // Away from edges (a 5 x 5 neighbourhood within 2 levels in the plain frame: the tent's bilinear taps reach two
    // pixels) the glass changes nothing beyond rounding; at the edges no more than the edge's own contrast.
    int flat_max = 0, any_max = 0;
    u32 flat = 0;
    for (usize y = 2; y + 2 < kH; ++y) {
        for (usize x = 2; x + 2 < kW; ++x) {
            const usize p = y * kW + x;
            int spread = 0;
            for (i32 j = -2; j <= 2; ++j) {
                for (i32 i = -2; i <= 2; ++i) {
                    const usize o = static_cast<usize>(i32(p) + j * i32(kW) + i);
                    for (usize c = 0; c < 3; ++c)
                        spread = std::max(spread, std::abs(int(plain.px[o * 4 + c]) - int(plain.px[p * 4 + c])));
                }
            }
            const int d = px_diff(plain.px, soft.px, p);
            any_max = std::max(any_max, d);
            if (spread > 2) continue;
            ++flat;
            flat_max = std::max(flat_max, d);
        }
    }
    MESSAGE("steepest step " << g_plain << " -> " << g_soft << ", summed steps " << sum_plain << " -> " << sum_soft
                             << "; " << flat << " flat pixels change at most " << flat_max << ", any pixel "
                             << any_max);
    CHECK(g_soft < 0.8 * g_plain);
    CHECK(sum_soft < sum_plain);
    CHECK(flat > 1000);
    CHECK(flat_max <= 2);
    CHECK(any_max < int(g_plain) + 1);
}

TEST_CASE("lens rain: with TAA the next frame's history holds no drops") {
    LensRain rain = lens_rain_setting();
    rain.amount = 1.0f;
    rain.blur = 1.0f;
    LensRain dry = rain;
    dry.amount = 0.0f;
    auto two_frames = [&](bool rain_first) {
        Fixture3D f;
        const auto taa = [](Scene3D& s) {
            PostFx post = s.post();
            post.taa = true;
            s.set_post(post);
        };
        const Shot first = draw(f, rain_first ? &rain : nullptr, 0.0f, Backdrop::Box, taa);
        CHECK(first.stats.taa);
        CHECK(first.stats.lens_rain == rain_first);
        const Shot second = draw(f, &dry, 0.0f, Backdrop::Box, taa);
        CHECK(second.stats.taa_history);
        CHECK_FALSE(second.stats.lens_rain);
        return second.px;
    };
    const Array<u8> after_rain = two_frames(true);
    const Array<u8> after_plain = two_frames(false);
    CHECK(same(after_rain, after_plain));
}

TEST_CASE("lens rain: the Low tier and r.lens_rain 0 record nothing; Medium draws one drop grid, High two") {
    LensRain rain = lens_rain_setting();
    rain.blur = 0.5f;
    // apply_quality clears `enabled` on Low, keeps it on Medium and High.
    ShadowSettings shadows;
    PostFx low;
    low.rain = rain;
    apply_quality(quality_settings(QualityTier::Low), shadows, low);
    CHECK_FALSE(low.rain.enabled);
    PostFx medium;
    medium.rain = rain;
    apply_quality(quality_settings(QualityTier::Medium), shadows, medium);
    CHECK(medium.rain.enabled);
    CHECK(quality_settings(QualityTier::Low).lens_rain_layers == 0);
    CHECK(quality_settings(QualityTier::Medium).lens_rain_layers == 1);
    CHECK(quality_settings(QualityTier::High).lens_rain_layers == 2);

    // The renderer reads the tier and the cvar itself too: a scene that skipped apply_quality still draws nothing.
    Fixture3D f;
    for (const auto& [name, value] : {std::pair{"r.quality", "low"}, std::pair{"r.lens_rain", "0"}}) {
        CvarScope scope(name, value);
        const Shot plain = draw(f, nullptr);
        const Shot wet = draw(f, &rain, 1.0f);
        CHECK_FALSE(wet.stats.lens_rain);
        CHECK(same(wet.px, plain.px));
    }
    // With blur 0 only the drops change pixels: Medium's are all inside first-grid discs, High adds the second grid's.
    LensRain sharp = rain;
    sharp.blur = 0.0f;
    u32 n_tier[2] = {};
    for (u32 t = 0; t < 2; ++t) {
        CvarScope scope("r.quality", t == 0 ? "medium" : "high");
        const Shot plain = draw(f, nullptr);
        const Shot m = draw(f, &sharp, 1.0f);
        CHECK(m.stats.lens_rain);
        const std::vector<Drop> drops = cpu_drops(sharp, 1.0f, t + 1);
        u32 outside = 0;
        for (usize p = 0; p < usize(kW) * kH; ++p) {
            if (same_px(m.px, plain.px, p)) continue;
            ++n_tier[t];
            outside += discs_at(drops, pos_of(p), 1.5f * kPx) == 0 ? 1u : 0u;
        }
        CHECK(outside == 0);
    }
    MESSAGE("changed pixels: Medium " << n_tier[0] << ", High " << n_tier[1]);
    CHECK(n_tier[0] > 0);
    CHECK(n_tier[1] > n_tier[0]);
}

TEST_CASE("lens rain: two views of different sizes each match the view alone; a render-texture view has no drops") {
    // post_fx_target's pair is per view slot: a small view recorded between two frames of a big one (or before it in
    // the same command buffer) never recreates the big one's target. A linear_output view (a render-to-texture camera)
    // records no drops (out of the milestone's scope).
    Fixture3D f;
    constexpr u32 kSmallW = 96, kSmallH = 72;
    rhi::TextureDesc td;
    td.size = {kSmallW, kSmallH};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.small";
    const rhi::TextureHandle small = f.device->create_texture(td);
    const LensRain rain = lens_rain_setting();
    auto record = [&](rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, u32 slot, bool first,
                      bool linear = false) {
        const Camera3D cam = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(cam, size);
        DirectionalLight sun;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        scene.set_light(sun);
        ShadowSettings ss;
        ss.resolution = 1024;
        scene.set_shadows(ss);
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance box;
        box.mesh = 1;
        scene.add(box);
        lens_rain_scene(scene, &rain, 1.0f);
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = slot;
        out.first = first;
        out.linear_output = linear;
        f.renderer.record_3d(cmd, scene, target, out);
        CHECK(f.renderer.forward_3d().last_stats().lens_rain == !linear);
    };
    Array<u8> big_alone, small_alone, big, small_px;
    big_alone.resize(usize(kW) * kH * 4);
    big.resize(usize(kW) * kH * 4);
    small_alone.resize(usize(kSmallW) * kSmallH * 4);
    small_px.resize(usize(kSmallW) * kSmallH * 4);
    for (int k = 0; k < 2; ++k) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        if (k == 0)
            record(cmd, f.target, {kW, kH}, 1, true);
        else
            record(cmd, small, {kSmallW, kSmallH}, 2, true);
        f.device->submit(cmd);
        if (k == 0)
            REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big_alone.span()).has_value());
        else
            REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_alone.span()).has_value());
        f.device->end_frame();
    }
    for (int frame = 0; frame < 3; ++frame) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        const bool big_first = frame != 1;
        if (big_first) {
            record(cmd, f.target, {kW, kH}, 3, true);
            record(cmd, small, {kSmallW, kSmallH}, 4, false);
        } else {
            record(cmd, small, {kSmallW, kSmallH}, 4, true);
            record(cmd, f.target, {kW, kH}, 3, false);
        }
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big.span()).has_value());
        REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_px.span()).has_value());
        f.device->end_frame();
        INFO("frame " << frame << (big_first ? ", big view first" : ", small view first"));
        CHECK(f.device->validation_error_count() == 0);
        CHECK(same(big, big_alone));
        CHECK(same(small_px, small_alone));
    }
    // The render-texture view: recorded with linear_output, no lens rain.
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        record(cmd, small, {kSmallW, kSmallH}, 5, true, true);
        f.device->submit(cmd);
    }
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_texture(small);
}
