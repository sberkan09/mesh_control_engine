#include "mce/render/sprite_animation.h"

#include <cmath>

namespace mce::render {

namespace detail {

u64 clip_steps_at(const SpriteClip& clip, f64 time) {
    if (clip.frames.empty() || !(clip.fps > 0.0f) || !(time > 0.0)) return 0;
    // The epsilon absorbs fixed-dt accumulation error (5 × 1/60 s must reach frame 1 at 12 fps).
    const f64 steps = std::floor(time * static_cast<f64>(clip.fps) + 1e-6);
    const u64 s = steps > 1e15 ? u64(1e15) : static_cast<u64>(steps);
    const u64 n = clip.frames.size();
    return clip.mode == PlayMode::Once && s > n - 1 ? n - 1 : s;
}

u32 clip_frame_for_step(const SpriteClip& clip, u64 step) {
    const u64 n = clip.frames.size();
    if (n <= 1) return 0;
    switch (clip.mode) {
        case PlayMode::Loop: return static_cast<u32>(step % n);
        case PlayMode::PingPong: {
            const u64 period = 2 * n - 2;
            const u64 p = step % period;
            return static_cast<u32>(p < n ? p : period - p);
        }
        case PlayMode::Once: return static_cast<u32>(step < n - 1 ? step : n - 1);
    }
    return 0;
}

}  // namespace detail

u32 clip_frame_at(const SpriteClip& clip, f64 time, bool* finished) {
    const u64 step = detail::clip_steps_at(clip, time);
    // A Once clip finishes when its last frame has been shown for a full frame time.
    if (finished)
        *finished = clip.mode == PlayMode::Once && !clip.frames.empty() &&
                    time * static_cast<f64>(clip.fps) + 1e-6 >= static_cast<f64>(clip.frames.size());
    return detail::clip_frame_for_step(clip, step);
}

}  // namespace mce::render
