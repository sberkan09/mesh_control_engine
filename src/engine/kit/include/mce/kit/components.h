#pragma once
// Starter-kit 2D components (FR-ENT-080, ecs-gameplay §2.11). Plain reflected structs: what an
// editor-authored scene is made of, alongside LocalTransform and the physics2d components.
//
// Conventions: metres, Y up; an entity's LocalTransform positions the pivot, its rotation about +Z
// turns the quad and its scale.xy multiplies `size`.

#include <cmath>

#include "mce/asset/handle.h"
#include "mce/fixed_string.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

// A textured (or, without a texture, solid) quad. The texture is shown whole, stretched to
// `size`; atlas regions and animation arrive with the sprite-sheet importer.
struct Sprite {
    asset::AssetRef<asset::Texture> texture;  // null GUID: a solid quad in `tint`
    asset::AssetRef<asset::Texture> normal_map;  // set: lit by Light2D (FR-R2D-090); import it with srgb off
    math::Color tint;
    math::vec2 size{1.0f, 1.0f};    // metres
    math::vec2 pivot{0.5f, 0.5f};   // normalized; (0, 0) = bottom-left
    i32 order = 0;                  // refines stacking inside a layer
    i16 layer = 0;                  // authoritative stacking (rendering-2d D11)
    bool flip_x = false;
    bool flip_y = false;
};
MCE_COMPONENT(Sprite,
    texture,
    (normal_map, Help{"tangent-space normal map (srgb off): the sprite is lit by Light2D entities"}),
    tint,
    (size, Help{"world size in metres"}),
    (pivot, Help{"normalized pivot, (0,0) = bottom-left"}),
    order,
    (layer, Range{-100, 100}),
    flip_x,
    flip_y)

enum class TextAlign : u8 { Left, Center, Right };
MCE_ENUM(TextAlign, Left, Center, Right)

// World-space text from a cooked font. `height` is the baked glyph height in metres; the layout
// box is anchored at the entity's position (horizontally by `align`, vertically centered).
struct TextLabel {
    asset::AssetRef<asset::Font> font;
    FixedString<64> text;
    math::Color color;
    f32 height = 0.5f;
    i32 order = 0;
    i16 layer = 0;
    TextAlign align = TextAlign::Center;
    u8 pad_ = 0;
};
MCE_COMPONENT(TextLabel, font, text, color, (height, Range{0.05f, 10}, Help{"glyph height in metres"}), order,
              (layer, Range{-100, 100}), align)

// The view a player renders from: the first `primary` camera in ECS order, at its entity's
// position (rotation about +Z included).
struct Camera2DComponent {
    f32 zoom = 1.0f;                // 1 = one art texel per device pixel at `pixels_per_meter`
    f32 pixels_per_meter = 48.0f;
    math::Color clear_color{0.08f, 0.09f, 0.12f, 1.0f};
    bool primary = true;
    bool pixel_snap = true;
    u8 pad_[2] = {};
    // Fit mode (resolution independence): > 0 shows at least this many metres across / up on any
    // screen; both set fits the whole rect. Overrides zoom. 0 = off (zoom and pixels_per_meter rule).
    f32 fit_width = 0.0f;
    f32 fit_height = 0.0f;
};
MCE_COMPONENT(Camera2DComponent, (zoom, Range{0.05f, 32}), (pixels_per_meter, Range{1, 512}), clear_color, primary,
              pixel_snap, (fit_width, Range{0, 10000}, Help{"metres visible across on every screen; 0 = use zoom"}),
              (fit_height, Range{0, 10000}, Help{"metres visible up on every screen; 0 = use zoom"}))

// World resource the player writes before every tick: the screen the game is drawn on and the
// primary camera's resolved view (fit applied, no shake). Read it with world.try_resource<ViewInfo>();
// headless runs write a fixed 1280 x 720 landscape screen so hashes stay deterministic; hosts that
// do not run the stock player (the editor viewport) may not write it.
struct ViewInfo {
    u32 framebuffer_width = 1280, framebuffer_height = 720;  // device pixels
    u32 window_width = 1280, window_height = 720;            // window points (InputState::mouse_pos_px)
    f32 display_scale = 1.0f;
    u32 safe_left = 0, safe_top = 0, safe_right = 0, safe_bottom = 0;  // safe-area insets in device pixels
    u8 orientation = 1;  // platform::Orientation: 0 unknown, 1 landscape, 2 landscape flipped, 3 portrait, 4 flipped
    bool headless = true;
    u8 pad_[2] = {};
    math::vec2 view_center;               // world point at the framebuffer's centre
    f32 view_pixels_per_meter = 48.0f;    // device pixels per world metre
    f32 view_rotation = 0.0f;             // radians, CCW
    f32 ui_scale = 1.0f;                  // the UI scale the player's mce_ui layers use (ui::compute_ui_scale)

