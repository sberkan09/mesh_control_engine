#include "mce/ui/ui.h"

#include <cmath>
#include <cstdio>

#include "mce/convar.h"
#include "mce/log.h"
#include "mce/render/renderer.h"
#include "mce/ui/document_data.h"
#include "mce/ui/minimap.h"

namespace mce::ui {
namespace {

#include "builtin_font_data.inc"

constexpr u32 kNone = ~0u;
constexpr u32 kConsoleScrollback = 256;
constexpr u32 kConsoleHistory = 32;
constexpr f64 kRepeatDelay = 0.4, kRepeatInterval = 0.12;
// Scroll views: a press turns into a drag after this many pixels (at UI scale 1); a released drag
// keeps its speed and loses kScrollFriction of it per second (e^-k); a wheel notch moves kWheelStep.
constexpr f32 kDragSlop = 8.0f, kScrollFriction = 4.0f, kWheelStep = 48.0f, kMinScrollSpeed = 8.0f;

constexpr u32 S(WidgetState s) { return static_cast<u32>(s); }

f32 snap(f32 v) { return std::floor(v + 0.5f); }
Rect snap(const Rect& r) { return Rect{vec2(snap(r.min.x), snap(r.min.y)), vec2(snap(r.max.x), snap(r.max.y))}; }
Rect intersect(const Rect& a, const Rect& b) {
    Rect r{vec2(a.min.x > b.min.x ? a.min.x : b.min.x, a.min.y > b.min.y ? a.min.y : b.min.y),
           vec2(a.max.x < b.max.x ? a.max.x : b.max.x, a.max.y < b.max.y ? a.max.y : b.max.y)};
    if (r.max.x < r.min.x) r.max.x = r.min.x;
    if (r.max.y < r.min.y) r.max.y = r.min.y;
    return r;
}
bool overlaps(const Rect& a, const Rect& b) { return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y; }
f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Styles are authored in sRGB (what a design tool or a CSS color shows); the batch takes linear.
f32 to_linear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
Color linear(Color c) { return Color{to_linear(c.r), to_linear(c.g), to_linear(c.b), c.a}; }

bool interactive_kind(WidgetKind k) {
    return k == WidgetKind::Button || k == WidgetKind::Toggle || k == WidgetKind::Slider || k == WidgetKind::Console;
}

// The direction of a stick / d-pad vector in screen space (+y down), one axis at a time.
vec2 quantize(vec2 v) {
    constexpr f32 kDead = 0.5f;
    if (std::fabs(v.x) < kDead && std::fabs(v.y) < kDead) return {};
    if (std::fabs(v.x) >= std::fabs(v.y)) return {v.x > 0 ? 1.0f : -1.0f, 0.0f};
    return {0.0f, v.y > 0 ? -1.0f : 1.0f};
}

}  // namespace

// ---- console storage --------------------------------------------------------------------------

struct Document::ConsoleData {
    struct Line {
        FixedString<192> text;
        Color color;
    };
    Array<Line> lines;  // ring of kConsoleScrollback
    u32 head = 0;       // oldest line when full
    FixedString<256> input;
    Array<FixedString<256>> history;
    i32 history_pos = -1;  // -1 = editing a fresh line

    u32 count() const { return static_cast<u32>(lines.size()); }
    const Line& at(u32 i) const { return lines[(head + i) % lines.size()]; }
    void push(StringView s, Color c) {
        if (lines.size() < kConsoleScrollback) {
            lines.push_back(Line{FixedString<192>(s), c});
            return;
        }
        lines[head] = Line{FixedString<192>(s), c};
        head = (head + 1) % kConsoleScrollback;
    }
};

// ---- built-in font -----------------------------------------------------------------------------

void builtin_font(render::FontMetrics& metrics, Array<render::FontGlyph>& glyphs, Array<u8>& atlas) {
    // One 16×6 grid of 8×14 cells (a texel of padding right and below each glyph).
    constexpr u32 kCols = 16, kCellW = kBuiltinW + 1, kCellH = kBuiltinH + 1;
    constexpr u32 kCount = 95;
    constexpr u32 kRowsN = (kCount + kCols - 1) / kCols;
    const u32 aw = kCols * kCellW, ah = kRowsN * kCellH;
    atlas.clear();
    atlas.resize(usize(aw) * ah, 0);
    glyphs.clear();
    for (u32 g = 0; g < kCount; ++g) {
        const u32 cx = (g % kCols) * kCellW, cy = (g / kCols) * kCellH;
        for (u32 y = 0; y < kBuiltinH; ++y)
            for (u32 x = 0; x < kBuiltinW; ++x)
                if (kBuiltinRows[g][y] & (0x40u >> x)) atlas[usize(cy + y) * aw + cx + x] = 255;
        render::FontGlyph fg{};
        fg.codepoint = 32 + g;
        fg.x0 = static_cast<u16>(cx);
        fg.y0 = static_cast<u16>(cy);
        fg.x1 = static_cast<u16>(cx + kBuiltinW);
        fg.y1 = static_cast<u16>(cy + kBuiltinH);
        fg.xoff = 0.0f;
        fg.yoff = -static_cast<f32>(kBuiltinAscent);
        fg.xoff2 = static_cast<f32>(kBuiltinW);
        fg.yoff2 = static_cast<f32>(kBuiltinH) - static_cast<f32>(kBuiltinAscent);
        fg.xadvance = static_cast<f32>(kBuiltinAdvance);
        glyphs.push_back(fg);
    }
    metrics = {};
    metrics.pixel_height = static_cast<f32>(kBuiltinH);
    metrics.ascent = static_cast<f32>(kBuiltinAscent);
    metrics.descent = -static_cast<f32>(kBuiltinDescent);
    metrics.line_gap = 0.0f;
    metrics.atlas_width = aw;
    metrics.atlas_height = ah;
}

Result<BuiltinSkin*> create_builtin_skin(render::Renderer& renderer, BuiltinSkin& out) {
    render::FontMetrics m;
    Array<render::FontGlyph> glyphs;
    Array<u8> atlas;
    builtin_font(m, glyphs, atlas);
    MCE_TRY_ASSIGN(out.font, renderer.create_font(m, glyphs.span(), {}, Span<const byte>(reinterpret_cast<const byte*>(atlas.data()), atlas.size()),
                                                  "ui.builtin_font"));

    // A 4×4 white texture: the flat region every panel, button and track draws with.
    static constexpr u8 kWhite[4 * 4 * 4] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                             255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                             255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                             255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    const u32 key = renderer.textures().allocate_key();
    const render::TextureMipUpload mip{4, 4, Span<const byte>(reinterpret_cast<const byte*>(kWhite), sizeof(kWhite))};
    render::TextureUpload up;
    up.mips = Span<const render::TextureMipUpload>(&mip, 1);
    up.debug_name = "ui.white";
    MCE_TRY(renderer.textures().upload(key, 1, up));
    const render::SpriteRegion region{0, 0.25f, 0.25f, 0.75f, 0.75f, 1.0f, 1.0f};
    render::SpriteSetDesc sd;
    sd.texture_key = key;
    sd.regions = Span<const render::SpriteRegion>(&region, 1);
    sd.filter = rhi::Filter::Nearest;
    sd.debug_name = "ui.skin";
    out.skin.set = renderer.create_sprite_set(sd);
    if (!out.skin.set.valid()) return Error(Errc::OutOfMemory, "ui.skin.set", "");
    out.skin.white_region = 0;
    out.skin.font = &out.font;
    return &out;
}

// ---- UI scale ----------------------------------------------------------------------------------

f32 compute_ui_scale(rhi::Extent2D viewport, f32 display_scale, f32 user_scale, vec2 min_logical, f32 reference_height) {
    const f32 user = user_scale > 0.0f ? user_scale : 1.0f;
    f32 s = (display_scale > 0.0f ? display_scale : 1.0f) * user;
    const f32 w = static_cast<f32>(viewport.width), h = static_cast<f32>(viewport.height);
    const f32 short_px = w < h ? w : h, long_px = w < h ? h : w;
    // A game authored for a reference height grows with the window: the short side over it, on top of the
    // user's setting. A phone whose display scale already asks for more keeps that (no double scaling).
    if (reference_height > 0.0f && short_px > 0.0f && short_px / reference_height * user > s) s = short_px / reference_height * user;
    const f32 short_min = min_logical.x < min_logical.y ? min_logical.x : min_logical.y;
    const f32 long_min = min_logical.x < min_logical.y ? min_logical.y : min_logical.x;
    if (short_min > 0.0f && short_px > 0.0f && short_px / short_min < s) s = short_px / short_min;
    if (long_min > 0.0f && long_px > 0.0f && long_px / long_min < s) s = long_px / long_min;
    s = std::floor(s * 8.0f) / 8.0f;
    return s < 0.25f ? 0.25f : s;
}

// ---- Document: structure -----------------------------------------------------------------------

Document::Document(mem::IAllocator& a)
    : alloc_(a), nodes_(a), free_(a), order_(a), modal_starts_(a), pending_remove_(a), events_(a), consoles_(a), scratch_(a) {
    nodes_.emplace_back();
    Node& r = nodes_[0];
    r.alive = true;
    r.name.assign("root");
    r.anchors.max = vec2(1, 1);
    for (Style& s : r.styles) s.draw_quad = false;
    root_ = id_of(0);
}

Document::~Document() {
    for (ConsoleData* c : consoles_)
        if (c) mem::destroy(alloc_, c);
}

Document::Node* Document::node(WidgetId w) {
    if (w.index >= nodes_.size()) return nullptr;
    Node& n = nodes_[w.index];
    return n.alive && n.generation == w.generation ? &n : nullptr;
}

const Document::Node* Document::node(WidgetId w) const {
    if (w.index >= nodes_.size()) return nullptr;
    const Node& n = nodes_[w.index];
    return n.alive && n.generation == w.generation ? &n : nullptr;
}

bool Document::valid(WidgetId w) const { return node(w) != nullptr; }

WidgetId Document::add(WidgetId parent, WidgetKind kind, StringView name) {
    if (!node(parent)) {
        MCE_LOG_ERROR("ui: add '{}' under an invalid parent", name);
        return {};
    }
    u32 index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
        const u32 gen = nodes_[index].generation;
        nodes_[index] = Node{};
        nodes_[index].generation = gen;
    } else {
        index = static_cast<u32>(nodes_.size());
        nodes_.emplace_back();
    }
    Node& n = nodes_[index];
    n.alive = true;
    n.kind = kind;
    n.name.assign(name);
    if (kind == WidgetKind::Stack || kind == WidgetKind::Grid || kind == WidgetKind::Text || kind == WidgetKind::Scroll ||
        kind == WidgetKind::Tabs || kind == WidgetKind::Minimap)
        for (Style& s : n.styles) s.draw_quad = false;
    if (kind == WidgetKind::Console) {
        ConsoleData* c = mem::make_unique<ConsoleData>(alloc_).release();
        n.console = static_cast<u32>(consoles_.size());
        for (u32 i = 0; i < consoles_.size(); ++i)
            if (!consoles_[i]) {
                n.console = i;
                break;
            }
        if (n.console == consoles_.size()) consoles_.push_back(c);
        else consoles_[n.console] = c;
    }
    Node& p = nodes_[parent.index];
    n.parent = parent.index;
    n.prev_sibling = p.last_child;
    if (p.last_child != kNone) nodes_[p.last_child].next_sibling = index;
    else p.first_child = index;
    p.last_child = index;
    order_dirty_ = true;
    mark_layout();
    ++stats_.widgets;
    return id_of(index);
}

