#pragma once
// Cutscenes (M-CINEMA 4): cinematic cameras and the cutscene a timeline plays as. All of it is data
// on entities, so the editor, MCP and a .mctimeline drive it without game code; kit::install_stock
// installs it.
//
//   CameraRail3D    moves its 3D camera along a Catmull-Rom path through up to eight points (a
//                   dolly): `t` 0..1 is the place on the path, set by a timeline property track
//                   ("CameraRail3D.t") or advanced by `speed`
//   CameraLookAt3D  turns its camera to face an entity (plus an offset), optionally smoothed
//   CameraShake3D   trauma shake of a 3D camera's view (ShakeTrigger feeds it as it feeds
//                   CameraShake2D); access.motion_scale tones the view's shake down, never the trauma
//   CameraBlend3D   on the primary camera: its view eases from `from`'s view to its own over
//                   `duration`. A timeline camera key with "blend" seconds writes one, so a cut
//                   becomes a camera move
//   Cutscene        next to a TimelinePlayer: while it plays the world's gameplay is held
//                   (World::set_cutscene_held), the player draws letterbox bars and a hold-to-skip
//                   prompt (its subtitles show as every timeline's do, captions.h); holding the skip
//                   action skips to the end
//
// Systems: "kit_cutscene" in FixedUpdate_Pre (before gameplay: it sets this tick's hold, reads the
// skip action and advances camera blends) and "kit_cinematic" in FixedUpdate_Post after the timeline player and the
// camera follow, both running through cutscene holds. A pause screen's hold stops them like the
// rest. Everything is a function of sim state (the shake noise hashes (seed, time)), so cutscenes
// replay and hash the same.

#include "mce/ecs/entity.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
struct Frame;
}  // namespace mce