    // Device pixel (origin top-left, y down) -> world metres under it.
    math::vec2 screen_to_world(math::vec2 px) const {
        const f32 ppm = view_pixels_per_meter > 0.0f ? view_pixels_per_meter : 1.0f;
        const f32 lx = (px.x - 0.5f * static_cast<f32>(framebuffer_width)) / ppm;
        const f32 ly = (0.5f * static_cast<f32>(framebuffer_height) - px.y) / ppm;
        const f32 c = std::cos(view_rotation), s = std::sin(view_rotation);
        return {view_center.x + c * lx - s * ly, view_center.y + s * lx + c * ly};
    }
    // InputState::touch_points() (0..1 of the window) -> world.
    math::vec2 touch_to_world(math::vec2 normalized) const {
        return screen_to_world({normalized.x * static_cast<f32>(framebuffer_width),
                                normalized.y * static_cast<f32>(framebuffer_height)});
    }
    // InputState::mouse_pos_px() (window points) -> world.
    math::vec2 mouse_to_world(math::vec2 window_pt) const {
        const f32 sx = window_width ? static_cast<f32>(framebuffer_width) / static_cast<f32>(window_width) : 1.0f;
        const f32 sy = window_height ? static_cast<f32>(framebuffer_height) / static_cast<f32>(window_height) : 1.0f;
        return screen_to_world({window_pt.x * sx, window_pt.y * sy});
    }
};

// 2D post effects (FR-R2D-100) for the view of the Camera2DComponent on the same entity. Each
// effect toggles at runtime; the player renders the 2D frame offscreen only while one is on.
// The LUT is a 256 × 16 strip (render::make_identity_lut) imported with srgb off.
struct Post2DComponent {
    asset::AssetRef<asset::Texture> lut;
    f32 vignette_intensity = 0.45f;
    f32 vignette_radius = 0.8f;
    f32 vignette_softness = 0.6f;
    f32 lut_strength = 1.0f;
    f32 aberration = 0.004f;
    bool vignette = false;
    bool color_grade = false;
    bool chromatic_aberration = false;
    u8 pad_ = 0;
};
MCE_COMPONENT(Post2DComponent, vignette, (vignette_intensity, Range{0, 1}), (vignette_radius, Range{0, 2}),
              (vignette_softness, Range{0.01f, 2}), color_grade, (lut, Help{"256x16 strip LUT, imported with srgb off"}),
              (lut_strength, Range{0, 1}), chromatic_aberration, (aberration, Range{0, 0.05f}))

// One baked tile layer (FR-R2D-115): the .mctilemap asset drawn with its tile (0, 0) bottom-left
// corner at the entity's position. The tiles live in the asset; the editor's Tilemap panel
// paints them and the asset hot-reloads (re-bake) on save.
struct TilemapLayer {
    asset::AssetRef<asset::Tilemap> map;
    math::Color tint;
    i32 order = 0;
    i16 layer = 0;
    u8 pad_[2] = {};
};
MCE_COMPONENT(TilemapLayer, map, tint, order, (layer, Range{-100, 100}))

// A particle emitter (FR-R2D-070; the simulation is kit::ParticleSystem in particles.h). Emits
// particles from the entity's position (LocalTransform, or WorldTransform when the
// hierarchy wrote one). The preset holds the look; the component holds the instance state that
// a scene authors.
struct ParticleEmitter {
    asset::AssetRef<asset::ParticlePreset> preset;
    u32 seed = 1;                 // per-emitter random stream (mixed with the entity index)
    i32 order = 0;                // sort order inside the layer
    i16 layer = 0;
    bool playing = true;          // emitting; clearing it stops emission, live particles finish
    bool destroy_when_done = false;  // one-shots: destroy the entity once emission ended and every particle died
};
MCE_COMPONENT(ParticleEmitter, preset, seed, order, (layer, Range{-100, 100}), playing, destroy_when_done)

// A 2D light (FR-R2D-090) at the entity's position. Point lights shade lit sprites (those with a
// normal map) within `radius`; ambient lights add up into the base light every lit sprite gets
// (white when the scene has none). At most 16 point lights touching the view are used, in ECS
// order. Unlit sprites, text and particles ignore lights.
enum class Light2DKind : u8 { Point, Ambient };
MCE_ENUM(Light2DKind, Point, Ambient)