void Document::remove(WidgetId w) {
    if (node(w) && w.index != root_.index) pending_remove_.push_back(w);
}

void Document::unlink(u32 index) {
    Node& n = nodes_[index];
    if (n.parent == kNone) return;
    Node& p = nodes_[n.parent];
    if (n.prev_sibling != kNone) nodes_[n.prev_sibling].next_sibling = n.next_sibling;
    else p.first_child = n.next_sibling;
    if (n.next_sibling != kNone) nodes_[n.next_sibling].prev_sibling = n.prev_sibling;
    else p.last_child = n.prev_sibling;
    n.parent = n.prev_sibling = n.next_sibling = kNone;
}

void Document::destroy_subtree(u32 index) {
    for (u32 c = nodes_[index].first_child; c != kNone;) {
        const u32 next = nodes_[c].next_sibling;
        destroy_subtree(c);
        c = next;
    }
    unlink(index);
    Node& n = nodes_[index];
    if (n.console != kNone) {
        mem::destroy(alloc_, consoles_[n.console]);
        consoles_[n.console] = nullptr;
        n.console = kNone;
    }
    n.alive = false;
    n.text.clear();
    ++n.generation;
    free_.push_back(index);
    --stats_.widgets;
}

void Document::rebuild_order() {
    order_.clear();
    auto open_modal_at = [&](u32 i) {
        for (const ModalEntry& m : modals_)
            if (m.widget.index == i) return true;
        return false;
    };
    // Iterative depth-first walk: a node, then its children in sibling order. Open modals are left
    // out and walked last, in the order they opened, so they draw and hit-test above the rest.
    Array<u32> stack(alloc_);
    auto walk = [&](u32 start) {
        stack.push_back(start);
        while (!stack.empty()) {
            const u32 i = stack.back();
            stack.pop_back();
            order_.push_back(i);
            for (u32 c = nodes_[i].last_child; c != kNone; c = nodes_[c].prev_sibling)
                if (!open_modal_at(c)) stack.push_back(c);
        }
    };
    walk(root_.index);
    modal_starts_.clear();
    for (const ModalEntry& m : modals_)
        if (node(m.widget)) {
            modal_starts_.push_back(static_cast<u32>(order_.size()));
            walk(m.widget.index);
        }
    order_dirty_ = false;
}

// ---- setters / getters -------------------------------------------------------------------------

void Document::set_anchors(WidgetId w, const Anchors& a) {
    if (Node* n = node(w)) {
        n->anchors = a;
        mark_layout();
    }
}
void Document::set_style(WidgetId w, WidgetState s, const Style& st) {
    if (Node* n = node(w)) n->styles[S(s)] = st;
}
void Document::set_style_all(WidgetId w, const Style& st) {
    if (Node* n = node(w))
        for (Style& s : n->styles) s = st;
}
void Document::set_text(WidgetId w, StringView utf8) {
    Node* n = node(w);
    if (!n || StringView(n->text.data(), n->text.size()) == utf8) return;
    n->text.clear();
    for (char c : utf8) n->text.push_back(c);
    mark_layout();
}
void Document::set_value(WidgetId w, f32 v) {
    if (Node* n = node(w)) n->value = v < n->lo ? n->lo : (v > n->hi ? n->hi : v);
}
void Document::set_range(WidgetId w, f32 lo, f32 hi, f32 step) {
    if (Node* n = node(w)) {
        n->lo = lo;
        n->hi = hi > lo ? hi : lo;
        n->step = step;
        n->value = n->value < n->lo ? n->lo : (n->value > n->hi ? n->hi : n->value);
    }
}
void Document::set_visible(WidgetId w, bool v) {
    if (Node* n = node(w); n && n->visible != v) {
        n->visible = v;
        mark_layout();
    }
}
void Document::set_interactable(WidgetId w, bool v) {
    if (Node* n = node(w)) n->interactable = v;
}
void Document::set_nav(WidgetId w, const NavEdges& edges) {
    if (Node* n = node(w)) n->nav = edges;
}
void Document::set_stack(WidgetId w, const StackLayout& s) {
    if (Node* n = node(w)) {
        n->stack = s;
        mark_layout();
    }
}
void Document::set_grid(WidgetId w, const GridLayout& g) {
    if (Node* n = node(w)) {
        n->grid = g;
        mark_layout();
    }
}
void Document::set_blocks_pointer(WidgetId w, bool v) {
    if (Node* n = node(w)) n->blocks_pointer = v;
}

// ---- scroll views, modals, tabs ----------------------------------------------------------------

void Document::set_scroll(WidgetId w, f32 offset) {
    Node* n = node(w);
    if (!n || n->kind != WidgetKind::Scroll) return;
    n->scroll_velocity = 0.0f;
    if (layout_dirty_) {  // clamped by the coming layout
        n->scroll = offset > 0.0f ? offset : 0.0f;
        return;
    }
    const f32 v = clampf(offset, 0.0f, n->scroll_max);
    if (v == n->scroll) return;
    n->scroll = v;
    relayout_scroll(w.index);
}
f32 Document::scroll(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->scroll : 0.0f;
}
f32 Document::scroll_max(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->scroll_max : 0.0f;
}

void Document::relayout_scroll(u32 index) { layout_children(index, scale_); }

void Document::scroll_into_view(WidgetId w) {
    if (!node(w)) return;
    if (layout_dirty_) layout();
    for (u32 a = nodes_[w.index].parent; a != kNone; a = nodes_[a].parent) {
        Node& s = nodes_[a];
        if (s.kind != WidgetKind::Scroll) continue;
        const bool vert = s.stack.axis == Axis::Vertical;
        const Rect& r = nodes_[w.index].rect;
        const f32 lo = vert ? r.min.y : r.min.x, hi = vert ? r.max.y : r.max.x;
        const f32 pad = s.stack.padding * scale_;
        const f32 view_lo = (vert ? s.rect.min.y : s.rect.min.x) + pad, view_hi = (vert ? s.rect.max.y : s.rect.max.x) - pad;
        f32 delta = 0.0f;
        if (lo < view_lo) delta = lo - view_lo;
        else if (hi > view_hi) delta = (hi - view_hi) < (lo - view_lo) ? hi - view_hi : lo - view_lo;
        const f32 v = clampf(s.scroll + delta, 0.0f, s.scroll_max);
        s.scroll_velocity = 0.0f;
        if (v == s.scroll) continue;
        s.scroll = v;
        relayout_scroll(a);
    }
}

u32 Document::scroll_at(vec2 p) const {
    for (usize k = order_.size(); k-- > 1;) {
        const u32 i = order_[k];
        const Node& n = nodes_[i];
        if (n.kind != WidgetKind::Scroll || !n.rect.contains(p) || (n.clipped && !n.clip.contains(p))) continue;
        if (effectively_visible(i) && in_modal_scope(i)) return i;
    }
    return kNone;
}

bool Document::in_modal_scope(u32 index) const {
    if (modals_.empty()) return true;
    const u32 top = modals_.back().widget.index;
    for (u32 i = index; i != kNone; i = nodes_[i].parent)
        if (i == top) return true;
    return false;
}

void Document::open_modal(WidgetId w) {
    Node* n = node(w);
    if (!n || w == root_) return;
    for (const ModalEntry& m : modals_)
        if (m.widget == w) return;
    modals_.push_back(ModalEntry{w, focused_});
    if (!n->visible) {
        n->visible = true;
        mark_layout();
    }
    order_dirty_ = true;
    focused_ = {};  // update() focuses into the modal (its default focus, else its first widget)
}

void Document::close_modal(WidgetId w) {
    for (usize i = 0; i < modals_.size(); ++i) {
        if (modals_[i].widget != w) continue;
        const bool top = i + 1 == modals_.size();
        const WidgetId restore = modals_[i].restore_focus;
        modals_.erase(i);
        if (Node* n = node(w); n && n->visible) {
            n->visible = false;
            mark_layout();
        }
        order_dirty_ = true;
        if (top) focused_ = restore;  // checked by the next update()
        return;
    }
}

void Document::select_tab(WidgetId tabs, u32 index) {
    Node* n = node(tabs);
    if (!n || n->kind != WidgetKind::Tabs || n->first_child == kNone) return;
    const u32 pages = nodes_[n->first_child].next_sibling;
    if (pages == kNone) return;
    u32 count = 0;
    for (u32 c = nodes_[pages].first_child; c != kNone; c = nodes_[c].next_sibling) ++count;
    if (count == 0) return;
    if (index >= count) index = count - 1;
    n->value = static_cast<f32>(index);
    u32 i = 0;
    for (u32 c = nodes_[pages].first_child; c != kNone; c = nodes_[c].next_sibling, ++i) nodes_[c].visible = i == index;
    mark_layout();
}

