// mce_render's 2D path against a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe). Each case renders
// into an offscreen target, reads pixels back and probes them: array-atlas sprites in one MDI,
// premultiplied blending, tilemap chunks, coverage text, texture hot reload, mip uploads.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"

using namespace mce;
using namespace mce::render;
using math::vec2;
using math::vec3;

namespace {

constexpr u32 kSize = 128;
constexpr f32 kPpm = 16.0f;  // 128 px / 16 = an 8 m × 8 m view centered on the origin

struct Rgba {
    u8 r, g, b, a;
};

struct Fixture {
    Fixture() {
        REQUIRE(platform::init().has_value());
        platform::WindowDesc wd;
        wd.title = "test_render_gl";
        wd.width = 64;
        wd.height = 64;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        REQUIRE(w.has_value());
        window = std::move(w).value();
        auto d = rhi::create_default_device(*window);
        REQUIRE(d.has_value());
        device = std::move(d).value();
        REQUIRE(renderer.init(*device).has_value());
        rhi::TextureDesc td;
        td.size = {kSize, kSize};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "test.target";
        target = device->create_texture(td);
        pixels.resize(usize(kSize) * kSize * 4);
    }
    ~Fixture() {
        device->destroy_texture(target);
        renderer.shutdown();
        device.reset();
        window.reset();
        platform::shutdown();
    }

    // A solid-color array atlas: one 4×4 layer per color.
    u32 solid_atlas(std::initializer_list<Rgba> colors, rhi::TextureFormat fmt = rhi::TextureFormat::RGBA8_SRGB) {
        Array<u8> px;
        for (const Rgba& c : colors)
            for (u32 i = 0; i < 16; ++i) {
                px.push_back(c.r);
                px.push_back(c.g);
                px.push_back(c.b);
                px.push_back(c.a);
            }
        const u32 key = renderer.textures().allocate_key();
        upload(key, 1, px, static_cast<u32>(colors.size()), fmt);
        return key;
    }
    void upload(u32 key, u32 gen, const Array<u8>& px, u32 layers, rhi::TextureFormat fmt) {
        const TextureMipUpload mip{4, 4, std::as_bytes(px.span())};
        TextureUpload up;
        up.format = fmt;
        up.layers = layers;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        REQUIRE(renderer.textures().upload(key, gen, up).has_value());
    }

    // Renders one frame of `fill(batch)` and reads the target back.
    template <class Fill>
    void render(Fill&& fill, const View2D& view) {
        device->begin_frame(frame++);
        SpriteBatch batch(renderer.sprite_sets(), mem::heap());
        batch.begin(view);
        fill(batch);
        batch.build();
        stats = batch.stats();
        rhi::PassAttachment color;
        color.texture = target;
        color.clear.color[0] = color.clear.color[1] = color.clear.color[2] = 0.0f;
        color.clear.color[3] = 1.0f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "test.2d";
        rhi::CommandBuffer cmd;
        cmd.begin_pass(pass);
        renderer.record_2d(cmd, batch);
        cmd.end_pass();
        device->submit(cmd);
        REQUIRE(device->read_pixels(target, rhi::Rect2D{0, 0, kSize, kSize}, pixels.span()).has_value());
        device->end_frame();
        gpu = device->last_frame_stats();
    }
    template <class Fill>
    void render(Fill&& fill) {
        Camera2D cam;
        render(fill, compute_view_2d(cam, {kSize, kSize}, kPpm));
    }

