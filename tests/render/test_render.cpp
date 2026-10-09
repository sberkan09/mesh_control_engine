// mce_render CPU-side tests: sort keys + radix sort, Camera2D/pixel snap, flipbook events, text
// layout, tilemap bake + chunk culling, and the batch builder's grouping (FR-R2D-010's "one MDI
// per (blend × sprite set)"). No GPU: the batch is built and inspected as data.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/core.h"
#include "mce/render/camera2d.h"
#include "mce/render/sort_key.h"
#include "mce/render/sprite_animation.h"
#include "mce/render/sprite_batch.h"
#include "mce/render/text.h"

using namespace mce;
using namespace mce::render;
using math::vec2;

// ---- sort keys -------------------------------------------------------------------------------

TEST_CASE("render: 2D sort key field round-trip and precedence") {
    const u64 k = sort_key_2d(PassId::World2D, -3, 0x1234, -77);
    CHECK(sort_key_pass(k) == PassId::World2D);
    CHECK(sort_key_layer(k) == -3);
    CHECK(sort_key_material(k) == 0x1234);
    CHECK(sort_key_order(k) == -77);

    // Layer dominates material, material dominates order; signed values order correctly.
    CHECK(sort_key_2d(PassId::World2D, -1, 0xFFFF, 1000) < sort_key_2d(PassId::World2D, 0, 0, -1000));
    CHECK(sort_key_2d(PassId::World2D, 0, 1, -5) < sort_key_2d(PassId::World2D, 0, 2, -9));
    CHECK(sort_key_2d(PassId::World2D, 0, 1, -5) < sort_key_2d(PassId::World2D, 0, 1, 4));
    CHECK(sort_key_2d(PassId::World2D, 32767, 0, 0) < sort_key_2d(PassId::Screen2D, -32768, 0, 0));
    // Orders beyond 24 bits clamp instead of wrapping.
    CHECK(sort_key_order(sort_key_2d(PassId::World2D, 0, 0, 1 << 30)) == (1 << 23) - 1);
    CHECK(sort_key_order(sort_key_2d(PassId::World2D, 0, 0, -(1 << 30))) == -(1 << 23));
}

TEST_CASE("render: radix sort is ordered and stable") {
    mem::LinearArena arena(mem::heap(), 1 * MiB);
    u64 state = 12345;
    auto rnd = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 33;
    };
    Array<u64> keys;
    Array<u32> vals;
    for (u32 i = 0; i < 5000; ++i) {
        keys.push_back(sort_key_2d(PassId::World2D, static_cast<i16>(rnd() % 7) - 3, static_cast<u16>(rnd() % 4),
                                   static_cast<i32>(rnd() % 50) - 25));
        vals.push_back(i);
    }
    Array<u64> orig = keys;
    radix_sort(keys.span(), vals.span(), arena);
    for (usize i = 1; i < keys.size(); ++i) {
        REQUIRE(keys[i - 1] <= keys[i]);
        if (keys[i - 1] == keys[i]) REQUIRE(vals[i - 1] < vals[i]);  // stability: submission order breaks ties
    }
    for (usize i = 0; i < keys.size(); ++i) REQUIRE(orig[vals[i]] == keys[i]);

    // Degenerate inputs.
    u64 one = 7;
    u32 one_v = 0;
    radix_sort(Span<u64>(&one, 1), Span<u32>(&one_v, 1), arena);
    CHECK(one == 7);
    radix_sort({}, {}, arena);
}

// ---- camera ----------------------------------------------------------------------------------

namespace {
// Screen-space x (pixels from the left edge) of world point p under view v.
f32 screen_x(const View2D& v, vec2 p) {
    const math::vec4 c = v.view_proj * math::vec4(p.x, p.y, 0.0f, 1.0f);
    return (c.x * 0.5f + 0.5f) * static_cast<f32>(v.viewport.width);
}
f32 screen_y(const View2D& v, vec2 p) {
    const math::vec4 c = v.view_proj * math::vec4(p.x, p.y, 0.0f, 1.0f);
    return (c.y * 0.5f + 0.5f) * static_cast<f32>(v.viewport.height);
}
bool on_pixel_edge(f32 px) { return std::fabs(px - std::round(px)) < 1e-3f; }
}  // namespace