namespace mce::kit {

inline constexpr u32 kRailPoints = 8;

// A camera dolly: the entity's position follows a Catmull-Rom curve through points[0..count)
// (in its parent's space; world space without one). The curve passes through every point; `t` = 0
// is the first point and 1 the last (by point, not by distance: equal steps of `t` cross one span
// each). With speed > 0 the engine advances `t` by speed × dt (wrapping with `loop`); with 0 a
// timeline or game sets it. Without a CameraLookAt3D, `face_path` turns the camera along the path.
struct CameraRail3D {
    math::vec3 points[kRailPoints];
    u32 count = 2;
    f32 t = 0.0f;
    f32 speed = 0.0f;  // t per second; 0 = driven by a timeline or the game
    bool loop = false;
    bool face_path = false;
    u8 pad_[2] = {};
};
MCE_COMPONENT(CameraRail3D, (points, Help{"the path's points (parent space); the first `count` are used"}),
              (count, Range{0, 8}, Help{"points in use (2 or more moves the camera)"}),
              (t, Range{0, 1}, Help{"place on the path: 0 = first point, 1 = last"}),
              (speed, Range{0, 10}, Help{"t per second; 0 = set by a timeline or the game"}),
              (loop, Help{"with speed: wrap at the end (the path closes back to the first point)"}),
              (face_path, Help{"turn the camera along the path (without a CameraLookAt3D)"}))

// Turns this camera to look at `target`'s position plus `offset`. smoothing 0 snaps.
struct CameraLookAt3D {
    Entity target;
    math::vec3 offset;
    f32 smoothing = 0.0f;  // 1/s; 0 = face the target every tick
    bool primed = false;   // engine-written
    u8 pad_[3] = {};
};
MCE_COMPONENT(CameraLookAt3D, (target, Help{"the entity to look at"}), (offset, Help{"added to the target's position"}),
              (smoothing, Range{0, 60}, Help{"1/s; 0 = snap"}), (primed, ReadOnly, Transient))

// Trauma shake of a 3D view (render::shake_offset's model): the view moves up to max_translation
// metres and turns up to max_rotation radians (pitch, yaw, roll) at trauma 1; both grow with
// trauma². Set `trauma` directly, from a timeline property track or through a ShakeTrigger.
struct CameraShake3D {
    f32 trauma = 0.0f;  // 0..1, decays by decay_per_second
    f32 max_translation = 0.15f;
    f32 max_rotation = 0.03f;
    f32 decay_per_second = 1.5f;
    f32 frequency_hz = 15.0f;
    u32 seed = 0x5EED;
    f32 time = 0.0f;              // engine-written
    math::vec3 offset;            // engine-written: this tick's view offset (camera space)
    math::vec3 offset_rotation;   // engine-written: pitch, yaw, roll in radians
};
MCE_COMPONENT(CameraShake3D, (trauma, Range{0, 1}), (max_translation, Range{0, 5}, Help{"metres at trauma 1"}),
              (max_rotation, Range{0, 0.5f}, Help{"radians at trauma 1"}), (decay_per_second, Range{0, 20}),
              (frequency_hz, Range{0.1f, 60}), seed, (time, ReadOnly, Transient), (offset, ReadOnly, Transient),
              (offset_rotation, ReadOnly, Transient))

// On a 3D camera: while elapsed < duration its view is a smoothstep blend from `from`'s view
// (position, orientation, field of view) to its own. The engine advances `elapsed` every tick; a
// timeline camera key with a blend writes `from`, `duration` and `elapsed` itself.
struct CameraBlend3D {
    Entity from;
    f32 duration = 1.0f;
    f32 elapsed = 0.0f;
    f32 weight = 1.0f;  // engine-written: 0 = all `from`, 1 = all this camera
};
MCE_COMPONENT(CameraBlend3D, (from, Help{"the camera the view blends from"}), (duration, Range{0, 30}, Help{"seconds"}),
              (elapsed, Help{"seconds into the blend"}), (weight, ReadOnly, Transient))

// The blend weight for `b` (smoothstep of elapsed / duration; 1 when done or without `from`).
f32 camera_blend_weight(const CameraBlend3D& b);

// Makes the TimelinePlayer on the same entity a cutscene. While the timeline plays (`active`):
// with hold_gameplay the world's gameplay is held (only timelines, animators, cameras, particles,
// sequences and audio run), the player draws letterbox bars `letterbox` of the screen high (they
// slide in and out over `letterbox_time`; the timeline's subtitle lines show over them, captions.h),
// and, when skippable, `skip_prompt` with a bar that fills while `skip_action` is held. Held for skip_hold
// seconds, the cutscene skips: the timeline's remaining event keys fire, the last animation keys
// start, the state tracks jump to the end and the timeline finishes. While a cutscene shows, the
// player switches the controls to the "UI" input map (whose Cancel is the default skip action).
struct Cutscene {
    FixedString<32> skip_action{"Cancel"};
    FixedString<48> skip_prompt{"Hold to skip"};
    f32 letterbox = 0.12f;       // each bar's height, a fraction of the screen
    f32 letterbox_time = 0.5f;   // seconds the bars take to slide in or out
    f32 skip_hold = 1.0f;        // seconds the skip action is held to skip
    bool hold_gameplay = true;
    bool skippable = true;
    bool active = false;         // engine-written: the timeline is playing
    bool skipped = false;        // engine-written: the player skipped it
    u8 pad_ = 0;
    f32 bars = 0.0f;             // engine-written: 0..1, how far the bars are in
    f32 skip_progress = 0.0f;    // engine-written: 0..1 of skip_hold
};
MCE_COMPONENT(Cutscene, (skip_action, Help{"the input action held to skip (the UI map's Cancel by default)"}),
              (skip_prompt, Help{"the text by the skip bar"}),
              (letterbox, Range{0, 0.3f}, Help{"each bar's height as a fraction of the screen; 0 = none"}),
              (letterbox_time, Range{0, 3}, Help{"seconds the bars slide in and out"}),
              (skip_hold, Range{0, 5}, Help{"seconds the skip action is held to skip"}),
              (hold_gameplay, Help{"hold the world's gameplay systems while it plays"}), skippable,
              (active, ReadOnly, Transient), (skipped, ReadOnly), (bars, ReadOnly, Transient), (skip_progress, ReadOnly, Transient))

// What the player draws for the cutscene showing (the first active one, or one whose bars are
// still sliding out). False when none.
struct CutsceneView {
    Entity entity;
    f32 bars = 0.0f;           // 0..1 eased
    f32 letterbox = 0.0f;      // each bar's full height as a fraction of the screen
    f32 skip_progress = 0.0f;  // 0..1
    bool active = false;
    bool show_skip = false;
};
bool cutscene_view(World& world, CutsceneView& out);

// The point at `t` (0..1) on a CameraRail3D's curve, and its direction of travel (not normalized).
math::vec3 rail_point(const CameraRail3D& r, f32 t, math::vec3* tangent = nullptr);

// The rotation that points a camera's -Z along `forward` with +Y up (no roll).
math::quat look_rotation(math::vec3 forward);

// Registers the components and adds the systems. Idempotent; install_stock calls it.
void install_cinematic(World& world);

// The systems (exposed for tests and custom pipelines).
void kit_cutscene(Frame& f);
void kit_cinematic(Frame& f);

// kit_cinematic's work: rails (advanced by speed × dt), look-ats (smoothed over dt) and blend
// weights. With dt = 0 it places the cameras where they are now (kit::timeline_scrub does, so a
// paused preview shows the rail and look-at at the playhead).
void place_cameras(World& world, f32 dt);

}  // namespace mce::kit
