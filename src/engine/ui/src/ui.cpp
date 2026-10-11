#include "mce/ui/ui.h"

#include <cmath>
#include <cstdio>

#include "mce/convar.h"
#include "mce/log.h"
#include "mce/render/renderer.h"

namespace mce::ui {
namespace {

#include "builtin_font_data.inc"

constexpr u32 kNone = ~0u;
constexpr u32 kConsoleScrollback = 256;
constexpr u32 kConsoleHistory = 32;
constexpr f64 kRepeatDelay = 0.4, kRepeatInterval = 0.12;

constexpr u32 S(WidgetState s) { return static_cast<u32>(s); }

f32 snap(f32 v) { return std::floor(v + 0.5f); }
Rect snap(const Rect& r) { return Rect{vec2(snap(r.min.x), snap(r.min.y)), vec2(snap(r.max.x), snap(r.max.y))}; }

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

// ---- Document: structure -----------------------------------------------------------------------

Document::Document(mem::IAllocator& a)
    : alloc_(a), nodes_(a), free_(a), order_(a), pending_remove_(a), events_(a), consoles_(a), scratch_(a) {
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
    if (kind == WidgetKind::Stack || kind == WidgetKind::Grid || kind == WidgetKind::Text)
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
    // Iterative depth-first walk: a node, then its children in sibling order.
    Array<u32> stack(alloc_);
    stack.push_back(root_.index);
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        order_.push_back(i);
        for (u32 c = nodes_[i].last_child; c != kNone; c = nodes_[c].prev_sibling) stack.push_back(c);
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
    const bool numeric = k == refl::FieldKind::Bool || (k >= refl::FieldKind::I8 && k <= refl::FieldKind::F64);
    if (!numeric || ref.value().count != 1) {
        MCE_LOG_ERROR("ui: property '{}' is not a numeric or bool leaf", path);
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
f64 read_number(const void* p, refl::FieldKind k) {
    switch (k) {
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
        const f64 v = read_number(static_cast<const byte*>(obj) + b.ref.offset, b.ref.kind);
        if (b.has_last && v == b.last) continue;
        b.last = v;
        b.has_last = true;
        ++stats_.binding_updates;
        if (n->kind == WidgetKind::Text) {
            char text[96];
            std::snprintf(text, sizeof(text), b.format.empty() ? "%g" : b.format.c_str(), v);
            set_text(b.widget, text);
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
    return n.alive && interactive_kind(n.kind) && n.interactable && effectively_visible(index);
}

// ---- layout ------------------------------------------------------------------------------------

namespace {
f32 text_px(const Style& s, const render::Font* font) {
    if (s.text_size > 0.0f) return s.text_size;
    return font ? font->face.metrics().pixel_height : 13.0f;
}
}  // namespace

vec2 Document::preferred_size(WidgetId w) const {
    const Node* n = node(w);
    if (!n) return {};
    const Style& st = n->styles[S(WidgetState::Normal)];
    auto measure = [&](StringView s) -> vec2 {
        if (!skin_.font || s.empty()) return vec2(0, s.empty() ? 0.0f : text_px(st, skin_.font));
        render::TextLayoutDesc d;
        d.size_px = text_px(st, skin_.font);
        return render::measure_text(skin_.font->face, s, d);
    };
    const StringView label(n->text.data(), n->text.size());
    switch (n->kind) {
        case WidgetKind::Text: return measure(label);
        case WidgetKind::Button: {
            const vec2 t = measure(label);
            return vec2(t.x + 32.0f, t.y + 14.0f);
        }
        case WidgetKind::Toggle: {
            const vec2 t = measure(label);
            const f32 box = text_px(st, skin_.font) + 4.0f;
            return vec2(box + 10.0f + t.x + 16.0f, (t.y > box ? t.y : box) + 10.0f);
        }
        case WidgetKind::Slider: return vec2(200.0f, 24.0f);
        case WidgetKind::Console: return vec2(640.0f, 300.0f);
        case WidgetKind::Stack: {
            const StackLayout& s = n->stack;
            const bool vert = s.axis == Axis::Vertical;
            f32 main = 0.0f, cross = 0.0f;
            u32 count = 0;
            for (u32 c = n->first_child; c != kNone; c = nodes_[c].next_sibling) {
                if (!nodes_[c].visible) continue;
                const Anchors& a = nodes_[c].anchors;
                vec2 sz = preferred_size(id_of(c));
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
            const u32 cols = g.columns ? g.columns : 1;
            const u32 used_cols = count < cols ? count : cols;
            const u32 rows = (count + cols - 1) / cols;
            if (count == 0) return {};
            return vec2(g.cell.x * static_cast<f32>(used_cols) + g.spacing.x * static_cast<f32>(used_cols - 1),
                        g.cell.y * static_cast<f32>(rows) + g.spacing.y * static_cast<f32>(rows - 1));
        }
        case WidgetKind::Panel:
        case WidgetKind::Image: break;
    }
    return {};
}

void Document::layout() {
    // The root is the safe part of the screen: anchors to an edge stay clear of a notch.
    const Rect screen{vec2(safe_.left, safe_.top), vec2(static_cast<f32>(viewport_.width) - safe_.right,
                                                         static_cast<f32>(viewport_.height) - safe_.bottom)};
    nodes_[root_.index].rect = screen;
    layout_children(root_.index, scale_);
    layout_dirty_ = false;
    ++stats_.layout_passes;
}

// Places `index` inside `parent` by its anchors (uGUI rule), then its children.
void Document::layout_node(u32 index, const Rect& parent, f32 scale) {
    Node& n = nodes_[index];
    const Anchors& a = n.anchors;
    vec2 lo = parent.min + parent.size() * a.min + a.offset_min * scale;
    vec2 hi = parent.min + parent.size() * a.max + a.offset_max * scale;
    // A point anchor with no authored size on an axis takes the preferred size, around the pivot.
    const bool auto_x = a.min.x == a.max.x && a.offset_min.x == a.offset_max.x;
    const bool auto_y = a.min.y == a.max.y && a.offset_min.y == a.offset_max.y;
    if (auto_x || auto_y) {
        const vec2 pref = preferred_size(id_of(index)) * scale;
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

void Document::layout_children(u32 index, f32 scale) {
    Node& n = nodes_[index];
    const Rect r = n.rect;
    if (n.kind == WidgetKind::Stack) {
        const StackLayout s = n.stack;
        const bool vert = s.axis == Axis::Vertical;
        const f32 pad = s.padding * scale;
        const f32 cross_size = (vert ? r.size().x : r.size().y) - 2.0f * pad;
        f32 cursor = (vert ? r.min.y : r.min.x) + pad;
        for (u32 c = nodes_[index].first_child; c != kNone; c = nodes_[c].next_sibling) {
            Node& ch = nodes_[c];
            if (!ch.visible) {
                ch.rect = Rect{r.min, r.min};
                continue;
            }
            vec2 sz = preferred_size(id_of(c));
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
            layout_children(c, scale);
        }
        return;
    }
    if (n.kind == WidgetKind::Grid) {
        const GridLayout g = n.grid;
        const u32 cols = g.columns ? g.columns : 1;
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
    for (u32 c = n.first_child; c != kNone; c = nodes_[c].next_sibling) layout_node(c, r, scale);
}

// ---- input -------------------------------------------------------------------------------------

u32 Document::hit_test(vec2 p) const {
    for (usize k = order_.size(); k-- > 1;) {
        const u32 i = order_[k];
        const Node& n = nodes_[i];
        if (!n.rect.contains(p) || !effectively_visible(i)) continue;
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

void Document::update(const UpdateContext& ctx) {
    events_.clear();
    submitted_.clear();
    time_ += ctx.dt;

    // 1. Removal.
    if (!pending_remove_.empty()) {
        for (const WidgetId w : pending_remove_)
            if (node(w)) destroy_subtree(w.index);
        pending_remove_.clear();
        order_dirty_ = true;
        mark_layout();
    }
    if (order_dirty_) rebuild_order();

    // 2. Layout, only when something changed.
    if (ctx.viewport.width != viewport_.width || ctx.viewport.height != viewport_.height || ctx.ui_scale != scale_ ||
        !(ctx.safe_insets == safe_)) {
        viewport_ = ctx.viewport;
        safe_ = ctx.safe_insets;
        scale_ = ctx.ui_scale > 0.0f ? ctx.ui_scale : 1.0f;
        mark_layout();
    }
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
    if (ctx.pointer_released && !pressed_.is_null()) {
        if (pressed_ == hovered_) {
            Node& p = nodes_[pressed_.index];
            if (p.kind == WidgetKind::Button) emit(UiEvent::Confirmed, pressed_);
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
            }
        }
        if (in.confirm_pressed && node(focused_)) {
            Node& f = nodes_[focused_.index];
            if (f.kind == WidgetKind::Button) emit(UiEvent::Confirmed, focused_);
            else if (f.kind == WidgetKind::Toggle) change_value(focused_.index, f.value > 0.5f ? 0.0f : 1.0f);
        }
        if (in.cancel_pressed) emit(UiEvent::Cancelled, root_);
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

void Document::draw_region(render::SpriteBatch& batch, const Rect& r, Color c, u32 region, i16 layer, i32 order) const {
    const render::SpriteRegion& reg = batch.sets().region(skin_.set, region);
    render::SpriteInstance s;
    s.position = r.min;
    s.pivot = vec2(0, 0);
    s.scale = vec2(r.size().x / (reg.width_m > 0 ? reg.width_m : 1.0f), r.size().y / (reg.height_m > 0 ? reg.height_m : 1.0f));
    s.tint = linear(c);
    s.set = skin_.set;
    s.region = region;
    s.sort_layer = layer;
    s.sort_order = order;
    s.flip_y = true;  // the screen view is y-down; regions are authored top-row-first
    batch.add(s);
    ++stats_.quads;
}

void Document::draw_text(render::SpriteBatch& batch, StringView s, vec2 at, f32 size, Color c, i16 layer, i32 order,
                         f32 max_width) const {
    if (!skin_.font || s.empty() || c.a <= 0.0f) return;
    render::TextLayoutDesc d;
    d.size_px = size;
    d.max_width = max_width;
    render::layout_text(skin_.font->face, s, d, scratch_);
    render::TextPlacement p;
    p.origin = vec2(snap(at.x), snap(at.y));
    p.tint = linear(c);
    p.sort_layer = layer;
    p.sort_order = order;
    batch.add_text(*skin_.font, scratch_, p);
    stats_.quads += static_cast<u32>(scratch_.quads.size());
}

void Document::submit(render::SpriteBatch& batch, i16 sort_layer) const {
    stats_.quads = 0;
    if (!skin_.set.valid()) return;
    const i16 quad_layer = sort_layer, text_layer = static_cast<i16>(sort_layer + 1);
    for (usize k = 0; k < order_.size(); ++k) {
        const u32 i = order_[k];
        const Node& n = nodes_[i];
        if (!n.alive || !effectively_visible(i)) continue;
        const WidgetId id = id_of(i);
        const Style& st = n.styles[S(state(id))];
        const i32 order = static_cast<i32>(k) * 4;
        const f32 px = text_px(st, skin_.font) * scale_;
        const StringView label(n.text.data(), n.text.size());
        const Rect& r = n.rect;
        const u32 region = st.sprite_region == ~0u ? skin_.white_region : st.sprite_region;
        if (st.draw_quad && st.tint.a > 0.0f && r.max.x > r.min.x && r.max.y > r.min.y && n.kind != WidgetKind::Slider)
            draw_region(batch, r, st.tint, region, quad_layer, order);
        const f32 line_h = skin_.font ? skin_.font->face.line_height() * (px / skin_.font->face.metrics().pixel_height) : px;
        switch (n.kind) {
            case WidgetKind::Text: draw_text(batch, label, r.min, px, st.text_color, text_layer, order, r.size().x); break;
            case WidgetKind::Button: {
                if (label.empty() || !skin_.font) break;
                render::TextLayoutDesc d;
                d.size_px = px;
                const vec2 t = render::measure_text(skin_.font->face, label, d);
                draw_text(batch, label, r.center() - t * 0.5f, px, st.text_color, text_layer, order);
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
                draw_text(batch, label, vec2(b.max.x + pad, r.center().y - line_h * 0.5f), px, st.text_color, text_layer, order);
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
                draw_text(batch, in.view(), vec2(r.min.x + pad, y), px, st.text_color, text_layer, order);
                for (u32 l = c.count(); l-- > 0;) {
                    y -= line_h;
                    if (y < r.min.y + pad) break;
                    const ConsoleData::Line& line = c.at(l);
                    draw_text(batch, line.text.view(), vec2(r.min.x + pad, y), px, line.color, text_layer, order + 1);
                }
                break;
            }
            case WidgetKind::Panel:
            case WidgetKind::Image:
            case WidgetKind::Stack:
            case WidgetKind::Grid: break;
        }
    }
}

// ---- Builder -----------------------------------------------------------------------------------

Builder Builder::child(WidgetKind kind, StringView name) {
    const WidgetId id = doc_->add(id_, kind, name);
    if (interactive_kind(kind)) apply_default_style(*doc_, id);
    return Builder(*doc_, id);
}

Builder Builder::panel(StringView name) {
    Builder b = child(WidgetKind::Panel, name);
    Style s;
    s.tint = Color{0.06f, 0.06f, 0.08f, 0.85f};
    doc_->set_style_all(b.id_, s);
    return b;
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
Builder Builder::console(StringView name) {
    Builder b = child(WidgetKind::Console, name);
    Style s;
    s.tint = Color{0.02f, 0.02f, 0.03f, 0.88f};
    s.text_color = Color{0.9f, 0.9f, 0.9f, 1};
    doc_->set_style_all(b.id_, s);
    return b;
}

Builder& Builder::fill() {
    Anchors a;
    a.min = vec2(0, 0);
    a.max = vec2(1, 1);
    doc_->set_anchors(id_, a);
    return *this;
}
Builder& Builder::at(vec2 anchor, vec2 size) {
    Anchors a;
    a.min = a.max = anchor;
    a.pivot = vec2(0.5f, 0.5f);
    a.offset_min = vec2(-size.x * 0.5f, -size.y * 0.5f);
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
    Style normal;
    normal.tint = Color{0.16f, 0.17f, 0.21f, 0.94f};
    normal.text_color = Color{0.88f, 0.88f, 0.9f, 1};
    Style hover = normal;
    hover.tint = Color{0.24f, 0.25f, 0.31f, 0.96f};
    Style pressed = normal;
    pressed.tint = Color{0.1f, 0.1f, 0.13f, 0.96f};
    Style focused = normal;
    focused.tint = Color{0.34f, 0.25f, 0.12f, 0.96f};
    focused.text_color = Color{1.0f, 0.88f, 0.55f, 1};
    Style disabled = normal;
    disabled.tint = Color{0.12f, 0.12f, 0.14f, 0.6f};
    disabled.text_color = Color{0.5f, 0.5f, 0.52f, 1};
    doc.set_style(w, WidgetState::Normal, normal);
    doc.set_style(w, WidgetState::Hover, hover);
    doc.set_style(w, WidgetState::Pressed, pressed);
    doc.set_style(w, WidgetState::Focused, focused);
    doc.set_style(w, WidgetState::Disabled, disabled);
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