TEST_CASE("render: Camera2D view maps meters to pixels") {
    Camera2D cam;
    cam.position = vec2(10, 5);
    cam.pixel_snap = false;
    const View2D v = compute_view_2d(cam, {1920, 1080}, 48.0f);
    CHECK(v.pixels_per_meter == doctest::Approx(48.0f));
    CHECK(screen_x(v, vec2(10, 5)) == doctest::Approx(960.0f));
    CHECK(screen_x(v, vec2(11, 5)) == doctest::Approx(1008.0f));
    CHECK(screen_y(v, vec2(10, 6)) == doctest::Approx(540.0f + 48.0f));  // Y up
    CHECK(v.world_bounds.min.x == doctest::Approx(10.0f - 20.0f));
    CHECK(v.world_bounds.max.y == doctest::Approx(5.0f + 11.25f));  // 1080 px / 48 = 22.5 m tall

    cam.zoom = 2.0f;
    const View2D z = compute_view_2d(cam, {1920, 1080}, 48.0f);
    CHECK(screen_x(z, vec2(11, 5)) == doctest::Approx(960.0f + 96.0f));
}

TEST_CASE("render: pixel snap lands texel edges on pixel edges (FR-R2D-040)") {
    const f32 ppm = 16.0f;  // one texel = 1/16 m
    for (rhi::Extent2D vp : {rhi::Extent2D{640, 360}, rhi::Extent2D{641, 361}}) {
        for (f32 zoom : {1.0f, 2.0f, 2.6f, 0.5f}) {
            for (f32 x = 0.0f; x < 2.0f; x += 0.0137f) {
                Camera2D cam;
                cam.position = vec2(x, 0.3f * x);
                cam.zoom = zoom;
                const View2D v = compute_view_2d(cam, vp, ppm);
                // Integer (or 1/integer) device pixels per texel after quantization.
                const f32 q = v.effective_zoom >= 1.0f ? v.effective_zoom : 1.0f / v.effective_zoom;
                REQUIRE(std::fabs(q - std::round(q)) < 1e-5f);
                // Any texel edge (multiple of 1/ppm m) must project onto a pixel edge; zoomed out,
                // only every 1/zoom-th texel edge can, so test on the coarser of the two grids.
                const f32 grid = math::max(1.0f / ppm, 1.0f / v.pixels_per_meter);
                const f32 ex = std::round(x / grid) * grid + 3.0f * grid;
                const f32 ey = std::round(0.3f * x / grid) * grid;
                REQUIRE(on_pixel_edge(screen_x(v, vec2(ex, 0))));
                REQUIRE(on_pixel_edge(screen_y(v, vec2(0, ey))));
            }
        }
    }
}

TEST_CASE("render: screen view is top-left origin, y down") {
    const View2D v = compute_screen_view({800, 600});
    CHECK(screen_x(v, vec2(0, 0)) == doctest::Approx(0.0f));
    CHECK(screen_y(v, vec2(0, 0)) == doctest::Approx(600.0f));  // GL y is bottom-up; pixel row 0 is the top
    CHECK(screen_y(v, vec2(0, 600)) == doctest::Approx(0.0f));
}

TEST_CASE("render: screen shake is deterministic and decays") {
    ScreenShake a(42), b(42), c(43);
    a.add_trauma(1.0f);
    b.add_trauma(1.0f);
    c.add_trauma(1.0f);
    bool differs = false;
    for (int i = 0; i < 30; ++i) {
        a.tick(1.0f / 60.0f);
        b.tick(1.0f / 60.0f);
        c.tick(1.0f / 60.0f);
        REQUIRE(a.offset().translation.x == b.offset().translation.x);
        REQUIRE(a.offset().rotation == b.offset().rotation);
        differs = differs || a.offset().translation.x != c.offset().translation.x;
        REQUIRE(std::fabs(a.offset().translation.x) <= a.max_translation + 1e-6f);
    }
    CHECK(differs);
    for (int i = 0; i < 120; ++i) a.tick(1.0f / 60.0f);
    CHECK(a.trauma() == 0.0f);
    CHECK(a.offset().translation.x == 0.0f);
}

