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
// scale 1 and multiplied by UpdateContext::ui_scale (compute_ui_scale picks it from the display).
//
// Phones (M-MOBILE step 11b): the root is the safe area, and a widget can anchor to the whole
// screen instead (a backdrop under the notch). A scroll view moves its children by finger drag
// with inertia (or the wheel), clipped to its rect; modals take focus and the pointer while open;
// tabs switch pages; images can be 9-sliced. The authoring values (anchors, styles, layouts) are in
// mce/ui/types.h, reflected so the editor and the MCP server can describe them.

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/fixed_string.h"
#include "mce/refl/property_path.h"
#include "mce/input/actions.h"
#include "mce/memory/handle.h"
#include "mce/render/sprite_batch.h"
#include "mce/ui/types.h"

namespace mce::render {
class Renderer;
}

namespace mce::ui {

using math::Color;
using math::vec2;

struct WidgetTag;
using WidgetId = Handle<WidgetTag>;  // generational: a removed widget's id resolves to nothing

struct Rect {
    vec2 min, max;
    vec2 size() const { return max - min; }
    vec2 center() const { return (min + max) * 0.5f; }
    bool contains(vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
};

struct NavEdges {  // explicit navigation; a null edge falls back to the geometric rule
    WidgetId up, down, left, right;
};

// What a document draws with: a sprite set holding a white region (flat quads) and a font.
struct Skin {
    render::SpriteSetId set;
    u32 white_region = 0;
    const render::Font* font = nullptr;        // body text, and every role without a font of its own
    const render::Font* title_font = nullptr;  // FontRole::Title; null: `font` (a theme's, M-UI-DATA 3)
    const render::Font* button_font = nullptr; // FontRole::Button (buttons, toggles, sliders); null: `font`

    const render::Font* font_for(FontRole role) const {
        if (role == FontRole::Title && title_font) return title_font;
        if (role == FontRole::Button && button_font) return button_font;
        return font;
    }
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
    f32 ui_scale = 1.0f;                            // compute_ui_scale(); display scale × user setting (FR-R2D-080)
    f32 text_scale = 1.0f;                          // text size on top of ui_scale (M-ACCESS access.text_scale)
    bool high_contrast = false;                     // draw with high_contrast_theme()'s colors (M-ACCESS)
    const input::InputState* actions = nullptr;     // the UI action map's snapshot (null: no input)
    vec2 pointer{-1, -1};                           // pixels; negative = no pointer this frame
    bool pointer_down = false, pointer_pressed = false, pointer_released = false;
    f32 wheel = 0.0f;                               // notches this frame, + = away from the user (scrolls up)
    StringView text_input;                          // typed text this frame (the console only)
    bool key_backspace = false, key_enter = false, key_up = false, key_down = false;  // console editing
    f64 dt = 0.0;                                   // presentation time (caret blink)
};

// The UI scale for a screen (FR-R2D-080, M-MOBILE 11b): the display's scale (the OS's DPI over
// its baseline: 96 on desktops, 160 on Android, points on iOS) times the user's setting, lowered
// when the screen would be smaller than `min_logical` (short side × long side, at scale 1) so a
// layout authored for that size still fits, and rounded down to 1/8 so pixel snapping stays even.
f32 compute_ui_scale(rhi::Extent2D viewport, f32 display_scale, f32 user_scale = 1.0f, vec2 min_logical = {360, 640},
                     f32 reference_height = 0.0f);

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
    void set_grid(WidgetId w, const GridLayout& g);  // a Grid
    // Scroll views (WidgetKind::Scroll lays out its children with its StackLayout, set_stack): the
    // offset along the axis in pixels, 0 .. scroll_max. A drag past a few pixels scrolls instead of
    // pressing the widget under the finger, and a release keeps the drag's speed and slows down.
    void set_scroll(WidgetId w, f32 offset);
    f32 scroll(WidgetId w) const;
    f32 scroll_max(WidgetId w) const;
    // Brings `w` fully into view inside every scroll view around it (gamepad focus does this itself).
    void scroll_into_view(WidgetId w);
    // Modals: an open modal is drawn above the rest of the document, and focus, navigation and the
    // pointer stay inside it until it closes; Cancel then names the modal instead of the root.
    // Opening shows the widget and focuses into it; closing hides it and gives focus back.
    void open_modal(WidgetId w);
    void close_modal(WidgetId w);
    WidgetId top_modal() const { return modals_.empty() ? WidgetId{} : modals_.back().widget; }
    // Tabs (Builder::tabs / tab): the selected page is visible, the others hidden. Confirming a
    // tab's button selects it and sends ValueChanged on the Tabs widget with the page index.
    void select_tab(WidgetId tabs, u32 index);
    u32 selected_tab(WidgetId tabs) const;
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
    // Data a host acts on (M-UI-DATA step 2; the document itself does nothing with it): the gameplay
    // action the widget's events raise, and the property path (+ format) the host binds it to.
    void set_action(WidgetId w, StringView action);
    // Which of the skin's fonts the widget's text uses (Auto: by kind; see ui::resolve_font_role).
    void set_font_role(WidgetId w, FontRole role);
    // Where a Text widget's lines sit across its rect (default Left; other kinds ignore it).
    void set_text_align(WidgetId w, TextAlign align);
    // StyleOwn bits: style fields a theme never replaces. apply_theme works on DocumentData before
    // the build; the Document keeps the bits so an editor's extract / save round-trips them.
    void set_own_style(WidgetId w, u8 own);
    void set_binding(WidgetId w, StringView path, StringView format);
    // Whether a host has dealt with the widget's binding (bound it, or given up on a bad path):
    // ui::bind_document's bookkeeping. set_binding clears it.
    bool binding_done(WidgetId w) const;
    void set_binding_done(WidgetId w);

