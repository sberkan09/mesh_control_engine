#pragma once
// mce_ui: the runtime UI (docs/architecture/ui.md; FR-UI-010/020/030/045). A retained widget tree
// per document (a menu screen, the HUD, the dev console) with uGUI anchored-rect layout, two
// container layouts (stack, grid), action-driven focus and navigation, per-state styles, and
// drawing exclusively through render::SpriteBatch in PassId::Screen2D. No ImGui, no second
// renderer. Gameplay never includes this header (CI wall); documents are owned by runtime states.
//
//   ui::Document pause(mem::heap());
//   pause.set_skin(skin);
//   ui::Builder b(pause, pause.root());
//   b.panel("bg").fill().tint({0, 0, 0, 0.6f});
//   ui::Builder col = b.stack("buttons").at({0.5f, 0.45f}, {320, 0});
//   col.button("resume", "RESUME");
//   col.button("quit", "QUIT TO MENU");
//   ...per frame (Render_Pre):
//   pause.update(ctx);
//   for (const ui::UiEvent& e : pause.events())
//       if (e.kind == ui::UiEvent::Confirmed && e.widget == pause.find("buttons/resume")) states.pop();
//   pause.submit(batch);  // a batch begun with compute_screen_view(viewport), PassId::Screen2D
//
// Coordinates are pixels, +y down, origin at the viewport's top-left. Offsets are authored at UI
// scale 1 and multiplied by UpdateContext::ui_scale.

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/refl/property_path.h"
#include "mce/input/actions.h"
#include "mce/memory/handle.h"
#include "mce/render/sprite_batch.h"

namespace mce::render {
class Renderer;
}

namespace mce::ui {

using math::Color;
using math::vec2;

struct WidgetTag;
using WidgetId = Handle<WidgetTag>;  // generational: a removed widget's id resolves to nothing

enum class WidgetKind : u8 { Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Console };
enum class WidgetState : u8 { Normal, Hover, Pressed, Focused, Disabled, Count };

struct Anchors {                                    // uGUI RectTransform, parent-relative
    vec2 min{0, 0}, max{0, 0};                      // 0..1 of the parent rect
    vec2 offset_min{0, 0}, offset_max{0, 0};        // pixels at UI scale 1
    vec2 pivot{0.5f, 0.5f};                         // size-change origin (point anchors)
};

struct Rect {
    vec2 min, max;
    vec2 size() const { return max - min; }
    vec2 center() const { return (min + max) * 0.5f; }
    bool contains(vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
};

struct Style {  // one per WidgetState; colors are sRGB (as a design tool shows them), alpha linear
    Color tint{1, 1, 1, 1};                         // the widget's quad (Panel / Button / track)
    Color text_color{1, 1, 1, 1};
    Color accent{0.95f, 0.75f, 0.35f, 1};           // slider fill + handle, toggle check, console caret
    u32 sprite_region = ~0u;                        // into the skin's sprite set; ~0 = the white region (flat)
    f32 text_size = 0.0f;                           // pixels; 0 = the font's baked height
    bool draw_quad = true;                          // false: text only (labels)
};

struct NavEdges {  // explicit navigation; a null edge falls back to the geometric rule
    WidgetId up, down, left, right;
};

enum class Axis : u8 { Vertical, Horizontal };
enum class Align : u8 { Start, Center, End, Stretch };
struct StackLayout {
    Axis axis = Axis::Vertical;
    f32 spacing = 8.0f;
    f32 padding = 0.0f;
    Align align = Align::Stretch;  // cross axis
};
struct GridLayout {
    u32 columns = 3;
    vec2 cell{64, 64};
    vec2 spacing{8, 8};
};

// What a document draws with: a sprite set holding a white region (flat quads) and a font.
struct Skin {
    render::SpriteSetId set;
    u32 white_region = 0;
    const render::Font* font = nullptr;
};

// The engine's built-in skin: a white texture region and ProggyClean (MIT) at 13 px, printable
// ASCII, created through the renderer. Lives as long as the renderer; one per renderer.
struct BuiltinSkin {
    render::Font font;
    Skin skin;
};
Result<BuiltinSkin*> create_builtin_skin(render::Renderer& renderer, BuiltinSkin& out);
// The font tables alone (tests, headless measuring): the atlas is R8 coverage.
void builtin_font(render::FontMetrics& metrics, Array<render::FontGlyph>& glyphs, Array<u8>& atlas);

// Screen edges the OS covers (notch, home indicator; platform::Window::safe_insets()), in pixels.
struct SafeInsets {
    f32 left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const SafeInsets&) const = default;
};

struct UpdateContext {
    rhi::Extent2D viewport;                         // pixels
    SafeInsets safe_insets;                         // the root lays out inside them (M-MOBILE)
    f32 ui_scale = 1.0f;                            // display scale × user setting (FR-R2D-080)
    const input::InputState* actions = nullptr;     // the UI action map's snapshot (null: no input)
    vec2 pointer{-1, -1};                           // pixels; negative = no pointer this frame
    bool pointer_down = false, pointer_pressed = false, pointer_released = false;
    StringView text_input;                          // typed text this frame (the console only)
    bool key_backspace = false, key_enter = false, key_up = false, key_down = false;  // console editing
    f64 dt = 0.0;                                   // presentation time (caret blink)
};

// Touch as the pointer (M-MOBILE, FR-INP-070): the oldest touch in the snapshot becomes
// ctx.pointer (scaled to ctx.viewport) with its press/hold/release, so widgets hit-test by finger
// exactly as by mouse. Leaves the context untouched when the snapshot has no touch, so a host can
// fill the mouse pointer first and call this after. Set ctx.viewport before calling.
void apply_touch_pointer(UpdateContext& ctx, const input::InputState& state);

struct UiEvent {
    enum Kind : u8 { Confirmed, ValueChanged, FocusChanged, Cancelled, Submitted } kind = Confirmed;
    WidgetId widget;
    f32 value = 0.0f;
};

struct DocumentStats {
    u32 widgets = 0;
    u64 layout_passes = 0;  // full layout runs (zero while nothing is dirty)
    u32 quads = 0;          // emitted by the last submit()
    u64 binding_updates = 0;  // property bindings that changed their widget
};

// The named action navigation reads (a vec2, +y up like InputState::move). Without it, navigation
// reads InputState::move. A direction navigates on its edge, then repeats while held (0.4 s, then
// every 0.12 s of UpdateContext::dt).
inline constexpr const char* kNavigateAction = "UI_Navigate";

class Document {
public:
    explicit Document(mem::IAllocator& a = mem::heap());
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    void set_skin(const Skin& skin) { skin_ = skin; mark_layout(); }
    const Skin& skin() const { return skin_; }
    // A modal document owns focus and navigation; a non-modal one (the HUD) never takes focus.
    void set_modal(bool modal) { modal_ = modal; }
    bool modal() const { return modal_; }