// ---- flipbook --------------------------------------------------------------------------------

TEST_CASE("render: flipbook frame events fire on the authored frame (FR-R2D-030)") {
    static constexpr u32 kFrames[] = {10, 11, 12, 13, 14, 15};
    static constexpr AnimEvent kEvents[] = {{2, 0xF007}, {5, 0xF008}};
    SpriteClip clip;
    clip.frames = kFrames;
    clip.fps = 12.0f;  // 5 ticks per frame at 60 Hz
    clip.events = kEvents;

    SpriteAnimState st;
    Array<u32> ticks_of_footstep;
    for (u32 tick = 1; tick <= 120; ++tick) {
        advance(clip, st, 1.0 / 60.0, [&](const AnimEvent& e) {
            if (e.id == 0xF007) ticks_of_footstep.push_back(tick);
        });
    }
    // Frame 2 starts at t = 2/12 s = tick 10, then every 6 frames (30 ticks).
    REQUIRE(ticks_of_footstep.size() == 4);
    CHECK(ticks_of_footstep[0] == 10);
    CHECK(ticks_of_footstep[1] == 40);
    CHECK(ticks_of_footstep[3] == 100);
    CHECK(current_region(clip, st) == kFrames[st.frame]);

    // Ping-pong walks 0..5..0 and Once clamps + reports finished.
    clip.mode = PlayMode::PingPong;
    CHECK(clip_frame_at(clip, 5.0 / 12.0) == 5);
    CHECK(clip_frame_at(clip, 6.0 / 12.0) == 4);
    CHECK(clip_frame_at(clip, 10.0 / 12.0) == 0);
    CHECK(clip_frame_at(clip, 11.0 / 12.0) == 1);
    clip.mode = PlayMode::Once;
    bool finished = false;
    CHECK(clip_frame_at(clip, 100.0, &finished) == 5);
    CHECK(finished);
    CHECK(clip_frame_at(clip, 5.5 / 12.0, &finished) == 5);
    CHECK_FALSE(finished);

    // A Once clip fires its last-frame event exactly once.
    SpriteAnimState once;
    u32 last_events = 0;
    for (int i = 0; i < 200; ++i)
        advance(clip, once, 1.0 / 60.0, [&](const AnimEvent& e) { last_events += e.id == 0xF008 ? 1u : 0u; });
    CHECK(last_events == 1);
    CHECK(once.finished);
}

// ---- text ------------------------------------------------------------------------------------

namespace {
// A synthetic monospace-ish font: every glyph 8 px wide, advance 10, baked at 16 px.
struct TestFont {
    Array<FontGlyph> glyphs;
    Array<FontKern> kerning;
    FontMetrics metrics;
    TestFont() {
        metrics.pixel_height = 16;
        metrics.ascent = 12;
        metrics.descent = -4;
        metrics.line_gap = 2;
        metrics.atlas_width = 256;
        metrics.atlas_height = 256;
        for (u32 cp = 32; cp < 127; ++cp) {
            FontGlyph g{};
            g.codepoint = cp;
            const u32 cell = cp - 32;
            g.x0 = static_cast<u16>((cell % 16) * 16);
            g.y0 = static_cast<u16>((cell / 16) * 16);
            g.x1 = static_cast<u16>(cp == ' ' ? g.x0 : g.x0 + 8);
            g.y1 = static_cast<u16>(cp == ' ' ? g.y0 : g.y0 + 12);
            g.xoff = 1;
            g.yoff = -12;
            g.xoff2 = 9;
            g.yoff2 = 0;
            g.xadvance = 10;
            glyphs.push_back(g);
        }
        kerning.push_back(FontKern{'A', 'V', -2.0f});
    }
    FontFace face() const { return FontFace(metrics, glyphs.span(), kerning.span()); }
};
}  // namespace

