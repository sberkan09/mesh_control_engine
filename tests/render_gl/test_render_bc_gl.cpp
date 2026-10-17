// M-CONTENT 1b-iii against a real driver: the BC1/BC4/BC5/BC7 block formats upload as cooked textures, sample and read back
// within the bounds; unpack_normal shades a BC5 normal map like the same RGBA8 map; a family whose DeviceCaps flag is false
// is refused cleanly and draws the error checker; a BC4 height composed into a BC5 array samples as (h, 0, 0).
// Runs on GL 4.5 and, as test_render_sdlgpu, on the SDL GPU backend (MCE_RHI=sdlgpu). Without a device (no video
// driver) every case reports that and returns, so a headless container passes and says why.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/asset_upload.h"
#include "mce/render/renderer.h"
#include "mce/render/terrain.h"
#include "mce/rhi/default_device.h"

using namespace mce;
using namespace mce::render;
using math::vec2;
using AF = asset::TextureFormat;
using RF = rhi::TextureFormat;

namespace {

constexpr u32 kSize = 128;
constexpr f32 kPpm = 16.0f;

struct Rgba {
    u8 r, g, b, a;
};

// Like test_render_gl's fixture, but a missing device is a skip, not a failure.
struct Gpu {
    Gpu() {
        if (!platform::init().has_value()) return;
        inited = true;
        platform::WindowDesc wd;
        wd.title = "test_render_bc_gl";
        wd.width = 64;
        wd.height = 64;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        if (!w) return;
        window = std::move(w).value();
        auto d = rhi::create_default_device(*window);
        if (!d) return;
        device = std::move(d).value();
        if (!renderer.init(*device).has_value()) return;
        renderer_ok = true;
        rhi::TextureDesc td;
        td.size = {kSize, kSize};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "test.target";
        target = device->create_texture(td);
        pixels.resize(usize(kSize) * kSize * 4);
    }
    ~Gpu() {
        if (device && renderer_ok) {
            device->destroy_texture(target);
            renderer.shutdown();
        }
        device.reset();
        window.reset();
        if (inited) platform::shutdown();
    }
    bool ok() const { return renderer_ok; }
    const rhi::DeviceCaps& caps() const { return device->caps(); }

    template <class Fill>
    void render(Fill&& fill) {
        Camera2D cam;
        device->begin_frame(frame++);
        SpriteBatch batch(renderer.sprite_sets(), mem::heap());
        batch.begin(compute_view_2d(cam, {kSize, kSize}, kPpm));
        fill(batch);
        batch.build();
        rhi::PassAttachment color;
        color.texture = target;
        color.clear.color[0] = color.clear.color[1] = color.clear.color[2] = 0.0f;
        color.clear.color[3] = 1.0f;
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.debug_name = "test.bc";
        rhi::CommandBuffer cmd;
        cmd.begin_pass(pass);
        renderer.record_2d(cmd, batch);
        cmd.end_pass();
        device->submit(cmd);
        REQUIRE(device->read_pixels(target, rhi::Rect2D{0, 0, kSize, kSize}, pixels.span()).has_value());
        device->end_frame();
    }
    Rgba at_world(vec2 p) const {
        const u32 x = static_cast<u32>(p.x * kPpm + kSize / 2.0f);
        const u32 y = static_cast<u32>(kSize / 2.0f - p.y * kPpm);
        const usize i = (usize(y) * kSize + x) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
    }
    SpriteSetId set_of(u32 key, u32 normal_key = ~0u) {
        SpriteSetDesc sd;
        sd.texture_key = key;
        const SpriteRegion regions[] = {{0, 0, 0, 1, 1, 1, 1}, {1, 0, 0, 1, 1, 1, 1}};
        sd.regions = regions;
        if (normal_key != ~0u) {
            sd.lit = true;
            sd.normal_key = normal_key;
        }
        return renderer.create_sprite_set(sd);
    }
    SpriteInstance sprite(SpriteSetId set, vec2 pos, u32 region = 0) const {
        SpriteInstance s;
        s.set = set;
        s.region = region;
        s.position = pos;
        return s;
    }

    bool inited = false, renderer_ok = false;
    mem::UniquePtr<platform::Window> window;
    mem::UniquePtr<rhi::RenderDevice> device;
    Renderer renderer;
    rhi::TextureHandle target;
    Array<u8> pixels;
    u64 frame = 0;
};

#define REQUIRE_GPU_OR_SKIP(g)                                                  \
    if (!(g).ok()) {                                                            \
        MESSAGE("skipped: no GPU device in this environment (no video driver)"); \
        return;                                                                 \
    }

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

// A texture is linear to the sprite shader unless it is sRGB; the target re-encodes to sRGB, so a linear texel v reads back as
// srgb(v) and an sRGB texel reads back as itself.
int srgb_encode(int v) {
    const f32 c = static_cast<f32>(v) / 255.0f;
    const f32 e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(e * 255.0f));
}