u32 Document::selected_tab(WidgetId tabs) const {
    const Node* n = node(tabs);
    return n && n->kind == WidgetKind::Tabs ? static_cast<u32>(n->value) : 0u;
}

// A button fired (pointer release on it, or confirm while focused). A tab's button selects its page.
void Document::activate(u32 index) {
    const WidgetId id = id_of(index);
    emit(UiEvent::Confirmed, id);
    const u32 bar = nodes_[index].parent;
    const u32 tabs = bar != kNone ? nodes_[bar].parent : kNone;
    if (tabs == kNone || nodes_[tabs].kind != WidgetKind::Tabs || nodes_[tabs].first_child != bar) return;
    u32 i = 0;
    for (u32 c = nodes_[bar].first_child; c != kNone && c != index; c = nodes_[c].next_sibling) ++i;
    if (static_cast<u32>(nodes_[tabs].value) == i) return;
    select_tab(id_of(tabs), i);
    emit(UiEvent::ValueChanged, id_of(tabs), static_cast<f32>(i));
}

bool Document::bind_convar(WidgetId w, StringView convar) {
    Node* n = node(w);
    if (!n) return false;
    const ConVar* cv = ConVar::find(StringId(convar));
    if (!cv) {
        MCE_LOG_ERROR("ui: bind '{}' to unknown convar '{}'", n->name.c_str(), convar);
        return false;
    }
    if (cv->flags() & CVarFlags::Cheat) {
        MCE_LOG_ERROR("ui: convar '{}' is a cheat and cannot be bound", convar);
        return false;
    }
    n->convar.assign(convar);
    if (n->kind == WidgetKind::Toggle) n->hi = 1.0f, n->lo = 0.0f, n->step = 1.0f;
    sync_bindings();
    return true;
}

bool Document::bind_property(WidgetId w, const World& world, Entity entity, StringView path, const char* format) {
    Node* n = node(w);
    if (!n) return false;
    const usize slash = path.find('/');
    if (slash == StringView::npos) {
        MCE_LOG_ERROR("ui: property path '{}' needs '<Component>/<field>'", path);
        return false;
    }
    const refl::TypeRecord* type = refl::find_type(StringId(path.substr(0, slash)));
    if (!type || !type->is_component()) {
        MCE_LOG_ERROR("ui: '{}' is not a reflected component", path.substr(0, slash));
        return false;
    }
    Result<refl::PropertyRef> ref = refl::resolve_path(*type, path.substr(slash + 1));
    if (!ref) {
        MCE_LOG_ERROR("ui: property path '{}' does not resolve", path);
        return false;
    }
    const refl::FieldKind k = ref.value().kind;
    const bool numeric = k == refl::FieldKind::Bool || k == refl::FieldKind::Enum || (k >= refl::FieldKind::I8 && k <= refl::FieldKind::F64);  // an enum reads as its number
    if (!numeric || ref.value().count != 1) {
        MCE_LOG_ERROR("ui: property '{}' is not a numeric or bool leaf", path);
        return false;
    }
    if (format && !valid_number_format(StringView(format))) {
        MCE_LOG_ERROR("ui: '{}' is not a number format (text and one f, e or g conversion)", format);
        return false;
    }
    PropertyBinding b;
    b.widget = w;
    b.world = &world;
    b.entity = entity;
    b.type = type;
    b.ref = ref.value();
    if (format) b.format.assign(format);
    properties_.push_back(b);
    sync_properties();
    return true;
}

namespace {
f64 read_number(const void* p, refl::FieldKind k, u32 size) {
    switch (k) {
        case refl::FieldKind::Enum:  // the underlying integer (reflected enums are unsigned or small)
            return size == 1 ? *static_cast<const u8*>(p) : size == 2 ? *static_cast<const u16*>(p) : size == 4 ? *static_cast<const u32*>(p) : 0.0;
        case refl::FieldKind::Bool: return *static_cast<const bool*>(p) ? 1.0 : 0.0;
        case refl::FieldKind::I8: return *static_cast<const i8*>(p);
        case refl::FieldKind::I16: return *static_cast<const i16*>(p);
        case refl::FieldKind::I32: return *static_cast<const i32*>(p);
        case refl::FieldKind::I64: return static_cast<f64>(*static_cast<const i64*>(p));
        case refl::FieldKind::U8: return *static_cast<const u8*>(p);
        case refl::FieldKind::U16: return *static_cast<const u16*>(p);
        case refl::FieldKind::U32: return *static_cast<const u32*>(p);
        case refl::FieldKind::U64: return static_cast<f64>(*static_cast<const u64*>(p));
        case refl::FieldKind::F32: return *static_cast<const f32*>(p);
        case refl::FieldKind::F64: return *static_cast<const f64*>(p);
        default: break;
    }
    return 0.0;
}
}  // namespace

void Document::sync_properties() {
    for (usize i = 0; i < properties_.size();) {
        PropertyBinding& b = properties_[i];
        Node* n = node(b.widget);
        if (!n) {  // the widget was removed: drop its binding
            properties_.erase_swap(i);
            continue;
        }
        ++i;
        // The component is registered lazily (first spawn), so resolve until it exists.
        if (b.component == ecs::kInvalidComponent) b.component = b.world->find_component_by_refl(b.type->id.value);
        if (b.component == ecs::kInvalidComponent) continue;
        const void* obj = b.world->get_raw(b.entity, b.component);
        if (!obj) continue;
        const f64 v = read_number(static_cast<const byte*>(obj) + b.ref.offset, b.ref.kind, b.ref.size);
        if (b.has_last && v == b.last) continue;
        b.last = v;
        b.has_last = true;
        ++stats_.binding_updates;
        if (n->kind == WidgetKind::Text) {
            char text[96];
            std::snprintf(text, sizeof(text), b.format.empty() ? "%g" : b.format.c_str(), v);
            set_text(b.widget, text);
        } else if (n->kind == WidgetKind::Panel || n->kind == WidgetKind::Image) {
            set_visible(b.widget, static_cast<f32>(v) >= n->lo && static_cast<f32>(v) <= n->hi);  // shown while lo <= value <= hi: a wanted star, a mission state
        } else {
            n->value = static_cast<f32>(v) < n->lo ? n->lo : (static_cast<f32>(v) > n->hi ? n->hi : static_cast<f32>(v));
        }
    }
}

WidgetKind Document::kind(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->kind : WidgetKind::Panel;
}
const Style& Document::style(WidgetId w, WidgetState s) const {
    const Node* n = node(w);
    return n ? n->styles[S(s)] : nodes_[0].styles[0];
}
const Anchors& Document::anchors(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->anchors : nodes_[0].anchors;
}
Rect Document::rect(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->rect : Rect{};
}
StringView Document::text(WidgetId w) const {
    const Node* n = node(w);
    return n ? StringView(n->text.data(), n->text.size()) : StringView();
}
StringView Document::name(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->name.view() : StringView();
}
f32 Document::value(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->value : 0.0f;
}
bool Document::visible(WidgetId w) const {
    const Node* n = node(w);
    return n && effectively_visible(w.index);
}
WidgetId Document::parent(WidgetId w) const {
    const Node* n = node(w);
    return n && n->parent != kNone ? id_of(n->parent) : WidgetId{};
}

bool Document::visible_self(WidgetId w) const {
    const Node* n = node(w);
    return n && n->visible;
}
bool Document::interactable(WidgetId w) const {
    const Node* n = node(w);
    return n && n->interactable;
}
bool Document::blocks_pointer(WidgetId w) const {
    const Node* n = node(w);
    return n && n->blocks_pointer;
}
NavEdges Document::nav(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->nav : NavEdges{};
}
StackLayout Document::stack(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->stack : StackLayout{};
}
GridLayout Document::grid(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->grid : GridLayout{};
}
void Document::range(WidgetId w, f32& lo, f32& hi, f32& step) const {
    const Node* n = node(w);
    lo = n ? n->lo : 0.0f;
    hi = n ? n->hi : 1.0f;
    step = n ? n->step : 0.1f;
}
StringView Document::convar(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->convar.view() : StringView();
}
StringView Document::action(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->action.view() : StringView();
}
FontRole Document::font_role(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->font_role : FontRole::Auto;
}
void Document::set_font_role(WidgetId w, FontRole role) {
    if (Node* n = node(w)) {
        n->font_role = role;
        mark_layout();  // another font measures differently
    }
}
TextAlign Document::text_align(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->text_align : TextAlign::Left;
}
void Document::set_text_align(WidgetId w, TextAlign align) {
    if (Node* n = node(w)) n->text_align = align;
}
u8 Document::own_style(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->own_style : u8(0);
}
void Document::set_own_style(WidgetId w, u8 own) {
    if (Node* n = node(w)) n->own_style = own;
}
void Document::set_minimap(WidgetId w, const Minimap& m) {
    if (Node* n = node(w)) n->minimap = m;
}
const Minimap& Document::minimap(WidgetId w) const {
    static const Minimap none;
    const Node* n = node(w);
    return n ? n->minimap : none;
}
void Document::set_minimap_view(WidgetId w, const MinimapView& view, Span<const MinimapMarker> markers) {
    Node* n = node(w);
    if (!n) return;
    n->minimap_view = view;
    n->minimap_markers.clear();
    for (const MinimapMarker& m : markers) n->minimap_markers.push_back(m);
}
const render::Font* Document::node_font(const Node& n) const { return skin_.font_for(resolve_font_role(n.kind, n.font_role)); }
StringView Document::binding(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->binding.view() : StringView();
}
StringView Document::binding_format(WidgetId w) const {
    const Node* n = node(w);
    return n ? n->binding_format.view() : StringView();
}
void Document::set_action(WidgetId w, StringView action) {
    if (Node* n = node(w)) n->action.assign(action);
}
void Document::set_binding(WidgetId w, StringView path, StringView format) {
    if (Node* n = node(w)) {
        n->binding.assign(path);
        n->binding_format.assign(format);
        n->binding_done = false;
    }
}
bool Document::binding_done(WidgetId w) const {
    const Node* n = node(w);
    return n && n->binding_done;
}
void Document::set_binding_done(WidgetId w) {
    if (Node* n = node(w)) n->binding_done = true;
}
WidgetId Document::first_child(WidgetId w) const {
    const Node* n = node(w);
    return n && n->first_child != kNone ? id_of(n->first_child) : WidgetId{};
}
WidgetId Document::next_sibling(WidgetId w) const {
    const Node* n = node(w);
    return n && n->next_sibling != kNone ? id_of(n->next_sibling) : WidgetId{};
}