TEST_CASE("render: utf8 decode") {
    const StringView s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xFFz";
    usize i = 0;
    CHECK(utf8_next(s, i) == 'a');
    CHECK(utf8_next(s, i) == 0xE9);
    CHECK(utf8_next(s, i) == 0x20AC);
    CHECK(utf8_next(s, i) == 0x1F600);
    CHECK(utf8_next(s, i) == 0xFFFD);
    CHECK(utf8_next(s, i) == 'z');
    CHECK(i == s.size());
    const StringView truncated = "\xE2\x82";
    i = 0;
    CHECK(utf8_next(truncated, i) == 0xFFFD);
}

TEST_CASE("render: text layout advances, kerns, wraps and aligns (FR-R2D-060)") {
    TestFont tf;
    const FontFace face = tf.face();
    TextLayout out;
    TextLayoutDesc d;

    layout_text(face, "AV A", d, out);
    REQUIRE(out.quads.size() == 3);  // the space has no quad
    CHECK(out.lines == 1);
    CHECK(out.quads[0].x0 == doctest::Approx(1.0f));
    CHECK(out.quads[1].x0 == doctest::Approx(1.0f + 10.0f - 2.0f));  // kerned pair
    CHECK(out.quads[0].y0 == doctest::Approx(0.0f));                 // ascent 12, yoff -12
    CHECK(out.quads[0].y1 == doctest::Approx(12.0f));
    CHECK(out.width == doctest::Approx(38.0f));
    CHECK(out.quads[0].u0 == doctest::Approx(static_cast<f32>(('A' - 32) % 16 * 16) / 256.0f));

    // Wrap at spaces: "aaa bbb ccc" with room for 7 glyphs -> 2 lines, trailing space swallowed.
    d.max_width = 75.0f;
    layout_text(face, "aaa bbb ccc", d, out);
    CHECK(out.lines == 2);
    REQUIRE(out.quads.size() == 9);
    CHECK(out.quads[6].x0 == doctest::Approx(1.0f));             // 'c' starts line 2
    CHECK(out.quads[6].y0 == doctest::Approx(18.0f));            // line height = 12 + 4 + 2
    CHECK(out.height == doctest::Approx(36.0f));

    // Newlines, alignment, scaling.
    d = {};
    d.align = TextAlign::Right;
    layout_text(face, "ab\nabcd", d, out);
    CHECK(out.lines == 2);
    CHECK(out.quads[0].x0 == doctest::Approx(20.0f + 1.0f));      // 2 glyphs right-aligned in a 40 px box
    d.align = TextAlign::Center;
    layout_text(face, "ab\nabcd", d, out);
    CHECK(out.quads[0].x0 == doctest::Approx(10.0f + 1.0f));
    d = {};
    d.size_px = 32.0f;
    const vec2 m = measure_text(face, "abc", d);
    CHECK(m.x == doctest::Approx(60.0f));
    CHECK(m.y == doctest::Approx(36.0f));

    // Unknown codepoints use the fallback glyph; fallback 0 skips them.
    d = {};
    layout_text(face, "a\xE2\x82\xAC", d, out);
    CHECK(out.quads.size() == 2);
    d.fallback = 0;
    layout_text(face, "a\xE2\x82\xAC", d, out);
    CHECK(out.quads.size() == 1);
    layout_text(face, "", d, out);
    CHECK(out.lines == 0);
    CHECK(out.quads.empty());
}

// ---- batch -----------------------------------------------------------------------------------

namespace {
struct BatchFixture {
    SpriteSets sets;
    SpriteSetId a, b;
    SpriteRegion regions[2];
    View2D view;

    BatchFixture() {
        regions[0] = {0, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 1.0f};
        regions[1] = {3, 0.5f, 0.5f, 1.0f, 1.0f, 2.0f, 1.0f};
        SpriteSetDesc d;
        d.texture_key = 1;
        d.regions = regions;
        a = sets.create(d);
        d.texture_key = 2;
        b = sets.create(d);
        Camera2D cam;
        cam.pixel_snap = false;
        view = compute_view_2d(cam, {480, 480}, 48.0f);  // 10 m × 10 m centered at 0
    }
    SpriteInstance sprite(SpriteSetId set, vec2 pos, i16 layer = 0, i32 order = 0) const {
        SpriteInstance s;
        s.set = set;
        s.position = pos;
        s.sort_layer = layer;
        s.sort_order = order;
        return s;
    }
};
}  // namespace

