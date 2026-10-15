#pragma once
// PreviewRenderer — draws the preview world into the viewport's render target (editor-tools.md
// §2.7): the starter-kit extract the player uses (same Sprite/TextLabel drawing) through the
// editor camera, then editor overlays (grid, selection outlines) as thin quads in the same batch.
// 3D scenes (kit3d: MeshRef, lights, Environment3D) go through the player's Kit3DExtract and the
// forward renderer instead (record_3d), started on first use. Owns the renderer, the asset upload
// sink and the extracts; content comes from the project's mounted cooked tree.

#include "mce/editor/viewport2d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/sprite_extract.h"
#include "mce/render/asset_upload.h"

namespace mce::editor {

struct OverlayLine {
    math::vec2 a, b;  // world
    math::Color color = math::Color::white();
    f32 width_px = 1.0f;
};

// One reflection probe's bake (PreviewRenderer::bake_probes): the probe's slot as the layers of a
// .mcenv file (Forward3D::kProbeLayers layers of kIblEnvWidth x kIblEnvWidth/2 RGBM texels, rows
// in texture order).
struct ProbeBake {
    Entity entity;
    Array<u8> rgba;
};

// The irradiance volume's bake (PreviewRenderer::bake_gi): the GiVolume3D's entity and its volume as
// one RGBM layer of render::gi_volume_size() texels, rows in texture order.
struct GiBake {
    Entity entity;
    u32 width = 0, height = 0;
    Array<u8> rgba;
};

class PreviewRenderer {
public:
    PreviewRenderer() = default;
    PreviewRenderer(const PreviewRenderer&) = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;
    ~PreviewRenderer() { shutdown(); }

    ResultV init(rhi::RenderDevice& device);
    void shutdown();
    bool initialized() const { return renderer_.initialized(); }

    // Switches content to `assets` (null: none). Call with null BEFORE the asset system it was
    // given is destroyed (the editor does this around every cook), then with the new one.
    void set_assets(asset::AssetSystem* assets);

    // Clears `target`, draws `world` through `cam`, then `lines` on top.
    void record(rhi::CommandBuffer& cmd, World& world, rhi::TextureHandle target, rhi::Extent2D size,
                const EditorCamera2D& cam, Span<const OverlayLine> lines, math::Color clear);

    // 3D: the forward renderer draws `world`'s kit3d content through `cam` into `target`, then
    // `lines_px` (viewport pixels, origin at the centre, +y up) go on top. False when the 3D path
    // does not start (the target is then cleared to `clear`).
    bool record_3d(rhi::CommandBuffer& cmd, World& world, rhi::TextureHandle target, rhi::Extent2D size,
                   const render::Camera3D& cam, Span<const OverlayLine> lines_px, math::Color clear);
    // Captures every ReflectionProbe3D of `world` (up to four, ignoring their current bakes) with
    // every mesh of the world, whatever the view, and reads each probe's slot back into `out`.
    // Records, submits and reads back by itself: call it outside any command recording. Fails
    // without a 3D path, without probes, or when the environment does not turn sky_ibl on.
    Result<void> bake_probes(World& world, Array<ProbeBake>& out);
    // Fills the first enabled GiVolume3D of `world` (ignoring its current bake) with every mesh of the
    // world, then re-captures every probe twice more (two more bounces), and reads the volume back
    // into `out`. Records, submits and reads back by itself: call it outside any command recording.
    // Fails without a 3D path or without a volume.
    Result<void> bake_gi(World& world, GiBake& out);
    // The primary Camera3DComponent's view of `world` (false: none, or no 3D path).
    bool game_camera_3d(World& world, render::Camera3D& out);

    render::Renderer& renderer() { return renderer_; }
    const render::BatchStats& last_stats() const { return stats_; }
    const render::Scene3D::Stats& last_stats_3d() const { return stats3d_; }
    const kit::Kit3DStats& last_kit_stats_3d() const { return extract3d_.stats(); }

private:
    render::Renderer renderer_;
    mem::UniquePtr<render::AssetUploadSink> sink_;
    bool ensure_3d();
    // `lines` as thin quads, `px` metres per pixel.
    void add_lines(render::SpriteBatch& batch, Span<const OverlayLine> lines, f32 px);
    // One 2D pass into `target`: cleared to `clear`, or drawn over what it holds when `load`.
    void record_batch(rhi::CommandBuffer& cmd, const render::SpriteBatch& batch, rhi::TextureHandle target, math::Color clear,
                      bool load);

    kit::SpriteExtract extract_;
    kit::Kit3DExtract extract3d_;
    bool three_d_ = false, three_d_failed_ = false;
    asset::AssetSystem* assets_ = nullptr;
    render::BatchStats stats_;
    render::Scene3D::Stats stats3d_;
};

// True when `world` holds kit3d content to draw in 3D (a Camera3DComponent or a MeshRef).
bool world_is_3d(World& world);

// Grid lines covering the camera's view (`step` metres; every `major`-th line brighter; the axes
// tinted). Skips the grid when it would be denser than one line per 6 px.
void grid_lines(const EditorCamera2D& cam, rhi::Extent2D size, f32 step, u32 major, Array<OverlayLine>& out);
// The four edges of an entity quad.
void quad_lines(const math::vec2 q[4], math::Color color, f32 width_px, Array<OverlayLine>& out);

}  // namespace mce::editor
