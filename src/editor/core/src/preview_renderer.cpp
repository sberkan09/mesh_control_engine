#include "mce/editor/preview_renderer.h"

#include <cmath>
#include <cstring>

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
    ui_skin_ready_ = ui_skin_failed_ = false;
}

const ui::Skin* PreviewRenderer::ui_skin() {
    if (ui_skin_ready_) return &ui_skin_.skin;
    if (ui_skin_failed_ || !renderer_.initialized()) return nullptr;
    if (Result<ui::BuiltinSkin*> r = ui::create_builtin_skin(renderer_, ui_skin_); !r) {
        MCE_LOG_ERROR("editor: the UI preview's built-in skin did not start");
        ui_skin_failed_ = true;
        return nullptr;
    }
    ui_skin_ready_ = true;
    return &ui_skin_.skin;
}

void PreviewRenderer::set_assets(asset::AssetSystem* assets) {
    if (assets == assets_) return;
    ++generation_;
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
    extract3d_.set_texture_store(&renderer_.textures());
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

Result<void> PreviewRenderer::bake_probes(World& world, Array<ProbeBake>& out) {
    out.clear();
    constexpr const char* kWhere = "editor.bake_probes";
    if (!ensure_3d()) return Error(Errc::InvalidArgument, kWhere, "the 3D preview is not running");
    rhi::RenderDevice& device = renderer_.device();
    render::Forward3D& fw = renderer_.forward_3d();
    // Any view: in bake mode the extract submits every mesh, and the captures ignore the camera.
    constexpr rhi::Extent2D kSize{16, 16};
    render::Scene3D scene(renderer_.meshes(), mem::heap());
    scene.begin(render::Camera3D{}, kSize);
    extract3d_.set_bake_mode(true);
    extract3d_.extract(world, scene, 1.0f);
    extract3d_.set_bake_mode(false);
    scene.build();
    Array<Entity> entities;
    for (Entity e : extract3d_.probe_entities()) entities.push_back(e);
    if (entities.empty()) return Error(Errc::InvalidArgument, kWhere, "the scene has no enabled ReflectionProbe3D");
    if (!scene.sky().ibl) return Error(Errc::InvalidArgument, kWhere, "reflection probes need Environment3D.sky_ibl = 1");

    rhi::TextureDesc td;
    td.size = kSize;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "mce.editor.probe_bake";
    const rhi::TextureHandle target = device.create_texture(td);
    fw.invalidate_probes();  // the preview's captures saw only its view's meshes
    {
        rhi::CommandBuffer cmd;
        renderer_.record_3d(cmd, scene, target);
        device.submit(cmd);
    }
    device.destroy_texture(target);
    if (fw.last_stats().probes_captured != entities.size())
        return Error(Errc::Unsupported, kWhere, "the renderer did not capture the probes (IBL off: -r.ibl 0?)");

    const rhi::Extent2D es = render::Forward3D::probe_export_size();
    const usize row = usize(es.width) * 4;
    Array<u8> strip;
    strip.resize(row * es.height);
    for (u32 i = 0; i < entities.size(); ++i) {
        {
            rhi::CommandBuffer cmd;
            if (!fw.record_probe_export(cmd, i)) return Error(Errc::Unsupported, kWhere, "probe slot is empty");
            device.submit(cmd);
        }
        MCE_TRY(device.read_pixels(fw.probe_export_target(), rhi::Rect2D{0, 0, es.width, es.height}, strip.span()));
        ProbeBake& b = out.emplace_back();
        b.entity = entities[i];
        b.rgba.resize(strip.size());
        for (u32 y = 0; y < es.height; ++y)  // read_pixels is top row first; .mcenv rows are v order
            std::memcpy(&b.rgba[y * row], &strip[(es.height - 1 - y) * row], row);
    }
    return ok();
}

Result<void> PreviewRenderer::bake_gi(World& world, GiBake& out) {
    out = {};
    constexpr const char* kWhere = "editor.bake_gi";
    if (!ensure_3d()) return Error(Errc::InvalidArgument, kWhere, "the 3D preview is not running");
    rhi::RenderDevice& device = renderer_.device();
    render::Forward3D& fw = renderer_.forward_3d();
    constexpr rhi::Extent2D kSize{16, 16};
    rhi::TextureDesc td;
    td.size = kSize;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "mce.editor.gi_bake";
    const rhi::TextureHandle target = device.create_texture(td);
    fw.invalidate_gi();  // the preview's fill saw only its view's meshes
    // In bake mode the extract submits every mesh and replaces each re-captured probe outright, so
    // every sweep after the fill adds one bounce.
    u32 sweeps = 0, frames = 0, probes = 0, probes_done = 0;
    render::GiVolume gi;
    bool filled = false;
    for (; frames < 4096; ++frames) {
        render::Scene3D scene(renderer_.meshes(), mem::heap());
        scene.begin(render::Camera3D{}, kSize);
        extract3d_.set_bake_mode(true);
        extract3d_.extract(world, scene, 1.0f);
        extract3d_.set_bake_mode(false);
        scene.build();
        gi = scene.gi_volume();
        if (!gi.enabled) break;
        probes = gi.counts[0] * gi.counts[1] * gi.counts[2];
        rhi::CommandBuffer cmd;
        renderer_.record_3d(cmd, scene, target);
        device.submit(cmd);
        // A bake submits many frames' captures: each one gets the device frame's rings afresh.
        const u64 index = device.frame_index();
        device.end_frame();
        device.begin_frame(index);
        const render::Forward3DStats& st = fw.last_stats();
        if (!filled) {
            filled = st.gi_filled;
            if (!filled && st.gi_probes == 0) break;  // nothing captured: GI off in this renderer
            continue;
        }
        probes_done += st.gi_probes;
        if (probes_done >= probes) {
            probes_done -= probes;
            if (++sweeps == 2) break;
        }
    }
    device.destroy_texture(target);
    if (!gi.enabled) return Error(Errc::InvalidArgument, kWhere, "the scene has no enabled GiVolume3D");
    if (!filled || sweeps < 2) return Error(Errc::Unsupported, kWhere, "the renderer did not fill the volume (GI off: -r.gi 0?)");

    const rhi::Extent2D es = render::gi_volume_size(gi);
    {
        rhi::CommandBuffer cmd;
        if (!fw.record_gi_export(cmd)) return Error(Errc::Unsupported, kWhere, "the volume holds nothing");
        device.submit(cmd);
    }
    const usize row = usize(es.width) * 4;
    Array<u8> strip;
    strip.resize(row * es.height);
    MCE_TRY(device.read_pixels(fw.gi_export_target(), rhi::Rect2D{0, 0, es.width, es.height}, strip.span()));
    out.entity = extract3d_.gi_entity();
    out.width = es.width;
    out.height = es.height;
    out.rgba.resize(strip.size());
    for (u32 y = 0; y < es.height; ++y)  // read_pixels is top row first; .mcenv rows are v order
        std::memcpy(&out.rgba[y * row], &strip[(es.height - 1 - y) * row], row);
    return ok();
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
