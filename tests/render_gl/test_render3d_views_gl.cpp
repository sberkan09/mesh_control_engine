// M-VIEWS 2: several 3D views a frame. Each Forward3D view slot keeps its own targets and history;
// a view drawn into a rect of the target matches the same view drawn alone at the rect's size, and
// views of different sizes interleave frame after frame without recreating each other's targets.

#include <doctest/doctest.h>

#include <cstdlib>

#include "fixture3d.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

// The fixture's box scene from `yaw_deg` at `size`, recorded into `target` as `out`.
void record_box(Fixture3D& f, rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, f32 yaw_deg,
                const Forward3D::ViewOutput& out) {
    const Camera3D cam = orbit_camera({0, 0.5f, 0}, math::radians(yaw_deg), math::radians(35.0f), 7.0f);
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
    scene.build();
    f.renderer.record_3d(cmd, scene, target, out);
}

void clear(rhi::CommandBuffer& cmd, rhi::TextureHandle target) {
    rhi::PassAttachment c;
    c.texture = target;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&c, 1);
    cmd.begin_pass(p);
    cmd.end_pass();
}

constexpr u32 kHalf = kW / 2;

}  // namespace

TEST_CASE("render3d_views_gl: two views side by side match each view drawn alone at its size") {
    Fixture3D f;
    rhi::TextureDesc td;
    td.size = {kHalf, kH};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.half";
    const rhi::TextureHandle half = f.device->create_texture(td);
    Array<u8> alone_a, alone_b;
    alone_a.resize(usize(kHalf) * kH * 4);
    alone_b.resize(usize(kHalf) * kH * 4);

    // Each view alone, in a fresh slot of its own, at the half size.
    for (int k = 0; k < 2; ++k) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        Forward3D::ViewOutput out;
        out.slot = 3 + k;
        record_box(f, cmd, half, {kHalf, kH}, k == 0 ? 0.0f : 90.0f, out);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(half, rhi::Rect2D{0, 0, kHalf, kH}, (k == 0 ? alone_a : alone_b).span()).has_value());
        f.device->end_frame();
    }

    // Both into the halves of the full target, in two more fresh slots.
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        clear(cmd, f.target);
        Forward3D::ViewOutput left;
        left.slot = 1;
        left.rect = {0, 0, kHalf, kH};
        record_box(f, cmd, f.target, {kHalf, kH}, 0.0f, left);
        Forward3D::ViewOutput right;
        right.slot = 2;
        right.rect = {i32(kHalf), 0, kHalf, kH};
        right.first = false;
        record_box(f, cmd, f.target, {kHalf, kH}, 90.0f, right);
        f.device->submit(cmd);
    }
    REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
    f.device->end_frame();

    int worst = 0;
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x)
            for (u32 c = 0; c < 3; ++c) {
                const Array<u8>& ref = x < kHalf ? alone_a : alone_b;
                const int want = ref[(usize(y) * kHalf + (x % kHalf)) * 4 + c];
                const int got = f.pixels[(usize(y) * kW + x) * 4 + c];
                worst = std::max(worst, std::abs(want - got));
            }
    INFO("largest channel difference " << worst);
    CHECK(worst <= 1);
    // The two halves differ (the views look from different sides).
    int diff = 0;
    for (usize i = 0; i < alone_a.size(); ++i) diff += std::abs(int(alone_a[i]) - int(alone_b[i]));
    CHECK(diff > 1000);
    CHECK(f.device->validation_error_count() == 0);
    f.device->destroy_texture(half);
}

