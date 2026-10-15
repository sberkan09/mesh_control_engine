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

// A textured (or, without a texture, solid) quad: `region` of the texture (the whole texture by
// default) stretched to `size`. An AnimatedSprite on the same entity writes texture and region
// from a sprite sheet every tick.
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
    math::vec4 region{0.0f, 0.0f, 1.0f, 1.0f};  // u0, v0, u1, v1 of the texture; v0 = top edge
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
    flip_y,
    (region, Help{"texture rect [u0, v0, u1, v1], 0..1, v0 = top; [0, 0, 1, 1] = whole"}))

// Plays a clip of a sprite sheet (.mcsheet) on the Sprite of the same entity: every tick it
// writes the sheet's texture and the current frame's region into the Sprite. Frames step on the
// fixed tick (render/sprite_animation.h); a clip's frame events fire as SpriteAnimEvent events
// (kit/sprite_anim.h) and show in `last_event`. Change `clip` to switch (the new clip starts from
// its first frame); set `restart` to play the current one again.
struct AnimatedSprite {
    asset::AssetRef<asset::SpriteSheet> sheet;
    FixedString<32> clip;           // empty: the sheet's first clip
    f32 speed = 1.0f;               // playback rate; 0 holds the frame
    bool playing = true;
    bool restart = false;
    bool finished = false;          // engine-written: a Once clip reached its end
    u8 pad_ = 0;
    f64 time = 0.0;                 // engine-written: seconds into the clip
    u32 frame = 0;                  // engine-written: frame index within the clip
    u32 events_fired = 0;           // engine-written
    FixedString<24> last_event;     // engine-written
    FixedString<32> playing_clip;   // engine-written: the clip `time` belongs to
};
MCE_COMPONENT(AnimatedSprite, (sheet, Help{"a .mcsheet sprite sheet"}), (clip, Help{"clip name; empty = the first clip"}),
              (speed, Range{0, 10}), playing, (restart, Help{"set to play the clip again from its start (the engine clears it)"}),
              (finished, ReadOnly, Transient), (time, ReadOnly, Transient), (frame, ReadOnly, Transient),
              (events_fired, ReadOnly, Transient), (last_event, ReadOnly, Transient), (playing_clip, ReadOnly, Transient))

enum class TextAlign : u8 { Left, Center, Right };
MCE_ENUM(TextAlign, Left, Center, Right)

// World-space text from a cooked font. `height` is the baked glyph height in metres; the layout
// box is anchored at the entity's position (horizontally by `align`, vertically centered). With
// `loc_key` the text is that key's string in the player's language (loc::Localization; `text`
// when the project has no string tables). `wrap_width` > 0 breaks lines at whitespace past that
// many metres, each line aligned by `align`.
//
// The look (M-UI-DATA step 4) is in fractions of `height`, so it scales with the text:
// `letter_spacing` adds space between letters, `line_height` multiplies the font's line height,
// `outline_width` > 0 rings every glyph in `outline_color`, and a non-zero `shadow_offset`
// (+y up) drops a copy in `shadow_color` under the text.
struct TextLabel {
    asset::AssetRef<asset::Font> font;
    FixedString<64> text;
    math::Color color;
    f32 height = 0.5f;
    i32 order = 0;
    i16 layer = 0;
    TextAlign align = TextAlign::Center;
    u8 pad_ = 0;
    f32 wrap_width = 0.0f;
    FixedString<32> loc_key;
    f32 letter_spacing = 0.0f;
    f32 line_height = 1.0f;
    math::Color outline_color = math::Color::black();
    f32 outline_width = 0.0f;
    math::Color shadow_color{0.0f, 0.0f, 0.0f, 0.6f};
    u32 pad2_ = 0;  // keeps shadow_offset 8-aligned without hidden padding (state_hash hashes the bytes)
    math::vec2 shadow_offset{};
};
MCE_COMPONENT(TextLabel, font, text, color, (height, Range{0.05f, 10}, Help{"glyph height in metres"}), order,
              (layer, Range{-100, 100}), align, (wrap_width, Range{0, 1000}, Help{"metres; 0 = one line"}),
              (loc_key, Help{"string table key; the text in the player's language replaces 'text'"}),
              (letter_spacing, Range{-0.5f, 2}, Help{"extra space between letters, a fraction of the height"}),
              (line_height, Range{0.5f, 4}, Help{"multiplies the font's line height (wrapped and multi-line text)"}),
              outline_color, (outline_width, Range{0, 0.5f}, Help{"outline thickness, a fraction of the height; 0 = none"}),
              shadow_color, (shadow_offset, Help{"drop shadow offset, a fraction of the height (+y up); [0, 0] = none"}))

