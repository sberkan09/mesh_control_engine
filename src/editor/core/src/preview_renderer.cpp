#include "mce/editor/preview_renderer.h"

#include <cmath>

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
    extract_.shutdown();
    renderer_.shutdown();
}

void PreviewRenderer::set_assets(asset::AssetSystem* assets) {
    if (assets == assets_) return;
    if (assets_) {
        extract_.reset(nullptr);  // releases its handles while the old system is still alive
        // Asset keys restart at 0 in the next system: drop every texture uploaded under the old keys.
        for (asset::AssetKey k = 0; k < assets_->asset_count(); ++k) renderer_.textures().release(k);
        assets_->set_upload_sink(nullptr);
    }
    sink_.reset();
    assets_ = assets;
    if (assets_) {
        sink_ = mem::make_unique<render::AssetUploadSink>(mem::heap(), renderer_, *assets_);
        assets_->set_upload_sink(sink_.get());
    }
    extract_.reset(assets_);
}

void PreviewRenderer::record(rhi::CommandBuffer& cmd, World& world, rhi::TextureHandle target, rhi::Extent2D size,
                             const EditorCamera2D& cam, Span<const OverlayLine> lines, Color clear) {
    render::SpriteBatch batch(renderer_.sprite_sets(), mem::frame());
    batch.begin(render::compute_view_2d(cam.to_render(), size, cam.pixels_per_meter));
    batch.reserve(lines.size() + 64);
    extract_.extract(world, batch);
    // Overlays: thin quads above every authored layer.
    const render::SpriteSetId solid = extract_.sprite_set(asset::Guid{});
    const f32 px = 1.0f / cam.ppm();
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
    batch.build();
    stats_ = batch.stats();

    rhi::PassAttachment color;
    color.texture = target;
    color.clear.color[0] = clear.r;
    color.clear.color[1] = clear.g;
    color.clear.color[2] = clear.b;
    color.clear.color[3] = clear.a;
    rhi::RenderPassDesc pass;
    pass.colors = Span<const rhi::PassAttachment>(&color, 1);
    pass.debug_name = "editor.preview";
    cmd.begin_pass(pass);
    renderer_.record_2d(cmd, batch);
    cmd.end_pass();
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
