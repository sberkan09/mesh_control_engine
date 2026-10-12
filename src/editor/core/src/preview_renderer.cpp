#include "mce/editor/preview_renderer.h"

#include <cmath>

#include "mce/log.h"
#include "mce/memory/frame_slots.h"

namespace mce::editor {

using namespace mce::math;

ResultV PreviewRenderer::init(rhi::RenderDevice& device) {
    MCE_TRY(renderer_.init(device));
    MCE_TRY(extract_.init(renderer_, nullptr));
    return ok();
}

void PreviewRenderer::shutdown() {
    if (!renderer_.initialized()) return;
    set_assets(nullptr);
    extract3d_.shutdown();
    three_d_ = three_d_failed_ = false;
    extract_.shutdown();
    renderer_.shutdown();
}

void PreviewRenderer::set_assets(asset::AssetSystem* assets) {
    if (assets == assets_) return;
    if (assets_) {
        extract_.reset(nullptr);  // releases its handles while the old system is still alive
        if (three_d_) extract3d_.reset(nullptr);
        // Asset keys restart at 0 in the next system: drop every texture and mesh uploaded under the old keys.
        for (asset::AssetKey k = 0; k < assets_->asset_count(); ++k) {
            renderer_.textures().release(k);
            renderer_.meshes().release(k);
        }
        assets_->set_upload_sink(nullptr);
    }
    sink_.reset();
    assets_ = assets;
    if (assets_) {
        sink_ = mem::make_unique<render::AssetUploadSink>(mem::heap(), renderer_, *assets_);
        assets_->set_upload_sink(sink_.get());
    }
    extract_.reset(assets_);
    if (three_d_) extract3d_.reset(assets_);
}

void PreviewRenderer::record(rhi::CommandBuffer& cmd, World& world, rhi::TextureHandle target, rhi::Extent2D size,
                             const EditorCamera2D& cam, Span<const OverlayLine> lines, Color clear) {
    render::SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
    batch.begin(render::compute_view_2d(cam.to_render(), size, cam.pixels_per_meter));
    batch.reserve(lines.size() + 64);
    extract_.extract(world, batch);
    // Overlays: thin quads above every authored layer.
    add_lines(batch, lines, 1.0f / cam.ppm());
    batch.build();
    stats_ = batch.stats();
    record_batch(cmd, batch, target, clear, false);
}

void PreviewRenderer::add_lines(render::SpriteBatch& batch, Span<const OverlayLine> lines, f32 px) {
    const render::SpriteSetId solid = extract_.sprite_set(asset::Guid{});
    for (usize i = 0; i < lines.size(); ++i) {
        const OverlayLine& l = lines[i];
        const vec2 d = l.b - l.a;
        const f32 len = length(d);
        if (len <= 0.0f) continue;
        render::SpriteInstance q;
        q.set = solid;
        q.position = (l.a + l.b) * 0.5f;
        q.rotation = std::atan2(d.y, d.x);
        q.scale = {len, l.width_px * px};
        q.tint = l.color;
        q.sort_layer = 0x7FFF;
        q.sort_order = static_cast<i32>(i);
        batch.add(q);
    }
}

void PreviewRenderer::record_batch(rhi::CommandBuffer& cmd, const render::SpriteBatch& batch, rhi::TextureHandle target,
                                   Color clear, bool load) {
    rhi::PassAttachment color;
    color.texture = target;
    color.clear.color[0] = clear.r;
    color.clear.color[1] = clear.g;
    color.clear.color[2] = clear.b;
    color.clear.color[3] = clear.a;
    if (load) color.load = rhi::LoadOp::Load;
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    pass.debug_name = "editor.preview";
    cmd.begin_pass(pass);
    renderer_.record_2d(cmd, batch);
    cmd.end_pass();
}

bool PreviewRenderer::ensure_3d() {
    if (three_d_) return true;
    if (three_d_failed_ || !renderer_.initialized()) return false;
    render::Forward3DDesc fd;
    fd.msaa = 4;
    fd.timestamps = false;  // the editor's own GPU timing owns the query slots
    if (Result<void> r = renderer_.init_3d(fd); !r) {
        FixedString<512> why;
        format_error(r.error(), why);
        MCE_LOG_ERROR("editor: the 3D preview did not start: {}", why.view());
        three_d_failed_ = true;
        return false;
    }
    extract3d_.init(renderer_.meshes(), assets_);
    three_d_ = true;
    return true;
}

bool PreviewRenderer::game_camera_3d(World& world, render::Camera3D& out) {
    if (!ensure_3d()) return false;
    return extract3d_.camera(world, 1.0f, out);
}

bool PreviewRenderer::record_3d(rhi::CommandBuffer& cmd, World& world, rhi::TextureHandle target, rhi::Extent2D size,
                                const render::Camera3D& cam, Span<const OverlayLine> lines_px, Color clear) {
    // Overlays in viewport pixels: a 2D camera with one pixel per metre centred on the viewport.
    EditorCamera2D px_cam;
    px_cam.pixels_per_meter = 1.0f;
    render::SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
    batch.begin(render::compute_view_2d(px_cam.to_render(), size, px_cam.pixels_per_meter));
    add_lines(batch, lines_px, 1.0f);
    batch.build();
    if (!ensure_3d()) {
        record_batch(cmd, batch, target, clear, false);
        return false;
    }
    render::Scene3D scene(renderer_.meshes(), mem::frame());
    scene.begin(cam, size);
    extract3d_.extract(world, scene, 1.0f);
    scene.build();
    renderer_.record_3d(cmd, scene, target);
    stats3d_ = scene.stats();
    record_batch(cmd, batch, target, clear, true);
    return true;
}

bool world_is_3d(World& world) {
    return kit::has_camera_3d(world) || world.query<const kit::MeshRef&>().count() > 0;
}

void grid_lines(const EditorCamera2D& cam, rhi::Extent2D size, f32 step, u32 major, Array<OverlayLine>& out) {
    if (step <= 0.0f || step * cam.ppm() < 6.0f) return;
    const vec2 lo = cam.screen_to_world({0, static_cast<f32>(size.height)}, size);
    const vec2 hi = cam.screen_to_world({static_cast<f32>(size.width), 0}, size);
    const Color minor_c{1, 1, 1, 0.06f}, major_c{1, 1, 1, 0.14f}, x_axis{0.9f, 0.3f, 0.3f, 0.6f}, y_axis{0.3f, 0.9f, 0.3f, 0.6f};
    const i64 x0 = static_cast<i64>(std::floor(lo.x / step)), x1 = static_cast<i64>(std::ceil(hi.x / step));
    const i64 y0 = static_cast<i64>(std::floor(lo.y / step)), y1 = static_cast<i64>(std::ceil(hi.y / step));
    if (x1 - x0 > 2000 || y1 - y0 > 2000) return;
    for (i64 i = x0; i <= x1; ++i) {
        const f32 x = static_cast<f32>(i) * step;
        const Color c = i == 0 ? y_axis : (major && i % static_cast<i64>(major) == 0 ? major_c : minor_c);
        out.push_back({{x, lo.y}, {x, hi.y}, c, 1.0f});
    }
    for (i64 j = y0; j <= y1; ++j) {
        const f32 y = static_cast<f32>(j) * step;
        const Color c = j == 0 ? x_axis : (major && j % static_cast<i64>(major) == 0 ? major_c : minor_c);
        out.push_back({{lo.x, y}, {hi.x, y}, c, 1.0f});
    }
}

void quad_lines(const vec2 q[4], Color color, f32 width_px, Array<OverlayLine>& out) {
    for (int i = 0; i < 4; ++i) out.push_back({q[i], q[(i + 1) % 4], color, width_px});
}

}  // namespace mce::editor