// A UI document (.mcui) the stock player shows over the game (M-UI-DATA step 2): a HUD, a title,
// a pause or a win screen. Gameplay never touches the widgets; it talks to them through data:
// toggle `visible` to show or hide the screen, give its widgets "bind": "Component/field" to show a
// field (read from this entity when it has that component, else from the first entity that does),
// and read the input actions its widgets name ("action"): a confirmed button presses its action for
// one tick, a slider / toggle / tabs change presses its action with x = the new value. Those actions
// travel in the tick's InputState exactly as an -input script line "<tick> <Action>" would, so a
// script replays a run (an action is in the snapshot only on its press and release ticks: ask
// has_action() before reading it). Screens stack by `order`
// (higher on top); a visible modal screen takes focus and switches input to the "UI" action map.
// `theme` (a .mcuitheme, M-UI-DATA step 3) restyles the document for this scene, in place of the
// theme the document names: one menu document, a different look per scene.
struct UiScreen {
    asset::AssetRef<asset::UiDocument> document;
    i32 order = 0;
    bool visible = true;
    u8 pad_[3] = {};
    asset::AssetRef<asset::UiTheme> theme;
};
MCE_COMPONENT(UiScreen, (document, Help{"a .mcui UI document"}), (order, Range{-100, 100}, Help{"higher draws on top"}),
              (visible, Help{"shown and fed input while set"}),
              (theme, Help{"a .mcuitheme; null: the theme the document names, or the default look"}))

// A screen's navigation as data (M-UI-DATA demo): on the same entity as a UiScreen, the kit's
// `kit_ui_flow` system shows it when `show_on` is pressed, hides it when `hide_on` is pressed, and
// loads `scene` (scene::request_change) when `load_on` is pressed; the actions come from the game's
// input map or the widgets of any screen ("action" in .mcui). While a `pause` screen is visible the
// world's ticks are held (World::set_held): gameplay, physics and audio stop until it hides. A title
// menu is a visible pause screen hidden by "Play"; a pause menu one shown by "Pause" and hidden by
// "Resume" (its Cancel action too). All of it runs in the sim, so an -input script replays it.
struct UiFlow {
    asset::AssetRef<asset::Scene> scene;
    FixedString<32> show_on;
    FixedString<32> hide_on;
    FixedString<32> load_on;
    FixedString<32> quit_on;
    bool pause = false;
    u8 pad_[3] = {};
};
MCE_COMPONENT(UiFlow, (scene, Help{"the scene load_on loads (null: none)"}),
              (show_on, Help{"an input action that shows this screen, e.g. Pause"}),
              (hide_on, Help{"an input action that hides it, e.g. Resume"}),
              (load_on, Help{"an input action that loads 'scene', e.g. Restart"}),
              (quit_on, Help{"an input action that ends the game (scene::request_quit), e.g. Quit"}),
              (pause, Help{"hold the game (systems, physics, audio) while this screen is visible"}))

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

// Scene-wide settings (M-DATA-REACH 3): the first WorldSettings in ECS order is applied at
// Startup by kit::install_stock (stock.h) to the 2D and 3D physics worlds. One per scene; the
// MCP project_* tools and the editor's Project Settings panel put it on an entity named
// "World Settings".
struct WorldSettings {
    math::vec2 gravity_2d{0.0f, -9.81f};     // m/s^2 of physics2d's World2D
    math::vec3 gravity_3d{0.0f, -9.81f, 0.0f};  // m/s^2 of kit3d's Physics3D world
    u32 pad_ = 0;
};
MCE_COMPONENT(WorldSettings, (gravity_2d, Help{"2D physics gravity, m/s^2"}), (gravity_3d, Help{"3D physics gravity, m/s^2"}))

}  // namespace mce::kit