TEST_CASE("render3d_views_gl: interleaved views of different sizes keep their own targets") {
    Fixture3D f;
    rhi::TextureHandle main_hdr;
    for (int frame = 0; frame < 4; ++frame) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        clear(cmd, f.target);
        Forward3D::ViewOutput a;  // slot 0: the full target
        record_box(f, cmd, f.target, {kW, kH}, 0.0f, a);
        Forward3D::ViewOutput b;  // slot 1: a 48 x 36 inset in the corner
        b.slot = 1;
        b.rect = {8, 8, 48, 36};
        b.first = false;
        record_box(f, cmd, f.target, {48, 36}, 180.0f, b);
        f.device->submit(cmd);
        f.device->end_frame();
        // The main view's HDR target survives the inset's frames (no per-frame re-creation).
        if (frame == 0) main_hdr = f.renderer.forward_3d().hdr_target();
        CHECK(f.renderer.forward_3d().hdr_target() == main_hdr);
        CHECK(f.renderer.forward_3d().view_alive(1));
    }
    f.renderer.forward_3d().release_view(1);
    CHECK_FALSE(f.renderer.forward_3d().view_alive(1));
    CHECK(f.renderer.forward_3d().hdr_target() == main_hdr);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_views_gl: view slots follow their keys and free the ones no view asked for") {
    Fixture3D f;
    Renderer& r = f.renderer;
    const u32 a = r.view_slot(101), b = r.view_slot(202);
    CHECK(a == 0);
    CHECK(b == 1);
    r.end_views();
    CHECK(r.view_slot(202) == 1);  // a key keeps its slot from frame to frame
    CHECK(r.view_slot(101) == 0);
    r.end_views();
    // Draw into slot 1 so it holds targets, then stop asking for it: the next end_views releases it.
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        Forward3D::ViewOutput out;
        out.slot = r.view_slot(202);
        record_box(f, cmd, f.target, {kW, kH}, 0.0f, out);
        f.device->submit(cmd);
    }
    f.device->end_frame();
    CHECK(r.forward_3d().view_alive(1));
    r.end_views();  // 101 not asked this frame: slot 0 is free again (its targets stay)
    CHECK(r.view_slot(303) == 0);
    r.end_views();  // 202 not asked: slot 1 released
    CHECK_FALSE(r.forward_3d().view_alive(1));
    for (u32 i = 0; i < Forward3D::kMaxViews - 1; ++i) r.view_slot(1000 + i);
    CHECK(r.view_slot(5000) == ~0u);  // every slot taken
    r.release_views();
    CHECK(r.view_slot(5000) == 0);
}

// ---- M-VIEWS 3: render textures ------------------------------------------------------------------

namespace {

// A linear RGBA16F one-layer array the sprite and material samplers read, adopted by the store.
u32 make_render_texture(Fixture3D& f, rhi::Extent2D size, rhi::TextureHandle& out) {
    rhi::TextureDesc d;
    d.size = size;
    d.format = rhi::TextureFormat::RGBA16F;
    d.usage = rhi::TextureUsage::SampledColorTarget;
    d.dimension = rhi::TextureDimension::Tex2DArray;
    d.debug_name = "test.render_texture";
    out = f.device->create_texture(d);
    const u32 key = f.renderer.textures().allocate_key();
    f.renderer.textures().adopt(key, 1, out);
    return key;
}

// Draws region 0 of `set` over the whole of `target` (a 1 m square sprite fitted to the view).
void draw_full(Fixture3D& f, rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, SpriteSetId set,
               bool linear) {
    Camera2D cam;
    cam.fit_width = 1.0f;
    cam.fit_height = static_cast<f32>(size.height) / static_cast<f32>(size.width);
    SpriteBatch batch(f.renderer.sprite_sets(), mem::heap());
    batch.begin(compute_view_2d(cam, size, 48.0f));
    SpriteInstance s;
    s.set = set;
    s.scale = {1.0f, cam.fit_height};
    batch.add(s);
    batch.build();
    rhi::PassAttachment c;
    c.texture = target;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&c, 1);
    cmd.begin_pass(p);
    f.renderer.record_2d(cmd, batch, linear);
    cmd.end_pass();
}

}  // namespace

TEST_CASE("render3d_views_gl: a view drawn into a linear render texture shows like the same view in the window") {
    Fixture3D f;
    rhi::TextureHandle rt;
    const u32 key = make_render_texture(f, {kW, kH}, rt);
    // The whole texture, upright: a drawn texture holds its top row at v = 1.
    const SpriteRegion upright{0, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    SpriteSetDesc sd;
    sd.texture_key = key;
    sd.regions = Span<const SpriteRegion>(&upright, 1);
    const SpriteSetId set = f.renderer.create_sprite_set(sd);

    // The view into the texture (linear, slot 1), shown full screen through a sprite.
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        Forward3D::ViewOutput out;
        out.slot = 1;
        out.linear_output = true;
        record_box(f, cmd, rt, {kW, kH}, 30.0f, out);
        draw_full(f, cmd, f.target, {kW, kH}, set, false);
        f.device->submit(cmd);
    }
    Array<u8> through;
    through.resize(f.pixels.size());
    REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, through.span()).has_value());
    f.device->end_frame();

    // The same view straight into the window target, from a fresh slot of its own.
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        Forward3D::ViewOutput out;
        out.slot = 2;
        record_box(f, cmd, f.target, {kW, kH}, 30.0f, out);
        f.device->submit(cmd);
    }
    REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
    f.device->end_frame();

    int worst = 0;
    usize off = 0;
    for (usize i = 0; i < f.pixels.size(); i += 4)
        for (usize c = 0; c < 3; ++c) {
            const int d = std::abs(int(f.pixels[i + c]) - int(through[i + c]));
            worst = std::max(worst, d);
            off += d > 4 ? 1 : 0;
        }
    INFO("largest channel difference " << worst << ", channels off by more than 4: " << off);
    // The anti-aliasing filter runs on linear instead of encoded values: edges may move a few
    // levels, nothing else does.
    CHECK(off * 100 < usize(kW) * kH * 3);
    CHECK(worst <= 48);
    CHECK(f.device->validation_error_count() == 0);
    f.renderer.sprite_sets().destroy(set);
    f.renderer.textures().release(key);
}