    // Per frame, Render_Pre: removal -> layout (dirty only) -> input -> state machines -> bindings.
    void update(const UpdateContext& ctx);
    // Quads + text in document order into a batch begun with compute_screen_view, Screen2D.
    // Quads go on `sort_layer`, text on `sort_layer + 1` (one sprite set + one font: two MDI
    // groups), so text sits above every quad of its document; stack documents 2 layers apart.
    // Each open modal takes the next two layers (sort_layer + 2, + 3 for the first), so it covers
    // the text below it: leave 2 + 2 × (modals open at once) layers to the next document.
    void submit(render::SpriteBatch& batch, i16 sort_layer = 100) const;

    Span<const UiEvent> events() const { return events_.span(); }  // the last update()'s events
    WidgetId focused() const { return focused_; }
    void focus(WidgetId w);
    WidgetId hovered() const { return hovered_; }
    WidgetId find(StringView path) const;  // "pause/buttons/resume", relative to the root
    WidgetId find_child(WidgetId parent, StringView name) const;
    // M-VIEWS 3: every Image widget named `name` (at any depth) draws the whole of `set`'s region 0
    // (a render texture's sprite set) instead of its skin region; an invalid set restores the skin.
    // Returns how many widgets it set.
    u32 set_image_texture(StringView name, render::SpriteSetId set);

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
    // The rest of a widget's authoring values (M-UI-DATA: extract_document reads them).
    bool visible_self(WidgetId w) const;      // its own flag; visible() also needs every ancestor's
    bool interactable(WidgetId w) const;
    bool blocks_pointer(WidgetId w) const;
    NavEdges nav(WidgetId w) const;
    StackLayout stack(WidgetId w) const;
    GridLayout grid(WidgetId w) const;
    void range(WidgetId w, f32& lo, f32& hi, f32& step) const;
    StringView convar(WidgetId w) const;      // the bound convar, or empty
    StringView action(WidgetId w) const;      // set_action, or empty
    FontRole font_role(WidgetId w) const;      // set_font_role, or Auto
    TextAlign text_align(WidgetId w) const;    // set_text_align, or Left
    u8 own_style(WidgetId w) const;            // set_own_style, or 0
    StringView binding(WidgetId w) const;     // set_binding's path, or empty
    StringView binding_format(WidgetId w) const;
    WidgetId default_focus() const { return default_focus_; }
    WidgetId first_child(WidgetId w) const;   // children in sibling (document) order
    WidgetId next_sibling(WidgetId w) const;
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
    struct ModalEntry {
        WidgetId widget;
        WidgetId restore_focus;
    };
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
        Rect clip;              // the part of the screen the widget may draw in (scroll views cut it)
        bool clipped = false;   // under a scroll view: draw through `clip`
        f32 value = 0.0f, lo = 0.0f, hi = 1.0f, step = 0.1f;
        f32 scroll = 0.0f, scroll_max = 0.0f, scroll_velocity = 0.0f;
        bool visible = true, interactable = true, blocks_pointer = false;
        NavEdges nav;
        StackLayout stack;
        GridLayout grid;
        FixedString<64> convar;
        FixedString<32> action;
        FixedString<64> binding;
        FixedString<32> binding_format;
        bool binding_done = false;
        FontRole font_role = FontRole::Auto;
        TextAlign text_align = TextAlign::Left;
        u8 own_style = 0;
        u32 console = ~0u;  // index into consoles_
        bool pressed = false;
        render::SpriteSetId image_set;  // set_image_texture: an Image's own set (invalid = the skin's)
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
    void layout_stack(u32 index, f32 scale);
    vec2 measure(u32 index, f32 avail_width) const;  // preferred size; avail_width (scale 1) <= 0: unknown
    Rect anchor_space(const Node& n, const Rect& parent) const;
    bool in_modal_scope(u32 index) const;
    void activate(u32 index);
    void update_scroll(const UpdateContext& ctx, bool has_pointer);
    void relayout_scroll(u32 index);
    u32 scroll_at(vec2 p) const;
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
    void draw_region(render::SpriteBatch& batch, const Rect& r, Color c, u32 region, i16 layer, i32 order,
                     const Slice& slice = {}, render::SpriteSetId set = {}) const;
    void draw_text(render::SpriteBatch& batch, const render::Font* font, StringView s, vec2 at, f32 size, Color c,
                   const Style& look, i16 layer, i32 order, f32 max_width = 0.0f, TextAlign align = TextAlign::Left) const;
    const render::Font* node_font(const Node& n) const;
    mutable const Rect* draw_clip_ = nullptr;  // set by submit() while drawing a clipped widget

