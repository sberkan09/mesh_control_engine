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