// ---- solid 4x4 blocks ------------------------------------------------------------------------------------------

void bc1_block(u16 c0, u16 c1, byte* out) {  // c0 > c1: four-colour mode; all indices 0 -> c0
    std::memset(out, 0, 8);
    out[0] = byte(c0 & 0xFF), out[1] = byte(c0 >> 8), out[2] = byte(c1 & 0xFF), out[3] = byte(c1 >> 8);
}
void bc4_block(u8 v, byte* out) {  // red0 > red1: eight-value mode; all indices 0 -> red0
    std::memset(out, 0, 8);
    out[0] = byte(v);
}
// Mode 6, all indices 0: the colour is endpoint 0, 7 bits and a p-bit per channel.
void bc7_block(Rgba c, byte* out) {
    u8 bits[16] = {};
    u32 at = 0;
    auto put = [&](u32 v, u32 n) {
        for (u32 i = 0; i < n; ++i, ++at)
            if ((v >> i) & 1u) bits[at / 8] = u8(bits[at / 8] | (1u << (at % 8)));
    };
    put(0x40, 7);  // mode 6: six zero bits, then a one
    const u8 ch[4] = {c.r, c.g, c.b, c.a};
    for (u8 v : ch) {
        put(v >> 1, 7);  // endpoint 0
        put(v >> 1, 7);  // endpoint 1
    }
    put(c.r & 1u, 1);  // p-bit of endpoint 0, shared by all four channels: the test colours have equal low bits
    put(c.r & 1u, 1);
    for (u32 i = 0; i < 16; ++i) out[i] = byte(bits[i]);
}

enum class Kind { BC1, BC4, BC5, BC7 };

// A cooked texture of `size`² with every mip a solid block colour.
void solid_blob(Kind kind, bool srgb, u32 size, u32 mips, Array<byte>& out) {
    Array<byte> px[8];
    asset::TextureMipData data[8];
    for (u32 m = 0; m < mips; ++m) {
        const u32 s = size >> m;
        const usize blocks = usize((s + 3) / 4) * ((s + 3) / 4);
        const usize block_size = (kind == Kind::BC1 || kind == Kind::BC4) ? 8 : 16;
        px[m].resize(blocks * block_size);
        for (usize b = 0; b < blocks; ++b) {
            byte* p = px[m].data() + b * block_size;
            switch (kind) {
                case Kind::BC1: bc1_block(0x8408, 0x0000, p); break;  // RGB565 (16, 32, 8) -> (132, 130, 66)
                case Kind::BC4: bc4_block(200, p); break;
                case Kind::BC5: bc4_block(180, p), bc4_block(60, p + 8); break;
                case Kind::BC7: bc7_block({201, 101, 51, 255}, p); break;
            }
        }
        data[m] = {s, s, px[m].span()};
    }
    const AF f = kind == Kind::BC1 ? AF::BC1 : kind == Kind::BC4 ? AF::BC4 : kind == Kind::BC5 ? AF::BC5 : AF::BC7;
    asset::write_texture(f, 1, srgb ? asset::TextureFlags::Srgb : 0u, Span<const asset::TextureMipData>(data, mips), out);
}

}  // namespace