    mem::IAllocator& alloc_;
    Array<Node> nodes_;
    Array<u32> free_;
    Array<u32> order_;          // visible-or-not, document order (depth-first), open modals last
    Array<u32> modal_starts_;   // where each open modal's subtree starts in order_
    Array<WidgetId> pending_remove_;
    Array<UiEvent> events_;
    Array<ConsoleData*> consoles_;
    Array<PropertyBinding> properties_;
    Array<ModalEntry> modals_;
    FixedString<256> submitted_;
    WidgetId root_, focused_, hovered_, pressed_, default_focus_;
    Skin skin_;
    rhi::Extent2D viewport_{};
    SafeInsets safe_{};
    f32 scale_ = 1.0f;
    f32 text_scale_ = 1.0f;     // UpdateContext::text_scale
    bool high_contrast_ = false;
    bool layout_dirty_ = true, order_dirty_ = true, modal_ = true;
    vec2 prev_nav_;             // last frame's quantized direction (edges + key repeat)
    f64 nav_repeat_at_ = 0.0;
    f64 time_ = 0.0;
    bool pointer_held_ = false;
    // A finger or mouse drag on a scroll view: armed by a press, scrolling past the slop.
    u32 drag_scroll_ = ~0u;
    vec2 drag_start_;
    f32 drag_origin_ = 0.0f;
    bool dragging_ = false;
    mutable DocumentStats stats_;
    mutable render::TextLayout scratch_;

public:
    ~Document();
};

// Builder sugar over Document (ui.md §5.4; .mcui assets are the data form, mce/ui/document_data.h).
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
    Builder scroll(StringView name, const StackLayout& s = {});  // a vertical list that scrolls
    // A modal: a full-screen veil (dim, blocks the pointer, hidden until Document::open_modal);
    // put its box inside it.
    Builder modal(StringView name);
    Builder tabs(StringView name);                     // then .tab(...) per page
    Builder tab(StringView name, StringView label);    // on a tabs builder: the page to fill

    Builder& fill();                                  // anchors 0..1, zero offsets
    Builder& at(vec2 anchor, vec2 size);              // point anchor + size (0 on an axis = preferred)
    Builder& anchors(const Anchors& a);
    Builder& style(const Style& s);                   // every state
    Builder& style(WidgetState state, const Style& s);
    Builder& tint(Color c);                           // every state's tint
    Builder& slice(const Slice& s);                   // every state's 9-slice insets
    Builder& to(AnchorTo space);                      // what the anchors are fractions of
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