struct Light2D {
    math::Color color;
    f32 intensity = 1.0f;
    f32 radius = 6.0f;   // metres; the falloff reaches zero here
    f32 height = 1.5f;   // metres above the sprite plane
    Light2DKind kind = Light2DKind::Point;
    u8 pad_[3] = {};
};
MCE_COMPONENT(Light2D, color, (intensity, Range{0, 64}), (radius, Range{0.05f, 1000}), (height, Range{0.01f, 100}), kind)

// A parallax layer (FR-R2D-050): the entity's Sprite, TilemapLayer and TextLabel scroll against
// the camera by `factor` per axis. 1 moves with the world, 0.5 at half speed (a far background), 0
// stays fixed to the camera (a sky); above 1 is a foreground that passes faster. The layer is
// authored where it sits when the camera is at the origin: drawn at position + camera × (1 −
// factor). `repeat_x` tiles a Sprite horizontally across the view, for endless backgrounds.
struct Parallax {
    math::vec2 factor{1.0f, 1.0f};
    bool repeat_x = false;
    u8 pad_[3] = {};
};
MCE_COMPONENT(Parallax, (factor, Help{"scroll factor per axis: 1 = world, 0 = fixed to the camera"}), repeat_x)

// Plays an action-list sequence (FR-ENT-100; the runner is kit::install_sequences in
// sequence.h): a cooked .mcseq asset, or a sequence defined from code by `name` when `sequence`
// has no GUID. The progress fields are the whole runtime state, so a save game resumes a
// sequence mid-step (they carry the Save flag; give the entity a SaveKey).
struct SequencePlayer {
    asset::AssetRef<asset::Sequence> sequence;
    FixedString<32> name;
    bool active = true;           // cleared by the runner when the sequence ends
    bool anchored = false;        // `anchor` holds the current MoveTo's start
    u16 step = 0;                 // the step being run
    f32 elapsed = 0.0f;           // seconds into a Wait
    math::vec3 anchor;            // where a relative MoveTo started
    u32 pad_ = 0;
};
MCE_COMPONENT(SequencePlayer, sequence, name, (active, Save), (anchored, Save, ReadOnly), (step, Save, ReadOnly),
              (elapsed, Save, ReadOnly), (anchor, Save, ReadOnly))

enum class PathStatus : u8 { Idle, Moving, Arrived, NoPath };
MCE_ENUM(PathStatus, Idle, Moving, Arrived, NoPath)

// Walks the entity to `target` over the world's kit::NavGrid resource (grid A*; the planner and
// the "path_agents" system are in pathfinding.h). Moves a physics2d body through its velocity
// when the entity has one, else its LocalTransform. The fields after `diagonal` are runtime
// state written by the system: the current smoothed path (at most kMaxWaypoints corners; a
// longer path is followed in pieces, re-planned when the stored piece runs out).
struct PathAgent {
    static constexpr u32 kMaxWaypoints = 16;

    math::vec2 target;               // world metres
    f32 speed = 3.0f;                // metres per second
    f32 arrival_radius = 0.05f;      // metres from the target that count as arrived
    u16 repath_interval = 30;        // ticks between re-plans while moving (0 = only on a target change)
    u16 repath_countdown = 0;        // ticks to the next re-plan
    bool active = true;              // cleared: the agent stops where it is
    bool diagonal = true;            // 8-neighbour search (4-neighbour when cleared)
    PathStatus status = PathStatus::Idle;
    u8 waypoint = 0;                 // the waypoint being walked to
    u8 waypoint_count = 0;
    bool truncated = false;          // the path had more corners than kMaxWaypoints
    u8 pad_[2] = {};
    math::vec2 planned_target;       // the target the stored path leads to
    math::vec2 waypoints[kMaxWaypoints];
};
MCE_COMPONENT(PathAgent, (target, Help{"world point to walk to"}), (speed, Range{0, 100}),
              (arrival_radius, Range{0, 10}), (repath_interval, Help{"ticks between re-plans; 0 = on target change only"}),
              active, (diagonal, Help{"8-neighbour paths (no corner cutting)"}), (status, ReadOnly, Transient),
              (repath_countdown, ReadOnly, Transient), (waypoint, ReadOnly, Transient),
              (waypoint_count, ReadOnly, Transient), (truncated, ReadOnly, Transient),
              (planned_target, ReadOnly, Transient), (waypoints, ReadOnly, Transient))

}  // namespace mce::kit