TEST_CASE("render: sprite quads expand with pivot, flip, rotation and region data") {
    BatchFixture f;
    SpriteBatch batch(f.sets, mem::heap());
    batch.begin(f.view);
    SpriteInstance s = f.sprite(f.b, vec2(1, 2));
    s.region = 1;            // 2 m × 1 m, layer 3
    s.pivot = vec2(0, 0);    // bottom-left
    s.flip_x = true;
    s.tint = math::Color{1, 0, 0, 0.5f};
    batch.add(s);
    batch.build();
    REQUIRE(batch.vertices().size() == 4);
    const SpriteVertex* v = batch.vertices().data();
    CHECK(v[0].x == doctest::Approx(1.0f));
    CHECK(v[0].y == doctest::Approx(2.0f));
    CHECK(v[2].x == doctest::Approx(3.0f));
    CHECK(v[2].y == doctest::Approx(3.0f));
    CHECK(v[0].u == doctest::Approx(1.0f));  // flipped: left edge samples u1
    CHECK(v[1].u == doctest::Approx(0.5f));
    CHECK(v[0].v == doctest::Approx(1.0f));  // bottom samples v1 (v0 is the top row)
    CHECK(v[3].v == doctest::Approx(0.5f));
    CHECK((v[0].packed & 0xFF) == 3);
    CHECK(v[0].color == 0x80'0000FFu);  // sRGB red, alpha 128

    batch.begin(f.view);
    s = f.sprite(f.a, vec2(0, 0));
    s.rotation = math::kPi * 0.5f;
    s.scale = vec2(2, 1);
    batch.add(s);
    batch.build();
    v = batch.vertices().data();
    // 2×1 m around a centered pivot, rotated 90° CCW: bottom-left corner (-1,-0.5) -> (0.5,-1).
    CHECK(v[0].x == doctest::Approx(0.5f));
    CHECK(v[0].y == doctest::Approx(-1.0f));
}

TEST_CASE("render: batch groups by blend × sprite set, one MDI each (FR-R2D-010)") {
    BatchFixture f;
    SpriteBatch batch(f.sets, mem::heap());
    batch.begin(f.view);
    // Interleaved submission of two sets inside one layer: the material field groups them.
    for (int i = 0; i < 100; ++i) batch.add(f.sprite(i % 2 ? f.a : f.b, vec2(0, 0), 0, i));
    batch.build();
    CHECK(batch.stats().quads == 100);
    REQUIRE(batch.groups().size() == 2);
    CHECK(batch.stats().commands == 2);
    CHECK(batch.groups()[0].set == f.a);  // set a has the lower material id
    CHECK(batch.groups()[0].command_count == 1);
    CHECK(batch.draw_args()[0].index_count == 50 * 6);
    CHECK(batch.draw_args()[1].base_vertex == 50 * 4);
    CHECK(batch.segments().size() == 1);

    // Layers dominate: a | b | a across three layers is three groups.
    batch.begin(f.view);
    batch.add(f.sprite(f.a, vec2(0, 0), 2));
    batch.add(f.sprite(f.b, vec2(0, 0), 1));
    batch.add(f.sprite(f.a, vec2(0, 0), 0));
    batch.build();
    REQUIRE(batch.groups().size() == 3);
    CHECK(batch.groups()[0].set == f.a);
    CHECK(batch.groups()[1].set == f.b);

    // Additive is its own pipeline group even with the same set.
    batch.begin(f.view);
    SpriteInstance add = f.sprite(f.a, vec2(0, 0));
    add.blend = SpriteBlend::Additive;
    batch.add(f.sprite(f.a, vec2(0, 0)));
    batch.add(add);
    batch.build();
    REQUIRE(batch.groups().size() == 2);
    CHECK(batch.groups()[1].blend == SpriteBlend::Additive);
}

TEST_CASE("render: batch culls off-screen and invisible sprites, keeps submission order on ties") {
    BatchFixture f;
    SpriteBatch batch(f.sets, mem::heap());
    batch.begin(f.view);
    batch.add(f.sprite(f.a, vec2(100, 0)));  // off-screen
    SpriteInstance hidden = f.sprite(f.a, vec2(0, 0));
    hidden.visible = false;
    batch.add(hidden);
    batch.add(f.sprite(f.a, vec2(-1, 0)));
    batch.add(f.sprite(f.a, vec2(1, 0)));
    batch.add(f.sprite(f.a, vec2(5.4f, 0)));  // straddles the edge: kept
    SpriteInstance bad = f.sprite(SpriteSetId{999}, vec2(0, 0));
    batch.add(bad);
    batch.build();
    CHECK(batch.stats().submitted == 6);
    CHECK(batch.stats().culled == 3);
    REQUIRE(batch.stats().quads == 3);
    CHECK(batch.vertices()[0].x < batch.vertices()[4].x);  // -1 before +1: submission order
}

TEST_CASE("render: long runs split into several commands of one MDI") {
    BatchFixture f;
    SpriteBatch batch(f.sets, mem::heap());
    batch.begin(f.view);
    const u32 n = kMaxQuadsPerDraw + 10;
    for (u32 i = 0; i < n; ++i) batch.add(f.sprite(f.a, vec2(0, 0)));
    batch.build();
    REQUIRE(batch.groups().size() == 1);
    CHECK(batch.groups()[0].command_count == 2);
    CHECK(batch.draw_args()[1].base_vertex == static_cast<i32>(kMaxQuadsPerDraw * 4));
    CHECK(batch.draw_args()[1].index_count == 60);
}

TEST_CASE("render: tilemap bakes into chunks and culls per chunk (FR-R2D-115)") {
    BatchFixture f;
    Array<u32> tiles;
    const u32 w = 100, h = 40;
    tiles.resize(usize(w) * h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) tiles[usize(y) * w + x] = y < 3 || x == 50 ? 1 : 0;  // a floor and a wall
    TilemapDesc d;
    d.set = f.a;
    d.width = w;
    d.height = h;
    d.tile_size = 0.5f;
    d.origin = vec2(-25, -5);
    d.tiles = tiles.span();
    d.sort_layer = -1;
    TilemapBake bake;
    REQUIRE(bake_tilemap(d, f.sets, bake).has_value());
    CHECK(bake.quad_count() == 3 * w + (h - 3));
    // Chunks: 4 across × 2 down; the top row only has the wall's chunk.
    CHECK(bake.chunks.size() == 5);
    const TilemapBake::Chunk& c0 = bake.chunks[0];
    CHECK(c0.bounds.min.x == doctest::Approx(-25.0f));
    CHECK(c0.bounds.max.x == doctest::Approx(-25.0f + 16.0f));
    CHECK(bake.vertices[0].x == doctest::Approx(-25.0f));
    CHECK(bake.vertices[2].y == doctest::Approx(-4.5f));

    // The view spans [-5, 5]²: of the chunk columns [-25,-9] [-9,7] [7,23] [23,25] only the second
    // overlaps, and of the rows [-5,11] [11,15] only the first.
    SpriteBatch batch(f.sets, mem::heap());
    batch.begin(f.view);
    batch.add(f.sprite(f.b, vec2(0, 0), 0));  // above the tilemap layer
    batch.add_tilemap(7, bake);
    batch.build();
    REQUIRE(batch.groups().size() == 2);
    CHECK(batch.groups()[0].source == SpriteBatch::GroupSource::Tilemap);  // layer -1 draws first
    CHECK(batch.groups()[0].tilemap == 7);
    CHECK(batch.groups()[0].command_count == 1);  // chunk (1, 0): floor + lower wall
    CHECK(batch.draw_args()[0].index_count == (32 * 3 + 29) * 6);
    CHECK(batch.groups()[1].source == SpriteBatch::GroupSource::Dynamic);
    CHECK(batch.stats().culled == 4);
    CHECK(batch.stats().tile_quads == 32 * 3 + 29);

    // Bad input is an error, not a crash.
    d.tiles = Span<const u32>(tiles.data(), 10);
    CHECK_FALSE(bake_tilemap(d, f.sets, bake).has_value());
}

TEST_CASE("render: text goes through the batch as coverage quads") {
    BatchFixture f;
    TestFont tf;
    SpriteSetDesc fd;
    fd.texture_key = 9;
    fd.coverage = true;
    Font font{tf.face(), f.sets.create(fd)};
    TextLayout layout;
    layout_text(font.face, "Hi!", {}, layout);

    SpriteBatch batch(f.sets, mem::heap());
    batch.begin(compute_screen_view({800, 600}), PassId::Screen2D);
    TextPlacement p;
    p.origin = vec2(10, 20);
    batch.add_text(font, layout, p);
    batch.build();
    REQUIRE(batch.stats().quads == 3);
    REQUIRE(batch.groups().size() == 1);
    const SpriteVertex* v = batch.vertices().data();
    CHECK((v[0].packed >> 24) == vertex_flags::kCoverage);
    CHECK(v[3].x == doctest::Approx(11.0f));  // top-left: origin + xoff
    CHECK(v[3].y == doctest::Approx(20.0f));
    CHECK(v[0].y == doctest::Approx(32.0f));  // bottom: + glyph height (y down)
}

TEST_CASE("render: batch build is deterministic") {
    BatchFixture f;
    auto run = [&](Array<SpriteVertex>& out) {
        SpriteBatch batch(f.sets, mem::heap());
        batch.begin(f.view);
        u64 s = 99;
        for (int i = 0; i < 2000; ++i) {
            s = s * 6364136223846793005ull + 1;
            SpriteInstance sp = f.sprite((s >> 40) & 1 ? f.a : f.b, vec2(static_cast<f32>((s >> 20) % 10) - 5.0f, 0),
                                         static_cast<i16>((s >> 50) % 3), static_cast<i32>((s >> 30) % 5));
            sp.rotation = static_cast<f32>((s >> 10) % 628) * 0.01f;
            batch.add(sp);
        }
        batch.build();
        out.clear();
        for (const SpriteVertex& v : batch.vertices()) out.push_back(v);
    };
    Array<SpriteVertex> a, b;
    run(a);
    run(b);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(SpriteVertex)) == 0);
}

