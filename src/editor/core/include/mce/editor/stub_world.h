#pragma once
// StubWorld — the M0 editor's stand-in for a scene (ROADMAP M0: "viewport + console panels over a
// stub world only"). It is the 01_triangle content: a colored triangle that turns at a fixed sim
// rate, rendered into whatever target the viewport hands it. Replaced by the ECS preview world
// (EditorWorld) once scenes exist.

#include "mce/rhi/rhi.h"

namespace mce::editor {

class StubWorld {
public:
    ResultV init(rhi::RenderDevice& device);
    void shutdown();

    // One fixed sim tick.
    void tick(f64 dt);
    // Records one pass that clears `target` and draws the world interpolated by `alpha`.
    // A null target is the backbuffer.
    void record(rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, f32 alpha) const;

    u64 ticks() const { return ticks_; }
    f32 angle() const { return angle_; }
    f32 turns_per_second = 0.25f;
    f32 clear_color[4] = {0.08f, 0.09f, 0.12f, 1.0f};

private:
    rhi::RenderDevice* device_ = nullptr;
    rhi::PipelineHandle pipeline_;
    f32 angle_ = 0.0f;
    f32 prev_angle_ = 0.0f;
    u64 ticks_ = 0;
};

}  // namespace mce::editor