TEST_CASE("bc gl: cooked BC1, BC4, BC5 and BC7 textures upload, sample and read back within the bounds") {
    Gpu g;
    REQUIRE_GPU_OR_SKIP(g);
    if (!g.caps().texture_bc) {
        MESSAGE("skipped: this device cannot sample BC (texture_bc false); the refusal case covers it");
        return;
    }
    struct Case {
        Kind kind;
        bool srgb;
        const char* name;
        RF format;
        Rgba expect;  // the stored texel (a BC5 blue is 0, alpha 1)
    };
    const Case cases[] = {{Kind::BC1, true, "BC1_SRGB", RF::BC1_SRGB, {132, 130, 66, 255}},
                          {Kind::BC1, false, "BC1", RF::BC1_UNorm, {132, 130, 66, 255}},
                          {Kind::BC4, false, "BC4", RF::BC4_UNorm, {200, 0, 0, 255}},
                          {Kind::BC5, false, "BC5", RF::BC5_UNorm, {180, 60, 0, 255}},
                          {Kind::BC7, true, "BC7_SRGB", RF::BC7_SRGB, {201, 101, 51, 255}},
                          {Kind::BC7, false, "BC7", RF::BC7_UNorm, {201, 101, 51, 255}}};
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Array<byte> blob;
        solid_blob(c.kind, c.srgb, 8, 4, blob);  // 8, 4, 2, 1: the last two are one padded block each
        const u32 key = g.renderer.textures().allocate_key();
        auto tex = upload_cooked_texture(g.renderer.textures(), key, 1, blob.span());
        REQUIRE(tex.has_value());
        const rhi::TextureDesc d = g.device->texture_desc(tex.value());
        CHECK(d.format == c.format);
        CHECK(d.mip_levels == 4);
        const SpriteSetId set = g.set_of(key);
        g.render([&](SpriteBatch& b) { b.add(g.sprite(set, vec2(0, 0))); });
        const Rgba px = g.at_world(vec2(0, 0));
        const int tol = 4;  // GPU block decode may differ by a level between vendors, plus the sRGB round trip
        const int er = c.srgb ? c.expect.r : srgb_encode(c.expect.r);
        const int eg = c.srgb ? c.expect.g : srgb_encode(c.expect.g);
        const int eb = c.srgb ? c.expect.b : srgb_encode(c.expect.b);
        CHECK(near(px.r, er, tol));
        CHECK(near(px.g, eg, tol));
        CHECK(near(px.b, eb, tol));
        CHECK(px.a == 255);
        CHECK(g.device->validation_error_count() == 0);
        g.renderer.textures().release(key);
    }
}

TEST_CASE("bc gl: unpack_normal shades a BC5 normal map like the same RGBA8 map") {
    Gpu g;
    REQUIRE_GPU_OR_SKIP(g);
    if (!g.caps().texture_bc) {
        MESSAGE("skipped: this device cannot sample BC (texture_bc false)");
        return;
    }
    // Layer 0 flat, layer 1 tilted 60 degrees toward +X: (238, 128, 191) as RGBA8, (238, 128) as BC5 (z is rebuilt).
    Array<u8> rgba;
    for (u32 layer = 0; layer < 2; ++layer)
        for (u32 i = 0; i < 16; ++i) {
            const u8 px[4] = {u8(layer ? 238 : 128), 128, u8(layer ? 191 : 255), 255};
            for (u8 c : px) rgba.push_back(c);
        }
    const u32 rgba_key = g.renderer.textures().allocate_key();
    {
        const TextureMipUpload mip{4, 4, std::as_bytes(rgba.span())};
        TextureUpload up;
        up.format = RF::RGBA8_UNorm;
        up.layers = 2;
        up.array = true;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        REQUIRE(g.renderer.textures().upload(rgba_key, 1, up).has_value());
    }
    Array<byte> bc5;
    bc5.resize(2 * 16);
    bc4_block(128, bc5.data()), bc4_block(128, bc5.data() + 8);
    bc4_block(238, bc5.data() + 16), bc4_block(128, bc5.data() + 24);
    const u32 bc5_key = g.renderer.textures().allocate_key();
    {
        const TextureMipUpload mip{4, 4, bc5.span()};
        TextureUpload up;
        up.format = RF::BC5_UNorm;
        up.layers = 2;
        up.array = true;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        REQUIRE(g.renderer.textures().upload(bc5_key, 1, up).has_value());
    }
    Array<u8> white;
    for (u32 i = 0; i < 2 * 16; ++i)
        for (u32 c = 0; c < 4; ++c) white.push_back(255);
    const u32 color_key = g.renderer.textures().allocate_key();
    {
        const TextureMipUpload mip{4, 4, std::as_bytes(white.span())};
        TextureUpload up;
        up.format = RF::RGBA8_SRGB;
        up.layers = 2;
        up.array = true;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        REQUIRE(g.renderer.textures().upload(color_key, 1, up).has_value());
    }
    const SpriteSetId lit_rgba = g.set_of(color_key, rgba_key), lit_bc5 = g.set_of(color_key, bc5_key);

    auto brightness = [&](SpriteSetId set, f32 light_x, u32 region) {
        Lights2D l;
        l.ambient = math::Color{0, 0, 0, 1};
        PointLight2D p;
        p.position = vec2(light_x, 0);
        p.height = 0.5f;
        p.radius = 4.0f;
        l.add(p);
        g.render([&](SpriteBatch& b) {
            b.set_lights(l);
            b.add(g.sprite(set, vec2(0, 0), region));
        });
        return static_cast<int>(g.at_world(vec2(0, 0)).r);
    };
    for (const f32 light_x : {1.0f, -1.0f})
        for (const u32 region : {0u, 1u}) {
            CAPTURE(light_x);
            CAPTURE(region);
            const int a = brightness(lit_rgba, light_x, region), b = brightness(lit_bc5, light_x, region);
            CHECK(near(a, b, 6));  // BC4 block of the same value is exact, the rebuilt z differs by rounding only
        }
    const int right = brightness(lit_bc5, 1.0f, 1), left = brightness(lit_bc5, -1.0f, 1);
    CHECK(right > left + 60);  // the BC5 map really is tilted, not flat
    CHECK(g.device->validation_error_count() == 0);
}