WidgetState Document::state(WidgetId w) const {
    const Node* n = node(w);
    if (!n) return WidgetState::Normal;
    if (!n->interactable) return WidgetState::Disabled;
    if (pressed_ == w && pointer_held_ && hovered_ == w) return WidgetState::Pressed;
    if (focused_ == w) return WidgetState::Focused;
    if (hovered_ == w) return WidgetState::Hover;
    return WidgetState::Normal;
}

WidgetId Document::find_child(WidgetId parent, StringView name) const {
    const Node* p = node(parent);
    if (!p) return {};
    for (u32 c = p->first_child; c != kNone; c = nodes_[c].next_sibling)
        if (nodes_[c].name == name) return id_of(c);
    return {};
}

WidgetId Document::find(StringView path) const {
    WidgetId at = root_;
    while (!path.empty() && !at.is_null()) {
        const usize slash = path.find('/');
        const StringView part = path.substr(0, slash);
        at = find_child(at, part);
        path = slash == StringView::npos ? StringView() : path.substr(slash + 1);
    }
    return at;
}

bool Document::effectively_visible(u32 index) const {
    for (u32 i = index; i != kNone; i = nodes_[i].parent)
        if (!nodes_[i].visible) return false;
    return true;
}

bool Document::focusable(u32 index) const {
    const Node& n = nodes_[index];
    return n.alive && interactive_kind(n.kind) && n.interactable && effectively_visible(index) && in_modal_scope(index);
}

// ---- layout ------------------------------------------------------------------------------------

namespace {
f32 text_px(const Style& s, const render::Font* font) {
    if (s.text_size > 0.0f) return s.text_size;
    return font ? font->face.metrics().pixel_height : 13.0f;
}
}  // namespace

vec2 Document::preferred_size(WidgetId w) const { return node(w) ? measure(w.index, 0.0f) : vec2(); }

namespace {
// Columns of a grid `width` pixels wide (UI scale 1): the authored count, or as many as fit.
u32 grid_columns(const GridLayout& g, f32 width, u32 count) {
    if (g.columns) return g.columns;
    if (width <= 0.0f) return count ? count : 1;
    const f32 step = g.cell.x + g.spacing.x;
    const u32 fit = step > 0.0f ? static_cast<u32>((width + g.spacing.x) / step) : 1;
    return fit ? fit : 1;
}
}  // namespace

vec2 Document::measure(u32 index, f32 avail_width) const {
    const Node* n = &nodes_[index];
    const Style& st = n->styles[S(WidgetState::Normal)];
    const render::Font* font = node_font(*n);
    const f32 text_size = text_px(st, font) * text_scale_;
    auto measure_text = [&](StringView s) -> vec2 {
        if (!font || s.empty()) return vec2(0, s.empty() ? 0.0f : text_size);
        render::TextLayoutDesc d;
        d.size_px = text_size;
        d.letter_spacing = st.letter_spacing;
        return render::measure_text(font->face, s, d);
    };
    const StringView label(n->text.data(), n->text.size());
    switch (n->kind) {
        case WidgetKind::Text: return measure_text(label);
        case WidgetKind::Button: {
            const vec2 t = measure_text(label);
            return vec2(t.x + 32.0f, t.y + 14.0f);
        }
        case WidgetKind::Toggle: {
            const vec2 t = measure_text(label);
            const f32 box = text_size + 4.0f;
            return vec2(box + 10.0f + t.x + 16.0f, (t.y > box ? t.y : box) + 10.0f);
        }
        case WidgetKind::Slider: return vec2(200.0f, 24.0f);
        case WidgetKind::Console: return vec2(640.0f, 300.0f);
        case WidgetKind::Stack:
        case WidgetKind::Scroll: {
            const StackLayout& s = n->stack;
            const bool vert = s.axis == Axis::Vertical;
            // A vertical stretch stack hands its width down (a fit-width grid inside sizes by it).
            const f32 child_avail = vert && s.align == Align::Stretch && avail_width > 0.0f ? avail_width - 2.0f * s.padding : 0.0f;
            f32 main = 0.0f, cross = 0.0f;
            u32 count = 0;
            for (u32 c = n->first_child; c != kNone; c = nodes_[c].next_sibling) {
                if (!nodes_[c].visible) continue;
                const Anchors& a = nodes_[c].anchors;
                vec2 sz = measure(c, child_avail);
                const vec2 off = a.offset_max - a.offset_min;
                if (off.x > 0.0f) sz.x = off.x;
                if (off.y > 0.0f) sz.y = off.y;
                main += vert ? sz.y : sz.x;
                const f32 cr = vert ? sz.x : sz.y;
                cross = cr > cross ? cr : cross;
                ++count;
            }
            if (count > 1) main += s.spacing * static_cast<f32>(count - 1);
            main += 2.0f * s.padding;
            cross += 2.0f * s.padding;
            return vert ? vec2(cross, main) : vec2(main, cross);
        }
        case WidgetKind::Grid: {
            const GridLayout& g = n->grid;
            u32 count = 0;
            for (u32 c = n->first_child; c != kNone; c = nodes_[c].next_sibling) count += nodes_[c].visible ? 1u : 0u;
            if (count == 0) return {};
            const u32 cols = grid_columns(g, avail_width, count);
            const u32 used_cols = count < cols ? count : cols;
            const u32 rows = (count + cols - 1) / cols;
            return vec2(g.cell.x * static_cast<f32>(used_cols) + g.spacing.x * static_cast<f32>(used_cols - 1),
                        g.cell.y * static_cast<f32>(rows) + g.spacing.y * static_cast<f32>(rows - 1));
        }
        case WidgetKind::Tabs: return n->first_child != kNone ? measure(n->first_child, avail_width) : vec2();
        case WidgetKind::Minimap: return vec2(160.0f, 160.0f);
        case WidgetKind::Panel:
        case WidgetKind::Image: break;
    }
    return {};
}

void Document::layout() {
    // The root is the safe part of the screen: anchors to an edge stay clear of a notch.
    Node& root = nodes_[root_.index];
    root.rect = Rect{vec2(safe_.left, safe_.top), vec2(static_cast<f32>(viewport_.width) - safe_.right,
                                                       static_cast<f32>(viewport_.height) - safe_.bottom)};
    root.clip = Rect{vec2(0, 0), vec2(static_cast<f32>(viewport_.width), static_cast<f32>(viewport_.height))};
    root.clipped = false;
    layout_children(root_.index, scale_);
    layout_dirty_ = false;
    ++stats_.layout_passes;
}

Rect Document::anchor_space(const Node& n, const Rect& parent) const {
    switch (n.anchors.to) {
        case AnchorTo::Parent: break;
        case AnchorTo::SafeArea: return nodes_[root_.index].rect;
        case AnchorTo::Screen: return nodes_[root_.index].clip;
    }
    return parent;
}

// Places `index` inside `parent` by its anchors (uGUI rule), then its children.
void Document::layout_node(u32 index, const Rect& parent, f32 scale) {
    Node& n = nodes_[index];
    const Anchors& a = n.anchors;
    const Rect space = anchor_space(n, parent);
    vec2 lo = space.min + space.size() * a.min + a.offset_min * scale;
    vec2 hi = space.min + space.size() * a.max + a.offset_max * scale;
    // A point anchor with no authored size on an axis takes the preferred size, around the pivot.
    const bool auto_x = a.min.x == a.max.x && a.offset_min.x == a.offset_max.x;
    const bool auto_y = a.min.y == a.max.y && a.offset_min.y == a.offset_max.y;
    if (auto_x || auto_y) {
        const vec2 pref = measure(index, auto_x ? 0.0f : (hi.x - lo.x) / scale) * scale;
        if (auto_x) {
            lo.x -= a.pivot.x * pref.x;
            hi.x = lo.x + pref.x;
        }
        if (auto_y) {
            lo.y -= a.pivot.y * pref.y;
            hi.y = lo.y + pref.y;
        }
    }
    n.rect = snap(Rect{lo, hi});
    layout_children(index, scale);
}

