#include "mce/kit/views.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "mce/kit/components.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/render_textures.h"
#include "mce/kit/sprite_extract.h"
#include "mce/memory/frame_slots.h"

namespace mce::kit {

static_assert(sizeof(CameraView) == 48);
static_assert(sizeof(SplitScreen) == 8);

rhi::Rect2D view_rect(const math::vec4& viewport, rhi::Extent2D size) {
    auto edge = [](f32 f, u32 n) {
        const f32 c = math::clamp(f, 0.0f, 1.0f);
        return static_cast<i32>(std::lround(c * static_cast<f32>(n)));
    };
    const i32 x0 = edge(viewport.x, size.width), x1 = edge(viewport.x + viewport.z, size.width);
    const i32 y0 = edge(viewport.y, size.height), y1 = edge(viewport.y + viewport.w, size.height);
    rhi::Rect2D r;
    r.x = x0;
    r.y = y0;
    r.width = x1 > x0 ? static_cast<u32>(x1 - x0) : 0u;
    r.height = y1 > y0 ? static_cast<u32>(y1 - y0) : 0u;
    return r;
}

math::vec4 split_cell(SplitLayout layout, u32 count, u32 index, f32 aspect) {
    count = math::clamp(count, 1u, 4u);
    if (count == 1 || index >= count) return {0.0f, 0.0f, 1.0f, 1.0f};
    if (layout == SplitLayout::Auto) {
        if (count == 2) layout = aspect >= 1.0f ? SplitLayout::Columns : SplitLayout::Rows;
        else if (count == 3) return index == 0 ? math::vec4{0.0f, 0.0f, 1.0f, 0.5f} : math::vec4{index == 1 ? 0.0f : 0.5f, 0.5f, 0.5f, 0.5f};
        else layout = SplitLayout::Grid;
    }
    if (layout == SplitLayout::Grid && count == 2) layout = SplitLayout::Columns;
    const f32 n = static_cast<f32>(count), i = static_cast<f32>(index);
    switch (layout) {
        case SplitLayout::Columns: return {i / n, 0.0f, 1.0f / n, 1.0f};
        case SplitLayout::Rows: return {0.0f, i / n, 1.0f, 1.0f / n};
        default: return {index % 2 == 0 ? 0.0f : 0.5f, index < 2 ? 0.0f : 0.5f, 0.5f, 0.5f};
    }
}

void scale_view_quality(render::Scene3D& scene, f32 area) {
    if (area > 0.5f) return;
    render::ShadowSettings sh = scene.shadow_settings();
    sh.cascades = math::min(sh.cascades, 2u);
    scene.set_shadows(sh);
    if (area > 0.34f) return;
    render::ScreenSpaceReflections ssr = scene.reflections();
    ssr.enabled = false;
    scene.set_reflections(ssr);
    render::ContactShadows cs = scene.contact_shadows();
    cs.enabled = false;
    scene.set_contact_shadows(cs);
    render::Volumetrics vol = scene.volumetrics();
    vol.enabled = false;
    scene.set_volumetrics(vol);
    render::PostFx post = scene.post();
    post.dof.enabled = false;
    post.motion_blur.enabled = false;
    scene.set_post(post);
}

bool collect_views(World& world, rhi::Extent2D window, Array<ViewEntry>& out, TargetSizeFn target_size, void* user) {
    out.clear();
    bool any = false;
    // The split cells: the distinct `split` values of the window's views, ascending (a bit each).
    u32 splits = 0;
    world.query<const CameraView&>().each([&](const CameraView& v) {
        any = any || v.enabled != 0;
        if (v.enabled != 0 && v.target.is_null() && v.split >= 1 && v.split <= 4) splits |= 1u << v.split;
    });
    if (!any) return false;
    SplitScreen layout;
    bool have_layout = false;
    world.query<const SplitScreen&>().each([&](const SplitScreen& s) {
        if (!have_layout) layout = s;
        have_layout = true;
    });
    const u32 cells = static_cast<u32>(std::popcount(splits));
    const f32 aspect = window.height > 0 ? static_cast<f32>(window.width) / static_cast<f32>(window.height) : 1.0f;
    const rhi::Rect2D whole{0, 0, window.width, window.height};
    // Cell `split` in window pixels, its edges inside the window moved in by half the divider.
    auto cell_of = [&](u32 split) {
        const u32 index = static_cast<u32>(std::popcount(splits & ((1u << split) - 1u)));
        rhi::Rect2D r = view_rect(split_cell(layout.layout, cells, index, aspect), window);
        const i32 half = static_cast<i32>(layout.divider / 2), rest = static_cast<i32>(layout.divider) - half;
        i32 x0 = r.x, y0 = r.y, x1 = r.x + static_cast<i32>(r.width), y1 = r.y + static_cast<i32>(r.height);
        if (x0 > 0) x0 += rest;
        if (y0 > 0) y0 += rest;
        if (x1 < static_cast<i32>(window.width)) x1 -= half;
        if (y1 < static_cast<i32>(window.height)) y1 -= half;
        return rhi::Rect2D{x0, y0, x1 > x0 ? static_cast<u32>(x1 - x0) : 0u, y1 > y0 ? static_cast<u32>(y1 - y0) : 0u};
    };

    auto add = [&](Entity e, const CameraView* v, bool is_3d) {
        ViewEntry ve;
        ve.camera = e;
        ve.is_3d = is_3d;
        ve.cell = whole;
        rhi::Extent2D size = window;
        const math::vec4 full{0.0f, 0.0f, 1.0f, 1.0f};
        if (v) {
            ve.order = v->order;
            ve.target = v->target;
            if (!v->target.is_null()) {
                if (!target_size || !world.alive(v->target)) return;
                size = target_size(world, v->target, user);
            } else if (v->split >= 1 && v->split <= 4) {
                ve.split = v->split;
                ve.cell = cell_of(v->split);
                ve.rect = view_rect(v->viewport, {ve.cell.width, ve.cell.height});
                ve.rect.x += ve.cell.x;
                ve.rect.y += ve.cell.y;
                if (ve.rect.width == 0 || ve.rect.height == 0) return;
                out.push_back(ve);
                return;
            }
        }
        ve.rect = view_rect(v ? v->viewport : full, size);
        if (ve.rect.width == 0 || ve.rect.height == 0) return;
        out.push_back(ve);
    };
    world.query<const Camera3DComponent&>().each([&](Entity e, const Camera3DComponent& c) {
        const CameraView* v = world.try_get<CameraView>(e);
        if (v ? v->enabled != 0 : c.primary != 0) add(e, v, true);
    });
    world.query<const Camera2DComponent&>().each([&](Entity e, const Camera2DComponent& c) {
        if (world.try_get<Camera3DComponent>(e)) return;  // one view per camera entity
        const CameraView* v = world.try_get<CameraView>(e);
        if (v ? v->enabled != 0 : c.primary) add(e, v, false);
    });
    std::sort(out.begin(), out.end(), [](const ViewEntry& a, const ViewEntry& b) {
        const bool at = !a.target.is_null(), bt = !b.target.is_null();
        if (at != bt) return at;
        if (a.target.index != b.target.index) return a.target.index < b.target.index;
        if (a.order != b.order) return a.order < b.order;
        if (a.is_3d != b.is_3d) return a.is_3d;
        if (a.split != b.split) return a.split < b.split;
        return a.camera.index < b.camera.index;
    });
    return true;
}

namespace {

bool covers(const rhi::Rect2D& r, const rhi::Rect2D& v) {
    return r.x <= v.x && r.y <= v.y && r.x + i32(r.width) >= v.x + i32(v.width) && r.y + i32(r.height) >= v.y + i32(v.height);
}

void set_clear(rhi::PassAttachment& a, const math::Color& c) {
    a.clear.color[0] = c.r;
    a.clear.color[1] = c.g;
    a.clear.color[2] = c.b;
    a.clear.color[3] = c.a;
}

// A clear color as a linear render texture stores it: what the window shows for `c` is c itself
// (no encode on a clear), so a texture that the window later shows through the encode holds c's
// linear value.
math::Color linear_clear(const math::Color& c) {
    auto lin = [](f32 v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {lin(c.r), lin(c.g), lin(c.b), c.a};
}

// One view into `target` (record_views' loop body). False when it drew nothing.
bool record_view(rhi::CommandBuffer& cmd, World& world, Span<const ViewEntry> views, usize i, const ViewRecordContext& ctx,
                 rhi::TextureHandle target, bool linear, ViewRecordStats& st) {
    render::Renderer& renderer = *ctx.renderer;
    const ViewEntry& v = views[i];
    {
        const rhi::Extent2D size{v.rect.width, v.rect.height};
        if (v.is_3d) {
            render::Camera3D cam3;
            if (!ctx.extract3d || !renderer.has_3d() || !ctx.extract3d->camera_of(world, v.camera, ctx.alpha, cam3)) return false;
            const u32 slot = renderer.view_slot((u64(v.camera.generation) << 32) | v.camera.index);
            if (slot == ~0u) return false;  // more 3D views than Forward3D::kMaxViews
            render::Scene3D scene(renderer.meshes(), mem::frame());
            scene.begin(cam3, size);
            scene.set_lod(ctx.lod);
            scene.set_collect_draw_causes(ctx.collect_draw_causes);
            const bool first = st.views_3d == 0;
            if (first) {
                ctx.extract3d->set_frame_dt(ctx.frame_dt);
                ctx.extract3d->extract(world, scene, ctx.alpha);
            } else {
                ctx.extract3d->extract_view(world, scene, ctx.alpha);
            }
            if (ctx.scale_quality && ctx.target_size.width > 0 && ctx.target_size.height > 0)
                scale_view_quality(scene, static_cast<f32>(u64(size.width) * size.height) /
                                              static_cast<f32>(u64(ctx.target_size.width) * ctx.target_size.height));
            scene.build();
            render::Forward3D::ViewOutput out;
            out.slot = slot;
            out.rect = v.rect;
            out.first = first;
            out.linear_output = linear;
            renderer.record_3d(cmd, scene, target, out);
            if (!linear && st.window_3d++ == 0) {
                st.first_3d = scene.stats();
                const render::Forward3DStats& fs = renderer.forward_3d().last_stats();  // this view's: the next record resets it
                st.first_3d_runs_instanced = fs.runs_instanced;
                st.first_3d_draws_instanced = fs.draws_instanced;
                for (u32 k = 0; k < kDrawCauseCount; ++k) st.first_3d_draw_causes[k] = fs.draw_causes[k];
            }
            ++st.views_3d;
            return true;
        }
        render::Camera2D cam;
        f32 ppm = 48.0f;
        math::Color clear = ctx.clear;
        if (!camera_2d_of(world, v.camera, cam, ppm, &clear)) return false;
        bool on_3d = false;
        for (usize j = 0; j < i; ++j) on_3d = on_3d || (views[j].is_3d && views[j].target == v.target && covers(views[j].rect, v.rect));
        render::SpriteBatch batch(renderer.sprite_sets(), mem::frame());
        render::View2D view = render::compute_view_2d(cam, size, ppm);
        // A render texture is written upright (its top row at v = 0, like uploaded art; see
        // Renderer::record_3d): mirror the clip space's y (sprites are not culled by winding).
        if (linear)
            for (auto& col : view.view_proj.c) col[1] = -col[1];
        batch.begin(view);
        // A view that does not own its whole target (a split-screen cell, a second camera into a texture)
        // loads what is there and fills its own rect with its clear color: a clear would wipe the others.
        bool earlier = false;
        for (usize j = 0; j < i; ++j) earlier = earlier || views[j].target == v.target;
        const bool owns_target = v.target.is_null()
                                     ? (ctx.target_size.width == 0 || covers(v.rect, rhi::Rect2D{0, 0, ctx.target_size.width, ctx.target_size.height}))
                                     : true;
        const bool fill = !on_3d && (earlier || !owns_target);
        if (fill && clear.a > 0.0f) ctx.extract2d->add_fill(batch, linear_clear(clear));  // alpha 0: an overlay camera
        ctx.extract2d->extract(world, batch);
        batch.build();
        if (!linear && st.window_2d++ == 0) st.first_2d = batch.stats();
        ++st.views_2d;
        rhi::PassAttachment color;
        color.texture = target;
        color.load = on_3d || fill ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
        set_clear(color, linear ? linear_clear(clear) : clear);
        rhi::RenderPassDesc pass;
        pass.colors = Span<const rhi::PassAttachment>(&color, 1);
        pass.viewport = v.rect;
        pass.debug_name = "views.2d";
        cmd.begin_pass(pass);
        renderer.record_2d(cmd, batch, linear);
        cmd.end_pass();
        return true;
    }
}


}  // namespace

ViewRecordStats record_views(rhi::CommandBuffer& cmd, World& world, Span<const ViewEntry> views, const ViewRecordContext& ctx) {
    ViewRecordStats st;
    MCE_ASSERT_MSG(ctx.renderer && ctx.extract2d, "record_views: renderer and sprite extract required");
    auto clear_pass = [&](rhi::TextureHandle t, const math::Color& color, const char* name) {
        rhi::PassAttachment c;
        c.texture = t;
        set_clear(c, color);
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&c, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
        cmd.end_pass();
    };
    bool window_cleared = false;
    RenderTextures* rts = ctx.render_textures;
    // A texture no view has drawn yet (its camera missing or not due) shows black, not stale memory.
    if (rts) rts->clear_new(cmd);
    for (usize i = 0; i < views.size(); ++i) {
        const ViewEntry& v = views[i];
        rhi::TextureHandle target = ctx.target;
        bool linear = false;
        if (!v.target.is_null()) {
            RenderTextures::Entry* rt = rts ? rts->find(v.target) : nullptr;
            if (!rt || rt->texture.is_null() || !rt->due) continue;  // none, or keeps last picture
            target = rt->texture;
            linear = true;
            if (!rt->cleared) {
                clear_pass(target, math::Color{0.0f, 0.0f, 0.0f, 1.0f}, "views.texture_clear");
                rt->cleared = true;
            }
        } else if (!window_cleared) {
            clear_pass(ctx.target, ctx.clear, "views.clear");
            window_cleared = true;
        }
        // A view never samples the texture it draws into (render_textures.h).
        if (rts) rts->set_drawing(v.target);
        const bool drawn = record_view(cmd, world, views, i, ctx, target, linear, st);
        if (rts) rts->set_drawing({});
        if (drawn && linear) ++st.views_texture;
    }
    if (!window_cleared) clear_pass(ctx.target, ctx.clear, "views.clear");
    return st;
}

}  // namespace mce::kit
