#pragma once
// Flipbook sprite animation (FR-R2D-030, rendering-2d D10): frames step on the fixed tick, never
// lerp; authored events fire when playback enters their frame.
//
// This is the pure clip evaluator. The AnimatedSprite component and the system that advances it
// belong to the gameplay starter kit (ecs-gameplay §2.9); the kit calls advance() from
// FixedUpdate and writes the resulting region into the sprite before extract.

#include "mce/types.h"

namespace mce::render {

enum class PlayMode : u8 { Loop, PingPong, Once };

struct AnimEvent {
    u16 frame;  // frame index within the clip
    u32 id;     // game-defined (e.g. StringId of "footstep")
};

struct SpriteClip {
    Span<const u32> frames;  // region indices into the sprite set, in playback order
    f32 fps = 12.0f;
    PlayMode mode = PlayMode::Loop;
    Span<const AnimEvent> events;
};

struct SpriteAnimState {
    f64 time = 0.0;      // seconds since play
    u32 frame = 0;       // current frame index within the clip
    bool finished = false;
};

// Frame index at `time` seconds (exposed for previews and tests).
u32 clip_frame_at(const SpriteClip& clip, f64 time, bool* finished = nullptr);

// Advances by dt (one fixed tick). Every frame the playhead *enters* during this step fires its
// events through `on_event(const AnimEvent&)` in playback order; the frame current at the start
// of the step does not re-fire, so frame 0's events fire on loop wrap-around but not at play
// start (the caller fires those when it starts the clip, if it wants them). Returns the frame.
template <class OnEvent>
u32 advance(const SpriteClip& clip, SpriteAnimState& state, f64 dt, OnEvent&& on_event);

// The region to draw for the current state.
inline u32 current_region(const SpriteClip& clip, const SpriteAnimState& state) {
    return clip.frames.empty() ? 0 : clip.frames[state.frame < clip.frames.size() ? state.frame : clip.frames.size() - 1];
}

// ---- implementation

namespace detail {
// Linear playhead step count (frames advanced since t = 0), saturating for Once.
u64 clip_steps_at(const SpriteClip& clip, f64 time);
u32 clip_frame_for_step(const SpriteClip& clip, u64 step);
}  // namespace detail

template <class OnEvent>
u32 advance(const SpriteClip& clip, SpriteAnimState& state, f64 dt, OnEvent&& on_event) {
    if (clip.frames.empty()) return 0;
    const u64 before = detail::clip_steps_at(clip, state.time);
    state.time += dt > 0.0 ? dt : 0.0;
    const u64 after = detail::clip_steps_at(clip, state.time);
    for (u64 s = before + 1; s <= after; ++s) {
        const u32 f = detail::clip_frame_for_step(clip, s);
        for (const AnimEvent& e : clip.events)
            if (e.frame == f) on_event(e);
    }
    bool finished = false;
    state.frame = clip_frame_at(clip, state.time, &finished);
    state.finished = finished;
    return state.frame;
}

}  // namespace mce::render