// A stack, or a scroll view's content: children one after another along the axis. A scroll view
// starts them `scroll` pixels back and learns how far it can scroll.
void Document::layout_stack(u32 index, f32 scale) {
    Node& n = nodes_[index];
    const Rect r = n.rect;
    const StackLayout s = n.stack;
    const bool vert = s.axis == Axis::Vertical;
    const bool scrolls = n.kind == WidgetKind::Scroll;
    const f32 pad = s.padding * scale;
    const f32 cross_size = (vert ? r.size().x : r.size().y) - 2.0f * pad;
    const f32 origin = vert ? r.min.y : r.min.x;
    f32 cursor = origin + pad - (scrolls ? n.scroll : 0.0f);
    bool any = false;
    for (u32 c = n.first_child; c != kNone; c = nodes_[c].next_sibling) {
        Node& ch = nodes_[c];
        if (!ch.visible) {
            ch.rect = Rect{r.min, r.min};
            continue;
        }
        vec2 sz = measure(c, vert ? cross_size / scale : 0.0f);
        const vec2 off = ch.anchors.offset_max - ch.anchors.offset_min;
        if (off.x > 0.0f) sz.x = off.x;
        if (off.y > 0.0f) sz.y = off.y;
        sz = sz * scale;
        f32& main = vert ? sz.y : sz.x;
        f32& cross = vert ? sz.x : sz.y;
        f32 cross_at = (vert ? r.min.x : r.min.y) + pad;
        switch (s.align) {
            case Align::Stretch: cross = cross_size; break;
            case Align::Center: cross_at += (cross_size - cross) * 0.5f; break;
            case Align::End: cross_at += cross_size - cross; break;
            case Align::Start: break;
        }
        const vec2 lo = vert ? vec2(cross_at, cursor) : vec2(cursor, cross_at);
        nodes_[c].rect = snap(Rect{lo, lo + sz});
        cursor += main + s.spacing * scale;
        any = true;
        layout_children(c, scale);
    }
    if (!scrolls) return;
    Node& sn = nodes_[index];
    const f32 content = (cursor - (any ? s.spacing * scale : 0.0f) + pad) - (origin - sn.scroll);
    const f32 view = vert ? r.size().y : r.size().x;
    sn.scroll_max = content > view ? content - view : 0.0f;
    if (sn.scroll > sn.scroll_max) {  // the content shrank: settle at the end
        sn.scroll = sn.scroll_max;
        sn.scroll_velocity = 0.0f;
        layout_stack(index, scale);
    }
}

void Document::layout_children(u32 index, f32 scale) {
    Node& n = nodes_[index];
    const Rect r = n.rect;
    // Children draw inside their parent's clip; a scroll view cuts it down to its own rect.
    const bool clipped = n.clipped || n.kind == WidgetKind::Scroll;
    const Rect clip = n.kind == WidgetKind::Scroll ? intersect(n.clip, r) : n.clip;
    for (u32 c = n.first_child; c != kNone; c = nodes_[c].next_sibling) {
        nodes_[c].clip = clip;
        nodes_[c].clipped = clipped;
    }
    if (n.kind == WidgetKind::Stack || n.kind == WidgetKind::Scroll) {
        layout_stack(index, scale);
        return;
    }
    if (n.kind == WidgetKind::Grid) {
        const GridLayout g = n.grid;
        u32 count = 0;
        for (u32 c = n.first_child; c != kNone; c = nodes_[c].next_sibling) count += nodes_[c].visible ? 1u : 0u;
        const u32 cols = grid_columns(g, r.size().x / scale, count);
        u32 i = 0;
        for (u32 c = nodes_[index].first_child; c != kNone; c = nodes_[c].next_sibling) {
            Node& ch = nodes_[c];
            if (!ch.visible) {
                ch.rect = Rect{r.min, r.min};
                continue;
            }
            const u32 row_index = i / cols;
            const f32 col = static_cast<f32>(i % cols), row = static_cast<f32>(row_index);
            const vec2 lo = r.min + vec2(col * (g.cell.x + g.spacing.x), row * (g.cell.y + g.spacing.y)) * scale;
            ch.rect = snap(Rect{lo, lo + g.cell * scale});
            ++i;
            layout_children(c, scale);
        }
        return;
    }
    if (n.kind == WidgetKind::Tabs) {
        // The bar (first child) across the top at its preferred height; the pages (second) below.
        const u32 bar = n.first_child;
        const u32 pages = bar != kNone ? nodes_[bar].next_sibling : kNone;
        f32 top = r.min.y;
        if (bar != kNone) {
            const f32 h = measure(bar, r.size().x / scale).y * scale;
            nodes_[bar].rect = snap(Rect{r.min, vec2(r.max.x, r.min.y + h)});
            top = nodes_[bar].rect.max.y;
            layout_children(bar, scale);
        }
        if (pages != kNone) {
            nodes_[pages].rect = snap(Rect{vec2(r.min.x, top), r.max});
            layout_children(pages, scale);
        }
        for (u32 c = pages != kNone ? nodes_[pages].next_sibling : kNone; c != kNone; c = nodes_[c].next_sibling)
            layout_node(c, r, scale);
        return;
    }
    for (u32 c = n.first_child; c != kNone; c = nodes_[c].next_sibling) layout_node(c, r, scale);
}

// ---- input -------------------------------------------------------------------------------------

u32 Document::hit_test(vec2 p) const {
    for (usize k = order_.size(); k-- > 1;) {
        const u32 i = order_[k];
        const Node& n = nodes_[i];
        if (!n.rect.contains(p) || (n.clipped && !n.clip.contains(p)) || !effectively_visible(i) || !in_modal_scope(i)) continue;
        if (focusable(i) || n.blocks_pointer) return i;
    }
    return kNone;
}

void Document::focus(WidgetId w) {
    if (w == focused_) return;
    if (!w.is_null() && (!node(w) || !focusable(w.index))) return;
    focused_ = w;
    emit(UiEvent::FocusChanged, w);
}

WidgetId Document::navigate(vec2 dir) const {
    const Node* cur = node(focused_);
    if (!cur) return {};
    const WidgetId* edge = dir.y < 0 ? &cur->nav.up : dir.y > 0 ? &cur->nav.down : dir.x < 0 ? &cur->nav.left : &cur->nav.right;
    if (!edge->is_null() && node(*edge) && focusable(edge->index)) return *edge;
    const vec2 from = cur->rect.center();
    f32 best = 0.0f;
    u32 pick = kNone;
    for (const u32 i : order_) {
        if (i == focused_.index || !focusable(i)) continue;
        const vec2 d = nodes_[i].rect.center() - from;
        const f32 along = d.x * dir.x + d.y * dir.y;
        if (along < 0.5f) continue;  // the half-plane of the direction
        const f32 perp = std::fabs(d.x * dir.y - d.y * dir.x);
        const f32 score = along + 2.0f * perp;
        if (pick == kNone || score < best) {
            best = score;
            pick = i;
        }
    }
    return pick == kNone ? WidgetId{} : id_of(pick);
}

void Document::change_value(u32 index, f32 v) {
    Node& n = nodes_[index];
    if (n.step > 0.0f) v = n.lo + std::round((v - n.lo) / n.step) * n.step;
    v = v < n.lo ? n.lo : (v > n.hi ? n.hi : v);
    if (v == n.value) return;
    n.value = v;
    emit(UiEvent::ValueChanged, id_of(index), v);
    if (!n.convar.empty()) {
        // The one sanctioned UI -> engine write goes through the console so it is logged.
        char line[128];
        if (n.kind == WidgetKind::Toggle) std::snprintf(line, sizeof(line), "%s %s", n.convar.c_str(), v > 0.5f ? "true" : "false");
        else std::snprintf(line, sizeof(line), "%s %g", n.convar.c_str(), static_cast<double>(v));
        console::execute(line);
    }
}

void Document::sync_bindings() {
    for (u32 i = 0; i < nodes_.size(); ++i) {
        Node& n = nodes_[i];
        if (!n.alive || n.convar.empty()) continue;
        const ConVar* cv = ConVar::find(StringId(n.convar.view()));
        if (!cv) continue;
        switch (cv->type()) {
            case CVarType::Bool: n.value = cv->get_bool() ? 1.0f : 0.0f; break;
            case CVarType::Int: n.value = static_cast<f32>(cv->get_int()); break;
            case CVarType::Float: n.value = cv->get_float(); break;
            case CVarType::String: {
                const StringView s = cv->get_string();
                if (StringView(n.text.data(), n.text.size()) != s) set_text(id_of(i), s);
                break;
            }
        }
    }
}

void Document::update_console(u32 index, const UpdateContext& ctx) {
    ConsoleData& c = *consoles_[nodes_[index].console];
    for (char ch : ctx.text_input)
        if (static_cast<unsigned char>(ch) >= 32 && ch != 127) c.input.append(StringView(&ch, 1));
    if (ctx.key_backspace && !c.input.empty()) {
        // Drop one UTF-8 codepoint: continuation bytes, then the lead byte.
        usize n = c.input.size();
        while (n > 0 && (static_cast<unsigned char>(c.input.c_str()[n - 1]) & 0xC0) == 0x80) --n;
        if (n > 0) --n;
        c.input.assign(c.input.view().substr(0, n));
    }
    if ((ctx.key_up || ctx.key_down) && !c.history.empty()) {
        const i32 last = static_cast<i32>(c.history.size()) - 1;
        if (ctx.key_up) c.history_pos = c.history_pos < 0 ? last : (c.history_pos > 0 ? c.history_pos - 1 : 0);
        else c.history_pos = c.history_pos < 0 || c.history_pos >= last ? -1 : c.history_pos + 1;
        if (c.history_pos < 0) c.input.clear();
        else c.input = c.history[static_cast<usize>(c.history_pos)];
    }
    if (ctx.key_enter && !c.input.empty()) {
        submitted_.assign(c.input.view());
        if (c.history.empty() || c.history.back().view() != c.input.view()) {
            if (c.history.size() == kConsoleHistory) c.history.erase(0);
            c.history.push_back(c.input);
        }
        c.history_pos = -1;
        c.input.clear();
        emit(UiEvent::Submitted, id_of(index));
    }
}