    // Pixel at world position p (meters) for the default view.
    Rgba at_world(vec2 p) const {
        const u32 x = static_cast<u32>(p.x * kPpm + kSize / 2.0f);
        const u32 y = static_cast<u32>(kSize / 2.0f - p.y * kPpm);  // read_pixels is top-row first
        return at_px(x, y);
    }
    Rgba at_px(u32 x, u32 y) const {
        const usize i = (usize(y) * kSize + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }

    SpriteInstance sprite(SpriteSetId set, vec2 pos, u32 region = 0) const {
        SpriteInstance s;
        s.set = set;
        s.region = region;
        s.position = pos;
        return s;
    }

    mem::UniquePtr<platform::Window> window;
    mem::UniquePtr<rhi::RenderDevice> device;
    Renderer renderer;
    rhi::TextureHandle target;
    Array<u8> pixels;
    BatchStats stats;
    rhi::FrameStats gpu;
    u64 frame = 0;
};

bool near(u8 a, u8 b, u8 tol = 3) { return (a > b ? a - b : b - a) <= tol; }

}  // namespace

TEST_CASE("render_gl: sprites from two atlas layers draw in one MDI") {
    Fixture f;
    const u32 key = f.solid_atlas({{255, 0, 0, 255}, {0, 255, 0, 255}});
    const SpriteRegion regions[] = {{0, 0, 0, 1, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1}};
    SpriteSetDesc sd;
    sd.texture_key = key;
    sd.regions = regions;
    const SpriteSetId set = f.renderer.create_sprite_set(sd);

    f.render([&](SpriteBatch& b) {
        b.add(f.sprite(set, vec2(-2, 0), 0));
        b.add(f.sprite(set, vec2(2, 0), 1));
        b.add(f.sprite(set, vec2(100, 0), 1));  // culled
    });
    const Rgba red = f.at_world(vec2(-2, 0));
    const Rgba green = f.at_world(vec2(2, 0));
    const Rgba bg = f.at_world(vec2(0, 0));
    CHECK(red.r == 255);
    CHECK(red.g == 0);
    CHECK(green.g == 255);
    CHECK(green.r == 0);
    CHECK(bg.r == 0);
    CHECK(bg.g == 0);
    // Sprite edges land on pixel edges: 1 m = 16 px exactly, centered at -2 m -> px [24, 40).
    CHECK(f.at_px(24, 64).r == 255);
    CHECK(f.at_px(23, 64).r == 0);
    CHECK(f.at_px(39, 64).r == 255);
    CHECK(f.at_px(40, 64).r == 0);
    CHECK(f.stats.culled == 1);
    CHECK(f.gpu.indirect_draws == 1);  // FR-R2D-010: one MDI for blend × sprite set
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: premultiplied alpha and additive blending") {
    Fixture f;
    const u32 key = f.solid_atlas({{255, 255, 255, 255}});
    SpriteSetDesc sd;
    sd.texture_key = key;
    const SpriteSetId set = f.renderer.create_sprite_set(sd);
    f.render([&](SpriteBatch& b) {
        SpriteInstance s = f.sprite(set, vec2(-2, 0));
        s.tint = math::Color{1, 1, 1, 0.5f};
        b.add(s);
        SpriteInstance a = f.sprite(set, vec2(2, 0));
        a.tint = math::Color{0.214f, 0, 0, 1};  // linear 0.214 ≈ sRGB 128
        a.blend = SpriteBlend::Additive;
        b.add(a);
        SpriteInstance a2 = a;
        a2.sort_order = 1;
        b.add(a2);
    });
    const Rgba half = f.at_world(vec2(-2, 0));
    CHECK(near(half.r, 128));  // 50 % white over black
    const Rgba add = f.at_world(vec2(2, 0));
    CHECK(near(add.r, 255, 4));  // two additive 128s
    CHECK(f.gpu.indirect_draws == 2);  // alpha group + additive group
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: tilemap chunks draw from their persistent buffer and interleave with sprites") {
    Fixture f;
    const u32 key = f.solid_atlas({{0, 0, 255, 255}, {255, 255, 0, 255}});
    const SpriteRegion regions[] = {{0, 0, 0, 1, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1}};
    SpriteSetDesc sd;
    sd.texture_key = key;
    sd.regions = regions;
    const SpriteSetId set = f.renderer.create_sprite_set(sd);

    Array<u32> tiles;
    const u32 w = 80, h = 8;
    tiles.resize(usize(w) * h);
    for (u32 x = 0; x < w; ++x) tiles[x] = 1;          // a blue floor, row 0
    tiles[usize(3) * w + 40] = 2;                      // one yellow tile
    TilemapDesc td;
    td.set = set;
    td.width = w;
    td.height = h;
    td.tile_size = 1.0f;
    td.origin = vec2(-40, -4);
    td.tiles = tiles.span();
    td.sort_layer = -1;
    auto tm = f.renderer.create_tilemap(td);
    REQUIRE(tm.has_value());

    f.render([&](SpriteBatch& b) {
        f.renderer.add_tilemap(b, tm.value());
        SpriteInstance s = f.sprite(set, vec2(-3.5f, -3.5f), 1);  // yellow sprite over the floor
        b.add(s);
    });
    CHECK(f.at_world(vec2(-2.5f, -3.5f)).b == 255);   // floor
    CHECK(f.at_world(vec2(0.5f, -0.5f)).r == 255);    // tile (40, 3) at x 0..1, y -1..0
    CHECK(f.at_world(vec2(0.5f, -0.5f)).b == 0);
    CHECK(f.at_world(vec2(-3.5f, -3.5f)).r == 255);   // sprite on layer 1 covers the floor
    CHECK(f.at_world(vec2(-3.5f, -3.5f)).b == 0);
    CHECK(f.stats.tile_quads > 0);
    CHECK(f.stats.tile_quads < w);  // chunks outside the view are culled
    CHECK(f.gpu.indirect_draws == 2);

    // Editor paint: re-bake, and the change shows the next frame.
    tiles[usize(3) * w + 40] = 1;
    REQUIRE(f.renderer.update_tilemap(tm.value(), td).has_value());
    f.render([&](SpriteBatch& b) { f.renderer.add_tilemap(b, tm.value()); });
    CHECK(f.at_world(vec2(0.5f, -0.5f)).b == 255);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: a tilemap offset (parallax) translates the baked layer without a re-bake") {
    Fixture f;
    const u32 key = f.solid_atlas({{0, 255, 0, 255}});
    const SpriteRegion region{0, 0, 0, 1, 1, 1, 1};
    SpriteSetDesc sd;
    sd.texture_key = key;
    sd.regions = Span<const SpriteRegion>(&region, 1);
    const SpriteSetId set = f.renderer.create_sprite_set(sd);
    const u32 tiles[1] = {1};
    TilemapDesc td;
    td.set = set;
    td.width = td.height = 1;
    td.origin = vec2(0, 0);
    td.tiles = tiles;
    auto tm = f.renderer.create_tilemap(td);
    REQUIRE(tm.has_value());

    f.render([&](SpriteBatch& b) {
        f.renderer.add_tilemap(b, tm.value(), vec2(-3, 1));
        b.add(f.sprite(set, vec2(2.5f, -2.5f)));  // a dynamic sprite after it draws untranslated
    });
    CHECK(f.at_world(vec2(-2.5f, 1.5f)).g == 255);
    CHECK(f.at_world(vec2(0.5f, 0.5f)).g == 0);
    CHECK(f.at_world(vec2(2.5f, -2.5f)).g == 255);
    // Culling uses the shifted bounds: far off-screen the layer is not drawn at all.
    f.render([&](SpriteBatch& b) { f.renderer.add_tilemap(b, tm.value(), vec2(100, 0)); });
    CHECK(f.stats.tile_quads == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FR-R2D-090: lit sprites shade by their normal map, rotation and flip; unlit ones ignore lights") {
    Fixture f;
    // Color: white. Normals: layer 0 flat (+Z), layer 1 tilted 60° toward +X. Linear, not sRGB.
    const u32 color = f.solid_atlas({{255, 255, 255, 255}, {255, 255, 255, 255}});
    const u32 normals = f.solid_atlas({{128, 128, 255, 255}, {238, 128, 191, 255}}, rhi::TextureFormat::RGBA8_UNorm);
    const SpriteRegion regions[] = {{0, 0, 0, 1, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1}};
    SpriteSetDesc sd;
    sd.texture_key = color;
    sd.regions = regions;
    const SpriteSetId unlit = f.renderer.create_sprite_set(sd);
    sd.lit = true;
    sd.normal_key = normals;
    const SpriteSetId lit = f.renderer.create_sprite_set(sd);

    // Default lights (white ambient, no points): lit draws exactly like unlit.
    f.render([&](SpriteBatch& b) {
        b.add(f.sprite(lit, vec2(-1.5f, 0)));
        b.add(f.sprite(unlit, vec2(1.5f, 0)));
    });
    CHECK(f.at_world(vec2(-1.5f, 0)).r == 255);
    CHECK(f.at_world(vec2(1.5f, 0)).r == 255);

    Lights2D lights;
    lights.ambient = math::Color{0.0f, 0.0f, 0.0f, 1.0f};
    PointLight2D above;
    above.position = vec2(-1.5f, 0);
    above.height = 1.0f;
    above.radius = 3.0f;
    above.color = math::Color{1.0f, 0.5f, 0.0f, 1.0f};
    lights.add(above);
    f.render([&](SpriteBatch& b) {
        b.set_lights(lights);
        b.add(f.sprite(lit, vec2(-1.5f, 0)));
        b.add(f.sprite(unlit, vec2(1.5f, 0)));
        b.add(f.sprite(lit, vec2(3.4f, 3.4f)));  // beyond the radius: ambient only (black)
    });
    const Rgba under = f.at_world(vec2(-1.5f, 0));  // N·L = 1 at the light's foot, falloff 1
    CHECK(under.r >= 250);
    CHECK(under.g > 170);  // 0.5 linear -> ~188 sRGB, minus the texel-center falloff
    CHECK(under.b == 0);
    CHECK(f.at_world(vec2(1.5f, 0)).r == 255);  // unlit: untouched
    CHECK(f.at_world(vec2(3.4f, 3.4f)).r == 0);

    // A normal tilted toward +X is lit by a light to its right and dark from the left; rotating
    // the sprite half a turn or flipping it swaps the sides.
    auto brightness = [&](f32 light_x, f32 rotation, bool flip) {
        Lights2D l;
        l.ambient = math::Color{0, 0, 0, 1};
        PointLight2D p;
        p.position = vec2(light_x, 0);
        p.height = 0.5f;
        p.radius = 4.0f;
        l.add(p);
        f.render([&](SpriteBatch& b) {
            b.set_lights(l);
            SpriteInstance s = f.sprite(lit, vec2(0, 0), 1);
            s.rotation = rotation;
            s.flip_x = flip;
            b.add(s);
        });
        return static_cast<int>(f.at_world(vec2(0, 0)).r);
    };
    const int right = brightness(1.0f, 0.0f, false), left = brightness(-1.0f, 0.0f, false);
    CHECK(right > left + 60);
    CHECK(brightness(-1.0f, 3.14159265f, false) > brightness(1.0f, 3.14159265f, false) + 60);
    CHECK(brightness(-1.0f, 0.0f, true) > brightness(1.0f, 0.0f, true) + 60);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FR-R2D-100: 2D post — copy, vignette, LUT grade and chromatic aberration toggle at runtime") {
    Fixture f;
    const u32 key = f.solid_atlas({{255, 255, 255, 255}, {200, 100, 50, 255}});
    const SpriteRegion regions[] = {{0, 0, 0, 1, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1}};
    SpriteSetDesc sd;
    sd.texture_key = key;
    sd.regions = regions;
    const SpriteSetId set = f.renderer.create_sprite_set(sd);

    rhi::TextureDesc td;
    td.size = {kSize, kSize};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.post_out";
    const rhi::TextureHandle out = f.device->create_texture(td);

    // Luts: identity and inverted, uploaded linear (UNorm): a LUT maps display values.
    Array<u8> lut;
    make_identity_lut(lut);
    const u32 ident = f.renderer.textures().allocate_key();
    const TextureMipUpload mip{kLutSize * kLutSize, kLutSize, std::as_bytes(lut.span())};
    TextureUpload up;
    up.format = rhi::TextureFormat::RGBA8_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    REQUIRE(f.renderer.textures().upload(ident, 1, up).has_value());
    Array<u8> inv = lut;
    for (usize i = 0; i < inv.size(); i += 4)
        for (u32 c = 0; c < 3; ++c) inv[i + c] = static_cast<u8>(255 - inv[i + c]);
    const u32 inverted = f.renderer.textures().allocate_key();
    const TextureMipUpload imip{kLutSize * kLutSize, kLutSize, std::as_bytes(inv.span())};
    up.mips = Span<const TextureMipUpload>(&imip, 1);
    REQUIRE(f.renderer.textures().upload(inverted, 1, up).has_value());

    // The 2D frame: a colored quad filling the view, a white bar in the middle of the left half.
    Array<u8> post_px;
    post_px.resize(usize(kSize) * kSize * 4);
    auto run = [&](const Post2D& post) {
        f.render([&](SpriteBatch& b) {
            SpriteInstance bg = f.sprite(set, vec2(0, 0), 1);
            bg.scale = {8, 8};
            b.add(bg);
            SpriteInstance bar = f.sprite(set, vec2(-2, 0), 0);
            bar.scale = {0.25f, 8};
            bar.sort_order = 1;
            b.add(bar);
        });
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        f.renderer.record_post_2d(cmd, f.target, {kSize, kSize}, post, out);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(out, rhi::Rect2D{0, 0, kSize, kSize}, post_px.span()).has_value());
        f.device->end_frame();
    };
    auto px = [&](u32 x, u32 y) {
        const usize i = (usize(y) * kSize + x) * 4;
        return Rgba{post_px[i], post_px[i + 1], post_px[i + 2], post_px[i + 3]};
    };

    run(Post2D{});  // nothing enabled: a copy
    for (u32 y : {5u, 64u, 120u})
        for (u32 x : {3u, 32u, 64u, 125u}) {
            const Rgba a = f.at_px(x, y), b = px(x, y);
            CHECK(near(a.r, b.r, 1));
            CHECK(near(a.g, b.g, 1));
            CHECK(near(a.b, b.b, 1));
        }
    const Rgba bg = px(100, 64);
    CHECK(near(bg.r, 200, 2));
    CHECK(near(bg.g, 100, 2));

    Post2D v;
    v.vignette = true;
    run(v);
    CHECK(near(px(100, 64).r, 200, 2));  // inside the radius: untouched
    CHECK(px(1, 1).r < 150);             // the corner darkens
    CHECK(px(1, 1).r < px(10, 20).r);

    Post2D g;
    g.color_grade = true;
    g.lut_key = ident;
    run(g);
    CHECK(near(px(100, 64).r, 200, 4));
    CHECK(near(px(100, 64).g, 100, 4));
    CHECK(near(px(100, 64).b, 50, 4));
    g.lut_key = inverted;
    run(g);
    CHECK(near(px(100, 64).r, 55, 4));
    CHECK(near(px(100, 64).g, 155, 4));
    CHECK(near(px(100, 64).b, 205, 4));
    g.lut_strength = 0.5f;
    run(g);
    CHECK(near(px(100, 64).r, 128, 4));

    // Aberration: left of center the red channel is sampled further out (left), blue further in,
    // so the bar's left edge shows red-only pixels and its right edge blue-only ones.
    Post2D ca;
    ca.chromatic_aberration = true;
    ca.aberration = 0.02f;
    run(ca);
    bool red_fringe = false, blue_fringe = false;
    for (u32 x = 20; x < 44; ++x) {
        const Rgba p = px(x, 64);
        if (p.r > 240 && p.b < 100) red_fringe = true;
        if (p.b > 240 && p.r < 240 && p.g < 240) blue_fringe = true;
    }
    CHECK(red_fringe);
    CHECK(blue_fringe);

    // M-ACCESS: the color-vision correction matches the CPU reference on the decoded color.
    const auto decode = [](u8 byte_value) {
        const f32 c = static_cast<f32>(byte_value) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const auto encode = [](f32 l) {
        const f32 c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
        return static_cast<int>(c * 255.0f + 0.5f);
    };
    for (ColorVision cv : {ColorVision::Protanopia, ColorVision::Deuteranopia, ColorVision::Tritanopia}) {
        Post2D c;
        c.color_vision = cv;
        CHECK(c.any());
        run(c);
        const Rgba src = f.at_px(100, 64), got = px(100, 64);
        const vec3 want = correct_color_vision(cv, 1.0f, {decode(src.r), decode(src.g), decode(src.b)});
        INFO(color_vision_name(cv) << ": " << int(got.r) << "," << int(got.g) << "," << int(got.b) << " want " << encode(want.x)
                                   << "," << encode(want.y) << "," << encode(want.z));
        CHECK(std::abs(got.r - encode(want.x)) <= 3);
        CHECK(std::abs(got.g - encode(want.y)) <= 3);
        CHECK(std::abs(got.b - encode(want.z)) <= 3);
        CHECK((got.r != src.r || got.g != src.g || got.b != src.b));  // the orange actually moves
        c.color_vision_strength = 0.0f;  // strength 0: a copy
        run(c);
        CHECK(near(px(100, 64).r, src.r, 1));
        CHECK(near(px(100, 64).g, src.g, 1));
    }
    f.device->destroy_texture(out);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: coverage text renders through the batch") {
    Fixture f;
    // A one-glyph font: 'X' is a solid 8×12 coverage block.
    FontMetrics m;
    m.pixel_height = 16;
    m.ascent = 12;
    m.descent = -4;
    m.atlas_width = 16;
    m.atlas_height = 16;
    Array<u8> atlas;
    atlas.resize(16 * 16);
    for (u32 y = 0; y < 12; ++y)
        for (u32 x = 0; x < 8; ++x) atlas[y * 16 + x] = 255;
    FontGlyph g{};
    g.codepoint = 'X';
    g.x1 = 8;
    g.y1 = 12;
    g.yoff = -12;
    g.xoff2 = 8;
    g.xadvance = 10;
    auto font = f.renderer.create_font(m, Span<const FontGlyph>(&g, 1), {}, std::as_bytes(atlas.span()));
    REQUIRE(font.has_value());

    TextLayout layout;
    TextLayoutDesc ld;
    ld.fallback = 0;
    layout_text(font.value().face, "XX", ld, layout);
    REQUIRE(layout.quads.size() == 2);
    f.render(
        [&](SpriteBatch& b) {
            TextPlacement p;
            p.origin = vec2(20, 30);
            p.tint = math::Color{0, 1, 0, 1};
            b.add_text(font.value(), layout, p);
        },
        compute_screen_view({kSize, kSize}));
    CHECK(f.at_px(24, 36).g == 255);  // inside the first glyph (x 20..28, y 30..42)
    CHECK(f.at_px(24, 36).r == 0);
    CHECK(f.at_px(29, 36).g == 0);    // the gap between the glyphs
    CHECK(f.at_px(31, 36).g == 255);  // second glyph at x 30..38
    CHECK(f.at_px(24, 44).g == 0);    // below the line
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: a distance-field font keeps a hard edge when magnified; coverage blurs (M-UI-DATA 4)") {
    Fixture f;
    // One 12×12 glyph 'X' holding an 8×8 box at (2, 2): as coverage, and as a distance field with
    // a 2-pixel spread (128 on the edge, 64 per pixel). Both are drawn 8× their baked size.
    auto make = [&](bool sdf) {
        FontMetrics m;
        m.pixel_height = 16;
        m.ascent = 12;
        m.descent = -4;
        m.atlas_width = 16;
        m.atlas_height = 16;
        m.sdf_spread = sdf ? 2.0f : 0.0f;
        Array<u8> atlas;
        atlas.resize(16 * 16);
        for (u32 y = 0; y < 12; ++y)
            for (u32 x = 0; x < 12; ++x) {
                const f32 cx = static_cast<f32>(x) + 0.5f, cy = static_cast<f32>(y) + 0.5f;
                const f32 d = std::min(std::min(cx - 2.0f, 10.0f - cx), std::min(cy - 2.0f, 10.0f - cy));  // + inside
                atlas[y * 16 + x] = sdf ? static_cast<u8>(std::clamp(128.0f + d * 64.0f, 0.0f, 255.0f)) : (d > 0 ? 255 : 0);
            }
        FontGlyph g{};
        g.codepoint = 'X';
        g.x1 = 12;
        g.y1 = 12;
        g.yoff = -12;
        g.xoff2 = 12;
        g.xadvance = 12;
        auto font = f.renderer.create_font(m, Span<const FontGlyph>(&g, 1), {}, std::as_bytes(atlas.span()));
        REQUIRE(font.has_value());
        return font.value();
    };
    const Font coverage = make(false), field = make(true);
    // The box spans x 26..90 on screen; count the half-lit pixels across its left edge on the middle row.
    auto ramp = [&](const Font& font) {
        TextLayout layout;
        TextLayoutDesc ld;
        ld.size_px = 128;
        ld.fallback = 0;
        layout_text(font.face, "X", ld, layout);
        REQUIRE(layout.quads.size() == 1);
        f.render(
            [&](SpriteBatch& b) {
                TextPlacement p;
                p.origin = vec2(10, 10);
                p.tint = math::Color{0, 1, 0, 1};
                b.add_text(font, layout, p);
            },
            compute_screen_view({kSize, kSize}));
        CHECK(f.at_px(58, 58).g == 255);
        CHECK(f.at_px(14, 58).g == 0);
        u32 partial = 0;
        for (u32 x = 14; x < 42; ++x) partial += f.at_px(x, 58).g > 16 && f.at_px(x, 58).g < 239;
        return partial;
    };
    const u32 soft = ramp(coverage), sharp = ramp(field);
    CAPTURE(soft);
    CAPTURE(sharp);
    CHECK(soft >= 4);   // bilinear coverage smears the edge over about one baked pixel (8 screen pixels)
    CHECK(sharp <= 2);  // the field is thresholded over one screen pixel
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: texture hot reload swaps generations; missing textures are magenta") {
    Fixture f;
    const u32 key = f.renderer.textures().allocate_key();
    Array<u8> red, blue;
    for (u32 i = 0; i < 16; ++i) {
        const u8 r[4] = {255, 0, 0, 255}, b[4] = {0, 0, 255, 255};
        for (u8 c : r) red.push_back(c);
        for (u8 c : b) blue.push_back(c);
    }
    f.upload(key, 1, red, 1, rhi::TextureFormat::RGBA8_UNorm);
    SpriteSetDesc sd;
    sd.texture_key = key;
    const SpriteSetId set = f.renderer.create_sprite_set(sd);
    sd.texture_key = 0x7777;  // never uploaded
    const SpriteSetId missing = f.renderer.create_sprite_set(sd);

    auto draw = [&](SpriteBatch& b) {
        b.add(f.sprite(set, vec2(-2, 0)));
        b.add(f.sprite(missing, vec2(2, 0)));
    };
    f.render(draw);
    CHECK(f.at_world(vec2(-2, 0)).r == 255);
    const Rgba checker = f.at_world(vec2(2.05f, 0.05f));
    CHECK(((checker.r == 255 && checker.b == 255) || (checker.r == 0 && checker.b == 0)));

    // Reload: generation 2 becomes current while generation 1 is still alive for frames in flight.
    f.upload(key, 2, blue, 1, rhi::TextureFormat::RGBA8_UNorm);
    f.render(draw);
    CHECK(f.at_world(vec2(-2, 0)).b == 255);
    CHECK(f.at_world(vec2(-2, 0)).r == 0);
    f.renderer.textures().retire(key, 1);
    CHECK(f.renderer.textures().contains(key));
    f.render(draw);
    CHECK(f.at_world(vec2(-2, 0)).b == 255);
    f.renderer.textures().retire(key, 2);
    CHECK_FALSE(f.renderer.textures().contains(key));

    // Untrusted upload data is validated, not trusted.
    const TextureMipUpload bad{4, 4, std::as_bytes(red.span().subspan(0, 10))};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&bad, 1);
    CHECK_FALSE(f.renderer.textures().upload(key, 3, up).has_value());
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render_gl: mip chains and array layers upload through update_texture") {
    Fixture f;
    Array<u8> m0, m1, m2;
    m0.resize(4 * 4 * 4 * 2, 200);
    m1.resize(2 * 2 * 4 * 2, 100);
    m2.resize(1 * 1 * 4 * 2, 50);
    const TextureMipUpload mips[] = {{4, 4, std::as_bytes(m0.span())}, {2, 2, std::as_bytes(m1.span())},
                                     {1, 1, std::as_bytes(m2.span())}};
    TextureUpload up;
    up.layers = 2;
    up.mips = mips;
    auto tex = render::create_texture(*f.device, up);
    REQUIRE(tex.has_value());
    const rhi::TextureDesc d = f.device->texture_desc(tex.value());
    CHECK(d.mip_levels == 3);
    CHECK(d.array_layers == 2);
    CHECK(d.dimension == rhi::TextureDimension::Tex2DArray);
    // Wrong mip extents are rejected before touching the device.
    const TextureMipUpload wrong[] = {{4, 4, std::as_bytes(m0.span())}, {4, 4, std::as_bytes(m0.span())}};
    up.mips = wrong;
    CHECK_FALSE(render::create_texture(*f.device, up).has_value());
    f.device->destroy_texture(tex.value());
    CHECK(f.device->validation_error_count() == 0);
}

#if MCE_RENDER_HAS_ASSET
#include "mce/render/asset_upload.h"

TEST_CASE("render_gl: cooked CHNK_TEX and CHNK_FONT payloads upload through the asset sink") {
    Fixture f;
    // A cooked 2-layer sRGB texture with a 2-level mip chain, exactly as the cook writes it.
    Array<u8> mip0, mip1;
    for (u32 layer = 0; layer < 2; ++layer)
        for (u32 i = 0; i < 16; ++i) {
            const u8 px[4] = {u8(layer ? 0 : 255), u8(layer ? 255 : 0), 0, 255};
            for (u8 c : px) mip0.push_back(c);
        }
    for (u32 i = 0; i < 2 * 4; ++i) {
        const u8 px[4] = {128, 128, 128, 255};
        for (u8 c : px) mip1.push_back(c);
    }
    const asset::TextureMipData mips[] = {{4, 4, std::as_bytes(mip0.span())}, {2, 2, std::as_bytes(mip1.span())}};
    Array<byte> blob;
    asset::write_texture(asset::TextureFormat::RGBA8, 2, asset::TextureFlags::Srgb, mips, blob);

    asset::AssetSystem assets;  // no mounts: generation() answers 0 for every key
    AssetUploadSink sink(f.renderer, assets);
    constexpr asset::AssetKey kKey = 5;
    REQUIRE(sink.upload_texture(kKey, blob.span(), asset::TextureFlags::Srgb).has_value());
    const rhi::TextureDesc d = f.device->texture_desc(f.renderer.textures().get(kKey));
    CHECK(d.format == rhi::TextureFormat::RGBA8_SRGB);
    CHECK(d.array_layers == 2);
    CHECK(d.mip_levels == 2);

    const SpriteRegion regions[] = {{0, 0, 0, 1, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1}};
    SpriteSetDesc sd;
    sd.texture_key = kKey;
    sd.regions = regions;
    const SpriteSetId set = f.renderer.create_sprite_set(sd);
    f.render([&](SpriteBatch& b) {
        b.add(f.sprite(set, vec2(-2, 0), 0));
        b.add(f.sprite(set, vec2(2, 0), 1));
    });
    CHECK(f.at_world(vec2(-2, 0)).r == 255);
    CHECK(f.at_world(vec2(2, 0)).g == 255);

    const asset::RetiredAsset retired{kKey, 0, asset::ChunkType::Texture};
    sink.destroy_gpu_objects(Span<const asset::RetiredAsset>(&retired, 1));
    CHECK_FALSE(f.renderer.textures().contains(kKey));

    // Truncated cooked bytes fail cleanly.
    CHECK_FALSE(sink.upload_texture(kKey, blob.span().subspan(0, 40), 0).has_value());

    // Font chunk -> Font.
    asset::FontHeader fh{};
    fh.pixel_height = 16;
    fh.ascent = 12;
    fh.descent = -4;
    fh.atlas_width = 16;
    fh.atlas_height = 16;
    asset::FontGlyph g{};
    g.codepoint = 'X';
    g.x1 = 8;
    g.y1 = 12;
    g.yoff = -12;
    g.xoff2 = 8;
    g.xadvance = 10;
    Array<byte> atlas;
    atlas.resize(256, byte{255});
    Array<byte> font_blob;
    asset::write_font(fh, Span<const asset::FontGlyph>(&g, 1), {}, atlas.span(), font_blob);
    auto font = load_cooked_font(f.renderer, font_blob.span());
    REQUIRE(font.has_value());
    CHECK(font.value().face.find('X') != nullptr);
    CHECK(font.value().face.metrics().ascent == 12.0f);
    CHECK(f.device->validation_error_count() == 0);
}
#endif