TEST_CASE("render3d_views_gl: a mesh shows a render texture upright, as it shows uploaded art") {
    Fixture3D f;
    // Mesh 9: a textured slab on the ground (each face spans the texture once).
    mesh_fixture::Builder slab;
    slab.add_node(-1);
    mesh_fixture::Box b;
    b.center = {0, 0.05f, 0};
    b.half = {2.5f, 0.05f, 2.5f};
    b.uvs = true;
    slab.add_box(b);
    REQUIRE(f.renderer.meshes().upload(9, 1, slab.build().span()).has_value());

    // Uploaded art: top row red, bottom row green (row 0 is sampled at v = 0).
    const u8 art_px[16] = {255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    rhi::TextureDesc ad;
    ad.size = {2, 2};
    ad.dimension = rhi::TextureDimension::Tex2DArray;
    ad.debug_name = "test.art";
    const rhi::TextureHandle art = f.device->create_texture(ad, std::as_bytes(Span<const u8>(art_px, 16)));
    const u32 art_key = f.renderer.textures().allocate_key();
    f.renderer.textures().adopt(art_key, 1, art);
    SpriteSetDesc asd;
    asd.texture_key = art_key;
    const SpriteRegion whole{};
    asd.regions = Span<const SpriteRegion>(&whole, 1);
    const SpriteSetId art_set = f.renderer.create_sprite_set(asd);

    // The render texture: the art drawn into it, upright.
    rhi::TextureHandle rt;
    const u32 rt_key = make_render_texture(f, {2, 2}, rt);

    auto frame = [&](u32 emissive_key, bool flip) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        draw_full(f, cmd, rt, {2, 2}, art_set, true);
        MaterialOverride mo;
        mo.params.base_color[3] = 1.0f;  // black base: the emission is all that shows
        mo.params.roughness = 1.0f;
        mo.params.emissive[0] = mo.params.emissive[1] = mo.params.emissive[2] = 1.0f;
        mo.params.uv_scale_half = flip ? asset::MaterialChunk::pack_uv_scale(1.0f, -1.0f) : 0u;
        mo.emissive_key = emissive_key;
        const Camera3D cam = orbit_camera({0, 0, 0}, 0.0f, math::radians(60.0f), 6.0f);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(cam, {kW, kH});
        no_sun(scene);
        scene.set_sky(constant_sky(0.0f, false));
        MeshInstance m;
        m.mesh = 9;
        m.material = &mo;
        scene.add(m);
        scene.build();
        f.renderer.record_3d(cmd, scene, f.target);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
        f.device->end_frame();
    };
    frame(art_key, false);
    const Array<u8> art_view = Array<u8>(f.pixels);
    frame(rt_key, true);  // what Kit3DExtract gives a ShowRenderTexture mesh

    // Both colors show, in the same places.
    usize red = 0, green = 0, differ = 0;
    for (usize i = 0; i < f.pixels.size(); i += 4) {
        red += art_view[i] > 100 && art_view[i + 1] < 40 ? 1 : 0;
        green += art_view[i + 1] > 100 && art_view[i] < 40 ? 1 : 0;
        const bool red_a = art_view[i] > art_view[i + 1] + 30, red_b = f.pixels[i] > f.pixels[i + 1] + 30;
        const bool green_a = art_view[i + 1] > art_view[i] + 30, green_b = f.pixels[i + 1] > f.pixels[i] + 30;
        differ += red_a != red_b || green_a != green_b ? 1 : 0;
    }
    INFO("red " << red << " green " << green << " pixels whose color differs " << differ);
    CHECK(red > 200);
    CHECK(green > 200);
    CHECK(differ * 100 < red + green);
    CHECK(f.device->validation_error_count() == 0);
    f.renderer.sprite_sets().destroy(art_set);
    f.renderer.textures().release(art_key);
    f.renderer.textures().release(rt_key);
}