// Scroll views: a press arms a drag on the scroll view under it; past the slop the drag scrolls
// (and the pressed widget is let go), a release keeps the speed and friction slows it; the wheel
// scrolls the view under the pointer.
void Document::update_scroll(const UpdateContext& ctx, bool has_pointer) {
    const f32 dt = static_cast<f32>(ctx.dt);
    if (ctx.pointer_pressed && has_pointer) {
        drag_scroll_ = scroll_at(ctx.pointer);
        dragging_ = false;
        if (drag_scroll_ != kNone) {
            drag_start_ = ctx.pointer;
            drag_origin_ = nodes_[drag_scroll_].scroll;
            nodes_[drag_scroll_].scroll_velocity = 0.0f;  // a touch catches a fling
        }
    }
    if (drag_scroll_ != kNone && (!nodes_[drag_scroll_].alive || nodes_[drag_scroll_].kind != WidgetKind::Scroll)) drag_scroll_ = kNone;
    if (drag_scroll_ != kNone && ctx.pointer_down && has_pointer) {
        Node& s = nodes_[drag_scroll_];
        const bool vert = s.stack.axis == Axis::Vertical;
        const f32 moved = vert ? ctx.pointer.y - drag_start_.y : ctx.pointer.x - drag_start_.x;
        if (!dragging_ && std::fabs(moved) > kDragSlop * scale_) {
            dragging_ = true;
            pressed_ = {};
            drag_start_ = ctx.pointer;  // no jump by the slop
            drag_origin_ = s.scroll;
        }
        if (dragging_) {
            const f32 now = vert ? ctx.pointer.y - drag_start_.y : ctx.pointer.x - drag_start_.x;
            const f32 v = clampf(drag_origin_ - now, 0.0f, s.scroll_max);
            const f32 speed = dt > 0.0f ? (v - s.scroll) / dt : 0.0f;
            s.scroll_velocity = 0.5f * s.scroll_velocity + 0.5f * speed;  // smoothed over the last frames
            if (v != s.scroll) {
                s.scroll = v;
                relayout_scroll(drag_scroll_);
            }
        }
    }
    if (ctx.pointer_released || !ctx.pointer_down) {
        if (drag_scroll_ != kNone && !dragging_) nodes_[drag_scroll_].scroll_velocity = 0.0f;
        drag_scroll_ = kNone;
        dragging_ = false;
    }
    if (ctx.wheel != 0.0f && has_pointer)
        if (const u32 w = scroll_at(ctx.pointer); w != kNone) {
            Node& s = nodes_[w];
            s.scroll_velocity = 0.0f;
            const f32 v = clampf(s.scroll - ctx.wheel * kWheelStep * scale_, 0.0f, s.scroll_max);
            if (v != s.scroll) {
                s.scroll = v;
                relayout_scroll(w);
            }
        }
    // Inertia: every released scroll view coasts and slows down; an end stops it.
    if (dt <= 0.0f) return;
    const f32 keep = std::exp(-kScrollFriction * dt);
    for (const u32 i : order_) {
        Node& s = nodes_[i];
        if (s.kind != WidgetKind::Scroll || s.scroll_velocity == 0.0f || (i == drag_scroll_ && dragging_)) continue;
        const f32 v = clampf(s.scroll + s.scroll_velocity * dt, 0.0f, s.scroll_max);
        s.scroll_velocity = (v <= 0.0f || v >= s.scroll_max) ? 0.0f : s.scroll_velocity * keep;
        if (std::fabs(s.scroll_velocity) < kMinScrollSpeed * scale_) s.scroll_velocity = 0.0f;
        if (v != s.scroll) {
            s.scroll = v;
            relayout_scroll(i);
        }
    }
}

void Document::update(const UpdateContext& ctx) {
    events_.clear();
    submitted_.clear();
    time_ += ctx.dt;

    // 1. Removal.
    if (!pending_remove_.empty()) {
        for (const WidgetId w : pending_remove_)
            if (node(w)) destroy_subtree(w.index);
        pending_remove_.clear();
        for (usize i = modals_.size(); i-- > 0;)
            if (!node(modals_[i].widget)) modals_.erase(i);
        order_dirty_ = true;
        mark_layout();
    }
    if (order_dirty_) rebuild_order();

    // 2. Layout, only when something changed.
    const f32 text_scale = ctx.text_scale > 0.0f ? ctx.text_scale : 1.0f;
    if (ctx.viewport.width != viewport_.width || ctx.viewport.height != viewport_.height || ctx.ui_scale != scale_ ||
        !(ctx.safe_insets == safe_) || text_scale != text_scale_) {
        viewport_ = ctx.viewport;
        safe_ = ctx.safe_insets;
        scale_ = ctx.ui_scale > 0.0f ? ctx.ui_scale : 1.0f;
        text_scale_ = text_scale;
        mark_layout();
    }
    high_contrast_ = ctx.high_contrast;
    if (layout_dirty_) layout();

    // 3. Focus: a modal document always has one (default, else the first interactable).
    if (!modal_) {
        focused_ = {};
    } else if (focused_.is_null() || !node(focused_) || !focusable(focused_.index)) {
        WidgetId pick;
        if (node(default_focus_) && focusable(default_focus_.index)) pick = default_focus_;
        else
            for (const u32 i : order_)
                if (focusable(i)) {
                    pick = id_of(i);
                    break;
                }
        focused_ = {};
        if (!pick.is_null()) focus(pick);
    }
    if (!node(pressed_)) pressed_ = {};

    // 4. Pointer: hover; a press focuses (modal) and arms the widget; a release on it activates.
    const bool has_pointer = ctx.pointer.x >= 0.0f && ctx.pointer.y >= 0.0f;
    const u32 hit = has_pointer ? hit_test(ctx.pointer) : kNone;
    hovered_ = hit != kNone ? id_of(hit) : WidgetId{};
    pointer_held_ = ctx.pointer_down;
    if (ctx.pointer_pressed && hit != kNone && focusable(hit)) {
        pressed_ = id_of(hit);
        if (modal_) focus(pressed_);
    }
    if (Node* p = node(pressed_); p && p->kind == WidgetKind::Slider && (ctx.pointer_down || ctx.pointer_pressed) && has_pointer) {
        const f32 w = p->rect.size().x;
        const f32 t = w > 0.0f ? (ctx.pointer.x - p->rect.min.x) / w : 0.0f;
        change_value(pressed_.index, p->lo + (t < 0 ? 0 : (t > 1 ? 1 : t)) * (p->hi - p->lo));
    }
    update_scroll(ctx, has_pointer);  // a drag past the slop takes the press back
    if (ctx.pointer_released && !pressed_.is_null()) {
        if (pressed_ == hovered_) {
            Node& p = nodes_[pressed_.index];
            if (p.kind == WidgetKind::Button) activate(pressed_.index);
            else if (p.kind == WidgetKind::Toggle) change_value(pressed_.index, p.value > 0.5f ? 0.0f : 1.0f);
        }
        pressed_ = {};
    }

    // 5. Actions: navigation (edge, then repeat), confirm on the pressed edge, cancel.
    if (modal_ && ctx.actions) {
        const input::InputState& in = *ctx.actions;
        const vec2 raw = in.has_action(StringId(kNavigateAction)) ? in.axis2(StringId(kNavigateAction)) : in.move;
        const vec2 dir = quantize(raw);
        bool step = false;
        if (dir.x != 0.0f || dir.y != 0.0f) {
            if (dir.x != prev_nav_.x || dir.y != prev_nav_.y) {
                step = true;
                nav_repeat_at_ = time_ + kRepeatDelay;
            } else if (time_ >= nav_repeat_at_) {
                step = true;
                nav_repeat_at_ = time_ + kRepeatInterval;
            }
        }
        prev_nav_ = dir;
        if (step && node(focused_)) {
            Node& f = nodes_[focused_.index];
            if (f.kind == WidgetKind::Slider && dir.x != 0.0f) {
                const f32 st = f.step > 0.0f ? f.step : (f.hi - f.lo) * 0.1f;
                change_value(focused_.index, f.value + st * dir.x);
            } else if (const WidgetId to = navigate(dir); !to.is_null()) {
                focus(to);
                scroll_into_view(to);
            }
        }
        if (in.confirm_pressed && node(focused_)) {
            Node& f = nodes_[focused_.index];
            if (f.kind == WidgetKind::Button) activate(focused_.index);
            else if (f.kind == WidgetKind::Toggle) change_value(focused_.index, f.value > 0.5f ? 0.0f : 1.0f);
        }
        if (in.cancel_pressed) emit(UiEvent::Cancelled, modals_.empty() ? root_ : modals_.back().widget);
    }

    // 6. The focused console edits its line.
    if (Node* f = node(focused_); f && f->kind == WidgetKind::Console) update_console(focused_.index, ctx);

    // 7. Bindings pull the convars' values (a refused write snaps back here) and the properties.
    sync_bindings();
    sync_properties();
    if (layout_dirty_) layout();  // a bound string changed a label
}

// ---- console API -------------------------------------------------------------------------------

void Document::console_push_line(WidgetId console, StringView line, Color color) {
    if (const Node* n = node(console); n && n->console != kNone) consoles_[n->console]->push(line, color);
}
void Document::console_clear(WidgetId console) {
    if (const Node* n = node(console); n && n->console != kNone) {
        consoles_[n->console]->lines.clear();
        consoles_[n->console]->head = 0;
    }
}
StringView Document::console_input(WidgetId console) const {
    const Node* n = node(console);
    return n && n->console != kNone ? consoles_[n->console]->input.view() : StringView();
}
u32 Document::console_line_count(WidgetId console) const {
    const Node* n = node(console);
    return n && n->console != kNone ? consoles_[n->console]->count() : 0;
}

// ---- drawing -----------------------------------------------------------------------------------

void Document::draw_quad(render::SpriteBatch& batch, const Rect& r, Color c, i16 layer, i32 order) const {
    if (c.a <= 0.0f || r.max.x <= r.min.x || r.max.y <= r.min.y) return;
    draw_region(batch, r, c, skin_.white_region, layer, order);
}

