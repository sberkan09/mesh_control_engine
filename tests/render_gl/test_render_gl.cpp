// mce_render's 2D path against a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe). Each case renders
// into an offscreen target, reads pixels back and probes them: array-atlas sprites in one MDI,
// premultiplied blending, tilemap chunks, coverage text, texture hot reload, mip uploads.

#include <doctest/doctest.h>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi_gl/gl_device.h"

using namespace mce;
using namespace mce::render;
using math::vec2;

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
        auto d = rhi::gl::create_device(*window);
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