    WidgetId root() const { return root_; }
    WidgetId add(WidgetId parent, WidgetKind kind, StringView name);
    void remove(WidgetId w);  // the subtree; applied at the next update()
    bool valid(WidgetId w) const;

    void set_anchors(WidgetId w, const Anchors& a);
    void set_style(WidgetId w, WidgetState s, const Style& st);
    void set_style_all(WidgetId w, const Style& st);  // every state
    void set_text(WidgetId w, StringView utf8);
    void set_value(WidgetId w, f32 v);                // slider / toggle (no event)
    void set_range(WidgetId w, f32 lo, f32 hi, f32 step);
    void set_visible(WidgetId w, bool v);
    void set_interactable(WidgetId w, bool v);
    void set_nav(WidgetId w, const NavEdges& edges);
    void set_stack(WidgetId w, const StackLayout& s);
    void set_grid(WidgetId w, const GridLayout& g);
    void set_default_focus(WidgetId w) { default_focus_ = w; }
    void set_blocks_pointer(WidgetId w, bool v);      // a full-screen modal panel shields the game
    // Two-way convar binding (options screens): the widget shows the convar's value; a change goes
    // through console::execute so it is logged. Cheat convars are refused.
    bool bind_convar(WidgetId w, StringView convar);
    // Read-only property binding (ui.md §4.4, FR-UI-030): "<Component>/<field path>" on `entity`
    // (the refl path syntax the inspector uses, e.g. "Health/hp" or "Body/velocity.x"). The path
    // resolves once; each update() reads the field and touches the widget only when it changed: a
    // Text re-formats with `format` (printf, one double; default "%g"), others take it as value.
    // Numeric and bool leaves only. The world must outlive the binding; a dead entity reads nothing.
    bool bind_property(WidgetId w, const World& world, Entity entity, StringView path, const char* format = nullptr);