void Document::draw_region(render::SpriteBatch& batch, const Rect& r, Color c, u32 region, i16 layer, i32 order,
                           const Slice& slice, render::SpriteSetId set) const {
    math::aabb2 clip;
    if (draw_clip_) clip = math::aabb2{draw_clip_->min, draw_clip_->max};
    render::RectQuad q;
    q.set = set.valid() ? set : skin_.set;
    q.region = region;
    q.tint = linear(c);
    q.sort_layer = layer;
    q.sort_order = order;
    q.clip = draw_clip_ ? &clip : nullptr;
    if (slice.empty()) {
        q.rect = math::aabb2{r.min, r.max};
        batch.add_rect(q);
        ++stats_.quads;
        return;
    }
    // 9-slice: the insets in texels of the region become uv cuts; on screen they keep their size
    // times the UI scale, shrunk together when the rect is smaller than two insets.
    const render::SpriteRegion& reg = batch.sets().region(skin_.set, region);
    const f32 ppm = batch.sets().pixels_per_meter(skin_.set);
    const f32 tw = reg.width_m * ppm, th = reg.height_m * ppm;
    if (tw <= 0.0f || th <= 0.0f) return;
    const vec2 size = r.size();
    f32 l = slice.left * scale_, t = slice.top * scale_, rr = slice.right * scale_, b = slice.bottom * scale_;
    if (l + rr > size.x && l + rr > 0.0f) {
        const f32 k = size.x / (l + rr);
        l *= k, rr *= k;
    }
    if (t + b > size.y && t + b > 0.0f) {
        const f32 k = size.y / (t + b);
        t *= k, b *= k;
    }
    const f32 xs[4] = {r.min.x, r.min.x + l, r.max.x - rr, r.max.x};
    const f32 ys[4] = {r.min.y, r.min.y + t, r.max.y - b, r.max.y};
    const f32 us[4] = {0.0f, slice.left / tw, 1.0f - slice.right / tw, 1.0f};
    const f32 vs[4] = {0.0f, slice.top / th, 1.0f - slice.bottom / th, 1.0f};
    for (u32 y = 0; y < 3; ++y)
        for (u32 x = 0; x < 3; ++x) {
            if (xs[x + 1] <= xs[x] || ys[y + 1] <= ys[y]) continue;
            q.rect = math::aabb2{vec2(xs[x], ys[y]), vec2(xs[x + 1], ys[y + 1])};
            q.uv = math::aabb2{vec2(us[x], vs[y]), vec2(us[x + 1], vs[y + 1])};
            batch.add_rect(q);
            ++stats_.quads;
        }
}

void Document::draw_text(render::SpriteBatch& batch, const render::Font* font, StringView s, vec2 at, f32 size, Color c,
                         const Style& look, i16 layer, i32 order, f32 max_width, TextAlign align) const {
    if (!font || s.empty() || c.a <= 0.0f) return;
    render::TextLayoutDesc d;
    d.size_px = size;
    d.max_width = max_width;
    d.align = static_cast<render::TextAlign>(align);
    d.letter_spacing = look.letter_spacing * scale_;
    render::layout_text(font->face, s, d, scratch_);
    math::aabb2 clip;
    if (draw_clip_) clip = math::aabb2{draw_clip_->min, draw_clip_->max};
    render::TextPlacement p;
    p.origin = vec2(snap(at.x), snap(at.y));
    p.tint = linear(c);
    p.sort_layer = layer;
    p.sort_order = order;
    p.clip = draw_clip_ ? &clip : nullptr;
    p.outline_px = look.text_outline * scale_;
    p.outline_color = linear(look.text_outline_color);
    p.shadow_px = look.text_shadow * scale_;
    p.shadow_color = linear(look.text_shadow_color);
    batch.add_text(*font, scratch_, p);
    u32 copies = 1;
    if (p.outline_px > 0.0f && p.outline_color.a > 0.0f) copies += 8;
    if ((p.shadow_px.x != 0.0f || p.shadow_px.y != 0.0f) && p.shadow_color.a > 0.0f) copies += 1;
    stats_.quads += static_cast<u32>(scratch_.quads.size()) * copies;
}

void Document::submit(render::SpriteBatch& batch, i16 sort_layer) const {
    stats_.quads = 0;
    if (!skin_.set.valid()) return;
    i16 quad_layer = sort_layer, text_layer = static_cast<i16>(sort_layer + 1);
    usize next_modal = 0;
    for (usize k = 0; k < order_.size(); ++k) {
        // Each open modal draws two layers up, so its quads cover the text below it too.
        if (next_modal < modal_starts_.size() && k == modal_starts_[next_modal]) {
            ++next_modal;
            quad_layer = static_cast<i16>(sort_layer + 2 * next_modal);
            text_layer = static_cast<i16>(quad_layer + 1);
        }
        const u32 i = order_[k];
        const Node& n = nodes_[i];
        if (!n.alive || !effectively_visible(i)) continue;
        if (n.clipped && !overlaps(n.rect, n.clip)) continue;  // scrolled out of view
        draw_clip_ = n.clipped ? &n.clip : nullptr;
        const WidgetId id = id_of(i);
        const WidgetState ws = state(id);
        Style contrast;
        if (high_contrast_ && n.kind != WidgetKind::Image) contrast = high_contrast_style(n.kind, ws, n.styles[S(ws)]);
        const Style& st = high_contrast_ && n.kind != WidgetKind::Image ? contrast : n.styles[S(ws)];
        const render::Font* font = node_font(n);
        const i32 order = static_cast<i32>(k) * 4;
        const f32 px = text_px(st, font) * text_scale_ * scale_;
        const StringView label(n.text.data(), n.text.size());
        const Rect& r = n.rect;
        const u32 region = st.sprite_region == ~0u ? skin_.white_region : st.sprite_region;
        if (n.kind == WidgetKind::Image && n.image_set.valid()) {
            if (st.tint.a > 0.0f && r.max.x > r.min.x && r.max.y > r.min.y) draw_region(batch, r, st.tint, 0, quad_layer, order, {}, n.image_set);
        } else if (st.draw_quad && st.tint.a > 0.0f && r.max.x > r.min.x && r.max.y > r.min.y && n.kind != WidgetKind::Slider) {
            draw_region(batch, r, st.tint, region, quad_layer, order, st.slice);
        }
        const f32 line_h = font ? font->face.line_height() * (px / font->face.metrics().pixel_height) : px;
        switch (n.kind) {
            // One pixel of slack: the rect is snapped to whole pixels, so below UI scale 1 a Text sized to
            // its own measured width (in a horizontal Stack) could lose a fraction and wrap (SBA-61).
            case WidgetKind::Text: {
                // Center / Right place the lines across that same box, less the slack pixel.
                const f32 slack = n.text_align == TextAlign::Center ? 0.5f : n.text_align == TextAlign::Right ? 1.0f : 0.0f;
                draw_text(batch, font, label, r.min - vec2(slack, 0.0f), px, st.text_color, st, text_layer, order, r.size().x + 1.0f,
                          n.text_align);
                break;
            }
            case WidgetKind::Button: {
                if (label.empty() || !font) break;
                render::TextLayoutDesc d;
                d.size_px = px;
                d.letter_spacing = st.letter_spacing * scale_;
                const vec2 t = render::measure_text(font->face, label, d);
                draw_text(batch, font, label, r.center() - t * 0.5f, px, st.text_color, st, text_layer, order);
                // A tab's button: the selected one is underlined in the accent color.
                const u32 tabs = n.parent != kNone ? nodes_[n.parent].parent : kNone;
                if (tabs != kNone && nodes_[tabs].kind == WidgetKind::Tabs && nodes_[tabs].first_child == n.parent) {
                    u32 at = 0;
                    for (u32 c = nodes_[n.parent].first_child; c != kNone && c != i; c = nodes_[c].next_sibling) ++at;
                    if (at == static_cast<u32>(nodes_[tabs].value))
                        draw_quad(batch, Rect{vec2(r.min.x, r.max.y - 3.0f * scale_), r.max}, st.accent, quad_layer, order + 1);
                }
                break;
            }
            case WidgetKind::Toggle: {
                const f32 box = px + 4.0f * scale_;
                const f32 pad = 8.0f * scale_;
                const vec2 b0(r.min.x + pad, r.center().y - box * 0.5f);
                const Rect b{b0, b0 + vec2(box, box)};
                draw_quad(batch, b, Color{0, 0, 0, 0.55f}, quad_layer, order + 1);
                if (n.value > 0.5f) {
                    const f32 in = 3.0f * scale_;
                    draw_quad(batch, Rect{b.min + vec2(in, in), b.max - vec2(in, in)}, st.accent, quad_layer, order + 2);
                }
                draw_text(batch, font, label, vec2(b.max.x + pad, r.center().y - line_h * 0.5f), px, st.text_color, st, text_layer, order);
                break;
            }
            case WidgetKind::Slider: {
                const f32 t = n.hi > n.lo ? (n.value - n.lo) / (n.hi - n.lo) : 0.0f;
                const f32 th = 6.0f * scale_;
                const f32 cy = r.center().y;
                const Rect track{vec2(r.min.x, cy - th * 0.5f), vec2(r.max.x, cy + th * 0.5f)};
                draw_quad(batch, track, st.tint, quad_layer, order);
                const f32 x = r.min.x + r.size().x * t;
                draw_quad(batch, Rect{track.min, vec2(x, track.max.y)}, st.accent, quad_layer, order + 1);
                const f32 hw = 5.0f * scale_;
                draw_quad(batch, Rect{vec2(x - hw, r.min.y), vec2(x + hw, r.max.y)}, st.accent, quad_layer, order + 2);
                break;
            }
            case WidgetKind::Console: {
                const ConsoleData& c = *consoles_[n.console];
                const f32 pad = 6.0f * scale_;
                // The input line at the bottom, scrollback above it, newest lowest.
                f32 y = r.max.y - pad - line_h;
                FixedString<260> in("> ");
                in.append(c.input.view());
                const bool caret = focused_ == id && std::fmod(time_, 1.0) < 0.5;
                if (caret) in.append("_");
                draw_text(batch, font, in.view(), vec2(r.min.x + pad, y), px, st.text_color, st, text_layer, order);
                for (u32 l = c.count(); l-- > 0;) {
                    y -= line_h;
                    if (y < r.min.y + pad) break;
                    const ConsoleData::Line& line = c.at(l);
                    draw_text(batch, font, line.text.view(), vec2(r.min.x + pad, y), px, line.color, st, text_layer, order + 1);
                }
                break;
            }
            case WidgetKind::Minimap: draw_minimap(batch, n, st, quad_layer, order); break;
            case WidgetKind::Panel:
            case WidgetKind::Image:
            case WidgetKind::Stack:
            case WidgetKind::Grid:
            case WidgetKind::Scroll:
            case WidgetKind::Tabs: break;
        }
    }
    draw_clip_ = nullptr;
}