TEST_CASE("bc gl: a family whose cap is false is refused cleanly and draws the error checker") {
    Gpu g;
    REQUIRE_GPU_OR_SKIP(g);
    struct Family {
        const char* name;
        bool cap;
        Kind kind;
        bool astc;
    };
    const Family families[] = {{"BC", g.caps().texture_bc, Kind::BC7, false}, {"ASTC", g.caps().texture_astc, Kind::BC7, true}};
    u32 refused = 0;
    for (const Family& f : families) {
        CAPTURE(f.name);
        if (f.cap) {
            MESSAGE("this family is supported on this device; its refusal is covered by tests/render/test_render_bc.cpp");
            continue;
        }
        ++refused;
        Array<byte> blob;
        if (f.astc) {
            Array<byte> px;
            px.resize(2 * 2 * 16);
            const asset::TextureMipData md[1] = {{8, 8, px.span()}};
            asset::write_texture(AF::ASTC_4x4, 1, 0, Span<const asset::TextureMipData>(md, 1), blob);
        } else {
            solid_blob(f.kind, false, 8, 1, blob);
        }
        const u32 key = g.renderer.textures().allocate_key();
        const u32 live = g.renderer.textures().live_textures();
        auto r = upload_cooked_texture(g.renderer.textures(), key, 1, blob.span());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == Errc::Unsupported);
        CHECK(g.renderer.textures().live_textures() == live);
        const SpriteSetId set = g.set_of(key);
        g.render([&](SpriteBatch& b) { b.add(g.sprite(set, vec2(0.05f, 0.05f))); });
        const Rgba px = g.at_world(vec2(0.05f, 0.05f));  // the 8x8 magenta/black checker
        CHECK(((px.r == 255 && px.b == 255) || (px.r == 0 && px.b == 0)));
        CHECK(px.g == 0);
        CHECK(g.device->validation_error_count() == 0);  // refused before any GL / Vulkan call
    }
    if (refused == 0) MESSAGE("every family is supported on this device: the refusal was not exercised here");
}

TEST_CASE("bc gl: a BC4 height composed into a BC5 normal array samples as (h, 0, 0) beside the normal") {
    Gpu g;
    REQUIRE_GPU_OR_SKIP(g);
    if (!g.caps().texture_bc) {
        MESSAGE("skipped: this device cannot sample BC (texture_bc false)");
        return;
    }
    Array<byte> normal, height;
    solid_blob(Kind::BC5, false, 8, 2, normal);  // (180, 60)
    solid_blob(Kind::BC4, false, 8, 2, height);  // 200
    const asset::TextureView vn = asset::TextureView::read(normal.span()).value(), vh = asset::TextureView::read(height.span()).value();
    const asset::TextureView* layers[2] = {&vn, &vh};
    LayerArray arr;
    REQUIRE(compose_layer_array(Span<const asset::TextureView* const>(layers, 2), arr, "test.parallax", true));
    REQUIRE(arr.present == 0b11u);
    auto tex = create_texture(*g.device, arr.upload);
    REQUIRE(tex.has_value());
    const u32 key = g.renderer.textures().allocate_key();
    g.renderer.textures().adopt(key, 1, tex.value());
    CHECK(g.device->texture_desc(tex.value()).format == RF::BC5_UNorm);
    const SpriteSetId set = g.set_of(key);
    g.render([&](SpriteBatch& b) {
        b.add(g.sprite(set, vec2(-2, 0), 0));
        b.add(g.sprite(set, vec2(2, 0), 1));
    });
    const Rgba n = g.at_world(vec2(-2, 0)), h = g.at_world(vec2(2, 0));
    CHECK(near(n.r, srgb_encode(180), 4));
    CHECK(near(n.g, srgb_encode(60), 4));
    CHECK(near(h.r, srgb_encode(200), 4));  // the parallax march reads .r
    CHECK(h.g == 0);                        // the zero half of the widened block
    CHECK(h.b == 0);
    CHECK(g.device->validation_error_count() == 0);
}