    // Per frame, Render_Pre: removal -> layout (dirty only) -> input -> state machines -> bindings.
    void update(const UpdateContext& ctx);
    // Quads + text in document order into a batch begun with compute_screen_view, Screen2D.
    // Quads go on `sort_layer`, text on `sort_layer + 1` (one sprite set + one font: two MDI
    // groups), so text sits above every quad of its document; stack documents 2 layers apart.
    void submit(render::SpriteBatch& batch, i16 sort_layer = 100) const;

    Span<const UiEvent> events() const { return events_.span(); }  // the last update()'s events
    WidgetId focused() const { return focused_; }
    void focus(WidgetId w);
    WidgetId hovered() const { return hovered_; }
    WidgetId find(StringView path) const;  // "pause/buttons/resume", relative to the root
    WidgetId find_child(WidgetId parent, StringView name) const;

    WidgetKind kind(WidgetId w) const;
    WidgetState state(WidgetId w) const;
    const Style& style(WidgetId w, WidgetState s) const;
    const Anchors& anchors(WidgetId w) const;
    Rect rect(WidgetId w) const;              // after the last update()
    StringView text(WidgetId w) const;
    StringView name(WidgetId w) const;
    f32 value(WidgetId w) const;
    bool visible(WidgetId w) const;
    WidgetId parent(WidgetId w) const;
    vec2 preferred_size(WidgetId w) const;    // text measured with the skin's font
    const DocumentStats& stats() const { return stats_; }

    // The console widget's scrollback and command history (FR-UI-045). Lines are pushed by the
    // host (the log ring); a submitted line arrives as a Submitted event with the text readable
    // through console_input_line() until the next update().
    void console_push_line(WidgetId console, StringView line, Color color = Color{0.85f, 0.85f, 0.85f, 1});
    void console_clear(WidgetId console);
    StringView console_submitted() const { return submitted_.view(); }
    StringView console_input(WidgetId console) const;
    u32 console_line_count(WidgetId console) const;

private:
    struct ConsoleData;
    struct Node {
        u32 generation = 1;
        bool alive = false;
        WidgetKind kind = WidgetKind::Panel;
        u32 parent = ~0u, first_child = ~0u, last_child = ~0u, next_sibling = ~0u, prev_sibling = ~0u;
        FixedString<32> name;
        Array<char> text;
        Anchors anchors;
        Style styles[static_cast<u32>(WidgetState::Count)];
        Rect rect;
        f32 value = 0.0f, lo = 0.0f, hi = 1.0f, step = 0.1f;
        bool visible = true, interactable = true, blocks_pointer = false;
        NavEdges nav;
        StackLayout stack;
        GridLayout grid;
        FixedString<64> convar;
        u32 console = ~0u;  // index into consoles_
        bool pressed = false;
    };

    Node* node(WidgetId w);
    const Node* node(WidgetId w) const;
    WidgetId id_of(u32 index) const { return WidgetId{index, nodes_[index].generation}; }
    void mark_layout() { layout_dirty_ = true; }
    void unlink(u32 index);
    void destroy_subtree(u32 index);
    void layout();
    void layout_node(u32 index, const Rect& parent, f32 scale);
    void layout_children(u32 index, f32 scale);
    void rebuild_order();
    bool focusable(u32 index) const;
    bool effectively_visible(u32 index) const;
    u32 hit_test(vec2 p) const;
    WidgetId navigate(vec2 dir) const;
    void emit(UiEvent::Kind kind, WidgetId w, f32 value = 0.0f) { events_.push_back(UiEvent{kind, w, value}); }
    void change_value(u32 index, f32 v);
    void sync_bindings();
    void sync_properties();
    struct PropertyBinding {
        WidgetId widget;
        const World* world = nullptr;
        Entity entity;
        const refl::TypeRecord* type = nullptr;
        refl::PropertyRef ref;
        ecs::ComponentId component = ecs::kInvalidComponent;
        FixedString<32> format;
        f64 last = 0.0;
        bool has_last = false;
    };
    void update_console(u32 index, const UpdateContext& ctx);
    void draw_quad(render::SpriteBatch& batch, const Rect& r, Color c, i16 layer, i32 order) const;
    void draw_region(render::SpriteBatch& batch, const Rect& r, Color c, u32 region, i16 layer, i32 order) const;
    void draw_text(render::SpriteBatch& batch, StringView s, vec2 at, f32 size, Color c, i16 layer, i32 order,
                   f32 max_width = 0.0f) const;