// The map: the background, then the markers (flat squares, or the skin region named by the marker's icon) and
// the tracked entity's arrow (a dot in the accent colour and a nose toward its heading) at the centre.
void Document::draw_minimap(render::SpriteBatch& batch, const Node& n, const Style& st, i16 layer, i32 order) const {
    const Minimap& m = n.minimap;
    const Rect& r = n.rect;
    draw_quad(batch, r, m.background, layer, order);
    const f32 px = m.marker_px * scale_;
    const vec2 half(px * 0.5f, px * 0.5f);
    Array<MinimapShown> shown;
    minimap_layout(m, n.minimap_view, n.minimap_markers.span(), r, shown);
    for (const MinimapShown& s : shown) {
        const MinimapMarker& k = n.minimap_markers[s.marker];
        // The rect clips the markers it clamps; a marker on the rim keeps its whole square inside.
        Rect q{s.point - half, s.point + half};
        if (s.on_rim) {
            const vec2 c = r.center();
            const vec2 d = c - s.point;
            const f32 len = std::sqrt(d.x * d.x + d.y * d.y);
            if (len > 0.0f) {
                const vec2 in = d * (half.x / len);
                q.min = q.min + in;
                q.max = q.max + in;
            }
        }
        if (k.icon != 0) draw_region(batch, q, k.color, k.icon, layer, order + 1);
        else draw_quad(batch, q, k.color, layer, order + 1);
    }
    if (m.full_map) {
        const MinimapPoint p = minimap_project_full(n.minimap_view.tracked, m.world_min, m.world_max, r);
        if (p.inside) draw_quad(batch, Rect{p.point - half, p.point + half}, st.accent, layer, order + 2);
        return;
    }
    const vec2 c = r.center();
    const f32 h = m.rotate_with_heading ? 0.0f : n.minimap_view.heading;
    const vec2 nose = c + vec2(-std::sin(h), -std::cos(h)) * px;
    const vec2 qh = half * 0.6f;
    draw_quad(batch, Rect{c - half, c + half}, st.accent, layer, order + 2);
    draw_quad(batch, Rect{nose - qh, nose + qh}, st.accent, layer, order + 2);
}

// ---- Builder -----------------------------------------------------------------------------------

Builder Builder::child(WidgetKind kind, StringView name) {
    const WidgetId id = doc_->add(id_, kind, name);
    WidgetData d;
    builder_defaults(kind, d);  // the same look a .mcui widget starts from
    for (u32 i = 0; i < S(WidgetState::Count); ++i) doc_->set_style(id, static_cast<WidgetState>(i), d.styles[i]);
    return Builder(*doc_, id);
}

Builder Builder::panel(StringView name) { return child(WidgetKind::Panel, name); }
u32 Document::set_image_texture(StringView name, render::SpriteSetId set) {
    u32 n = 0;
    for (Node& node : nodes_) {
        if (!node.alive || node.kind != WidgetKind::Image || node.name.view() != name) continue;
        node.image_set = set;
        ++n;
    }
    return n;
}

Builder Builder::image(StringView name, u32 sprite_region) {
    Builder b = child(WidgetKind::Image, name);
    Style s;
    s.sprite_region = sprite_region;
    doc_->set_style_all(b.id_, s);
    return b;
}
Builder Builder::text(StringView name, StringView utf8) {
    Builder b = child(WidgetKind::Text, name);
    doc_->set_text(b.id_, utf8);
    return b;
}
Builder Builder::button(StringView name, StringView label) {
    Builder b = child(WidgetKind::Button, name);
    doc_->set_text(b.id_, label);
    return b;
}
Builder Builder::toggle(StringView name, StringView label, bool on) {
    Builder b = child(WidgetKind::Toggle, name);
    doc_->set_text(b.id_, label);
    doc_->set_range(b.id_, 0.0f, 1.0f, 1.0f);
    doc_->set_value(b.id_, on ? 1.0f : 0.0f);
    return b;
}
Builder Builder::slider(StringView name, f32 lo, f32 hi, f32 step) {
    Builder b = child(WidgetKind::Slider, name);
    doc_->set_range(b.id_, lo, hi, step);
    return b;
}
Builder Builder::stack(StringView name, const StackLayout& s) {
    Builder b = child(WidgetKind::Stack, name);
    doc_->set_stack(b.id_, s);
    return b;
}
Builder Builder::grid(StringView name, const GridLayout& g) {
    Builder b = child(WidgetKind::Grid, name);
    doc_->set_grid(b.id_, g);
    return b;
}
Builder Builder::console(StringView name) { return child(WidgetKind::Console, name); }

Builder Builder::scroll(StringView name, const StackLayout& s) {
    Builder b = child(WidgetKind::Scroll, name);
    doc_->set_stack(b.id_, s);
    return b;
}
Builder Builder::modal(StringView name) {
    Builder b = child(WidgetKind::Panel, name);
    WidgetData d;
    modal_defaults(d);
    doc_->set_style_all(b.id_, d.styles[0]);
    doc_->set_anchors(b.id_, d.anchors);
    doc_->set_blocks_pointer(b.id_, d.blocks_pointer);
    doc_->set_visible(b.id_, d.visible);
    return b;
}
Builder Builder::tabs(StringView name) {
    Builder b = child(WidgetKind::Tabs, name);
    StackLayout bar;
    bar.axis = Axis::Horizontal;
    bar.spacing = 4.0f;
    b.stack("bar", bar);
    Builder pages = b.child(WidgetKind::Panel, "pages");
    Style none;
    none.draw_quad = false;
    doc_->set_style_all(pages.id_, none);
    return b;
}
Builder Builder::tab(StringView name, StringView label) {
    if (doc_->kind(id_) != WidgetKind::Tabs) {
        MCE_LOG_ERROR("ui: tab '{}' outside a tabs widget", name);
        return Builder(*doc_, WidgetId{});
    }
    const WidgetId bar = doc_->find_child(id_, "bar"), pages = doc_->find_child(id_, "pages");
    Builder(*doc_, bar).button(name, label);
    Builder page = Builder(*doc_, pages).child(WidgetKind::Panel, name);
    Style none;
    none.draw_quad = false;
    doc_->set_style_all(page.id_, none);
    page.fill();
    doc_->select_tab(id_, doc_->selected_tab(id_));  // a new page starts hidden unless it is the selected one
    return page;
}

Builder& Builder::fill() {
    Anchors a;
    a.to = doc_->anchors(id_).to;
    a.min = vec2(0, 0);
    a.max = vec2(1, 1);
    doc_->set_anchors(id_, a);
    return *this;
}
Builder& Builder::at(vec2 anchor, vec2 size) {
    Anchors a;
    a.to = doc_->anchors(id_).to;
    a.min = a.max = anchor;
    a.pivot = vec2(0.5f, 0.5f);
    a.offset_min = vec2(0.0f - size.x * 0.5f, 0.0f - size.y * 0.5f);  // +0 for a 0 size: data round-trips exactly
    a.offset_max = vec2(size.x * 0.5f, size.y * 0.5f);
    doc_->set_anchors(id_, a);
    return *this;
}
Builder& Builder::anchors(const Anchors& a) {
    doc_->set_anchors(id_, a);
    return *this;
}
Builder& Builder::style(const Style& s) {
    doc_->set_style_all(id_, s);
    return *this;
}
Builder& Builder::style(WidgetState state, const Style& s) {
    doc_->set_style(id_, state, s);
    return *this;
}
Builder& Builder::tint(Color c) {
    for (u32 i = 0; i < S(WidgetState::Count); ++i) {
        Style s = doc_->style(id_, static_cast<WidgetState>(i));
        s.tint = c;
        doc_->set_style(id_, static_cast<WidgetState>(i), s);
    }
    return *this;
}
Builder& Builder::slice(const Slice& sl) {
    for (u32 i = 0; i < S(WidgetState::Count); ++i) {
        Style s = doc_->style(id_, static_cast<WidgetState>(i));
        s.slice = sl;
        doc_->set_style(id_, static_cast<WidgetState>(i), s);
    }
    return *this;
}
Builder& Builder::to(AnchorTo space) {
    Anchors a = doc_->anchors(id_);
    a.to = space;
    doc_->set_anchors(id_, a);
    return *this;
}
Builder& Builder::text_size(f32 px) {
    for (u32 i = 0; i < S(WidgetState::Count); ++i) {
        Style s = doc_->style(id_, static_cast<WidgetState>(i));
        s.text_size = px;
        doc_->set_style(id_, static_cast<WidgetState>(i), s);
    }
    doc_->set_anchors(id_, doc_->anchors(id_));  // re-measure
    return *this;
}
Builder& Builder::nav(const NavEdges& e) {
    doc_->set_nav(id_, e);
    return *this;
}
Builder& Builder::bind_convar(StringView convar) {
    doc_->bind_convar(id_, convar);
    return *this;
}
Builder& Builder::default_focus() {
    doc_->set_default_focus(id_);
    return *this;
}
Builder& Builder::blocks_pointer() {
    doc_->set_blocks_pointer(id_, true);
    return *this;
}

void apply_default_style(Document& doc, WidgetId w) {
    WidgetData d;
    builder_defaults(WidgetKind::Button, d);
    for (u32 i = 0; i < S(WidgetState::Count); ++i) doc.set_style(w, static_cast<WidgetState>(i), d.styles[i]);
}

void apply_touch_pointer(UpdateContext& ctx, const input::InputState& state) {
    if (state.touch_count == 0) return;
    const input::TouchPoint& t = state.touches[0];
    ctx.pointer = vec2(t.pos.x * static_cast<f32>(ctx.viewport.width), t.pos.y * static_cast<f32>(ctx.viewport.height));
    ctx.pointer_down = t.held;
    ctx.pointer_pressed = t.pressed;
    ctx.pointer_released = t.released;
}

}  // namespace mce::ui