TEST_CASE("FR-R2D-090: lit quads carry their orientation in the reserved vertex bits") {
    CHECK(pack_orientation(0.0f, false, false) == 0u);
    CHECK(pack_orientation(3.14159265f, false, false) == 8192u);
    CHECK(pack_orientation(-1.57079633f, false, false) == 12288u);  // -90° = 270°
    CHECK(pack_orientation(6.28318531f * 3.0f, true, false) == (1u << 14));
    CHECK(pack_orientation(0.0f, false, true) == (1u << 15));
    const u32 packed = pack_layer_flags(5, vertex_flags::kLit, pack_orientation(3.14159265f, true, true));
    CHECK((packed & 0xFFu) == 5u);
    CHECK(((packed >> 8) & 0x3FFFu) == 8192u);
    CHECK(((packed >> 22) & 3u) == 3u);
    CHECK((packed >> 24) == vertex_flags::kLit);

    // A lit set marks its quads (and only lit sets turn the orientation on).
    SpriteSets sets;
    SpriteRegion r;
    SpriteSetDesc d;
    d.regions = Span<const SpriteRegion>(&r, 1);
    const SpriteSetId plain = sets.create(d);
    d.lit = true;
    d.normal_key = 9;
    const SpriteSetId lit = sets.create(d);
    SpriteBatch b(sets, mem::heap());
    Camera2D cam;
    b.begin(compute_view_2d(cam, {256, 256}, 16.0f));
    SpriteInstance s;
    s.set = plain;
    s.rotation = 1.0f;
    b.add(s);
    s.set = lit;
    s.position = {2, 0};
    b.add(s);
    b.build();
    REQUIRE(b.vertices().size() == 8);
    CHECK(b.vertices()[0].packed == 0u);
    CHECK((b.vertices()[4].packed >> 24) == vertex_flags::kLit);
    CHECK(((b.vertices()[4].packed >> 8) & 0x3FFFu) == pack_orientation(1.0f, false, false));
}
