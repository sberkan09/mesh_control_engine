#pragma once
// PreviewRenderer — draws the preview world into the viewport's render target (editor-tools.md
// §2.7): the starter-kit extract the player uses (same Sprite/TextLabel drawing) through the
// editor camera, then editor overlays (grid, selection outlines) as thin quads in the same batch.
// Owns the renderer, the asset upload sink and the extract; content comes from the project's
// mounted cooked tree.

#include "mce/editor/viewport2d.h"
#include "mce/kit/sprite_extract.h"
#include "mce/render/asset_upload.h"

namespace mce::editor {

struct OverlayLine {
    math::vec2 a, b;  // world
    math::Color color = math::Color::white();
    f32 width_px = 1.0f;
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

    render::Renderer& renderer() { return renderer_; }
    const render::BatchStats& last_stats() const { return stats_; }

private:
    render::Renderer renderer_;
    mem::UniquePtr<render::AssetUploadSink> sink_;
    kit::SpriteExtract extract_;
    asset::AssetSystem* assets_ = nullptr;
    render::BatchStats stats_;
};

// Grid lines covering the camera's view (`step` metres; every `major`-th line brighter; the axes
// tinted). Skips the grid when it would be denser than one line per 6 px.
void grid_lines(const EditorCamera2D& cam, rhi::Extent2D size, f32 step, u32 major, Array<OverlayLine>& out);
// The four edges of an entity quad.
void quad_lines(const math::vec2 q[4], math::Color color, f32 width_px, Array<OverlayLine>& out);

}  // namespace mce::editor