    mem::IAllocator& alloc_;
    Array<Node> nodes_;
    Array<u32> free_;
    Array<u32> order_;          // visible-or-not, document order (depth-first)
    Array<WidgetId> pending_remove_;
    Array<UiEvent> events_;
    Array<ConsoleData*> consoles_;
    Array<PropertyBinding> properties_;
    FixedString<256> submitted_;
    WidgetId root_, focused_, hovered_, pressed_, default_focus_;
    Skin skin_;
    rhi::Extent2D viewport_{};
    SafeInsets safe_{};
    f32 scale_ = 1.0f;
    bool layout_dirty_ = true, order_dirty_ = true, modal_ = true;
    vec2 prev_nav_;             // last frame's quantized direction (edges + key repeat)
    f64 nav_repeat_at_ = 0.0;
    f64 time_ = 0.0;
    bool pointer_held_ = false;
    mutable DocumentStats stats_;
    mutable render::TextLayout scratch_;

public:
    ~Document();
};

// Builder sugar over Document (C++ is the authoring format at v1.0, ui.md §5.4).
class Builder {
public:
    Builder(Document& doc, WidgetId parent) : doc_(&doc), id_(parent) {}

    Builder panel(StringView name);
    Builder image(StringView name, u32 sprite_region);
    Builder text(StringView name, StringView utf8);
    Builder button(StringView name, StringView label);
    Builder toggle(StringView name, StringView label, bool on = false);
    Builder slider(StringView name, f32 lo, f32 hi, f32 step = 0.1f);
    Builder stack(StringView name, const StackLayout& s = {});
    Builder grid(StringView name, const GridLayout& g = {});
    Builder console(StringView name);

    Builder& fill();                                  // anchors 0..1, zero offsets
    Builder& at(vec2 anchor, vec2 size);              // point anchor + size (0 on an axis = preferred)
    Builder& anchors(const Anchors& a);
    Builder& style(const Style& s);                   // every state
    Builder& style(WidgetState state, const Style& s);
    Builder& tint(Color c);                           // every state's tint
    Builder& text_size(f32 px);
    Builder& nav(const NavEdges& e);
    Builder& bind_convar(StringView convar);
    Builder& default_focus();
    Builder& blocks_pointer();
    WidgetId id() const { return id_; }

private:
    Builder child(WidgetKind kind, StringView name);
    Document* doc_;
    WidgetId id_;
};

// A game UI layer a host adds to the stock player (player::PlayerDesc::ui_layers): a view of the
// world drawn in screen space over the game, under the pause overlay and the console. It reads the
// world (FR-UI-030: gameplay never includes mce/ui) and does not change it; input reaches the game
// through the sim's input as usual. The player calls init once the renderer and its built-in skin
// exist, update every rendered frame and submit into the screen-space batch (sort layers from
// `sort_layer`, two per document). Headless runs call none of them. Null functions are skipped.
// The default NPC dialogue box is one (mce/kit/novel_ui.h).
struct Layer {
    void* user = nullptr;
    bool (*init)(void* user, render::Renderer& renderer, const Skin& skin) = nullptr;
    void (*update)(void* user, World& world, const UpdateContext& ctx) = nullptr;
    void (*submit)(void* user, render::SpriteBatch& batch, i16 sort_layer) = nullptr;
};

// The shipped developer console (FR-UI-045): a modal document holding one console widget over the
// top of the screen. It mirrors the log ring (mce::log::read_console), runs submitted lines through
// console::execute (convars and confuncs; "help" lists them, "clear" empties the scrollback) and
// closes on cancel. The host toggles it (` or F1 by convention), turns platform text input on
// while it is open, and stops feeding game input meanwhile.
class DevConsole {
public:
    explicit DevConsole(mem::IAllocator& a = mem::heap());
    void init(const Skin& skin, f32 height_fraction = 0.6f);
    bool is_open() const { return open_; }
    void set_open(bool open);
    void toggle() { set_open(!open_); }
    // Pulls new log lines (open or not, so nothing is missed), then updates the document while open.
    void update(const UpdateContext& ctx);
    void submit(render::SpriteBatch& batch, i16 sort_layer = 200) const;
    Document& document() { return doc_; }
    WidgetId widget() const { return console_; }

private:
    void run(StringView line);
    Document doc_;
    WidgetId console_;
    u64 seq_ = 0;
    bool open_ = false;
};

// The default look for interactive widgets (dark panel, lighter on hover, accent on focus).
void apply_default_style(Document& doc, WidgetId w);

}  // namespace mce::ui
