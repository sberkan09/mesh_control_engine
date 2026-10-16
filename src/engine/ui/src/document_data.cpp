// The CHNK_UIDOC codec (M-UI-DATA step 1; mce/ui/document_data.h). Needs nothing but the header,
// so the cook links it without the renderer.

#include <cstring>

#include "mce/ui/document_data.h"

namespace mce::ui {

namespace {

constexpr const char* kStage = "ui.document";

struct StrRef {
    u32 offset;
    u32 size;
};

struct UiDocHeader {
    u64 font_hi, font_lo;
    u64 sprites_hi, sprites_lo;
    u32 widget_count;
    u32 default_focus;
    u32 white_region;
    u32 strings_size;
    u8 modal;
    u8 pad[7];
    u64 theme_hi, theme_lo;  // v3
};
static_assert(sizeof(UiDocHeader) == 72);

struct StyleRecord {
    f32 tint[4];
    f32 text_color[4];
    f32 accent[4];
    u32 sprite_region;
    f32 text_size;
    f32 slice[4];
    f32 letter_spacing;
    f32 text_outline_color[4];
    f32 text_outline;
    f32 text_shadow_color[4];
    f32 text_shadow[2];
    u8 draw_quad;
    u8 pad[3];
};
static_assert(sizeof(StyleRecord) == 124);

namespace WidgetFlag {
enum : u8 { Visible = 1u << 0, Interactable = 1u << 1, BlocksPointer = 1u << 2 };
}

struct UiWidgetRecord {
    StrRef name, text, convar, action, bind, format;
    u32 parent;
    u8 kind;
    u8 flags;
    u8 anchor_to;
    u8 stack_axis;
    u8 stack_align;
    u8 font_role;  // v3: FontRole
    u8 text_align;  // TextAlign (M-UI-DATA2 1; was padding: older payloads read Left)
    u8 own_style;   // StyleOwn bits (M-UI-DATA2 2; was padding: older payloads read 0)
    f32 anchors[10];  // min, max, offset_min, offset_max, pivot
    StyleRecord styles[kWidgetStateCount];
    f32 value, lo, hi, step;
    f32 stack_spacing, stack_padding;
    u32 grid_columns;
    f32 grid_cell[2], grid_spacing[2];
    u32 nav[NavEdgeCount];
};
static_assert(sizeof(UiWidgetRecord) == 48 + 4 + 8 + 40 + 124 * 5 + 16 + 8 + 4 + 16 + 16);

void put_color(f32 (&o)[4], const Color& c) {
    o[0] = c.r;
    o[1] = c.g;
    o[2] = c.b;
    o[3] = c.a;
}
Color get_color(const f32 (&i)[4]) { return Color{i[0], i[1], i[2], i[3]}; }
void put_vec(f32* o, vec2 v) {
    o[0] = v.x;
    o[1] = v.y;
}
vec2 get_vec(const f32* i) { return vec2(i[0], i[1]); }

void put_style(StyleRecord& o, const Style& st) {
    put_color(o.tint, st.tint);
    put_color(o.text_color, st.text_color);
    put_color(o.accent, st.accent);
    o.sprite_region = st.sprite_region;
    o.text_size = st.text_size;
    o.slice[0] = st.slice.left;
    o.slice[1] = st.slice.top;
    o.slice[2] = st.slice.right;
    o.slice[3] = st.slice.bottom;
    o.letter_spacing = st.letter_spacing;
    put_color(o.text_outline_color, st.text_outline_color);
    o.text_outline = st.text_outline;
    put_color(o.text_shadow_color, st.text_shadow_color);
    put_vec(o.text_shadow, st.text_shadow);
    o.draw_quad = st.draw_quad ? 1 : 0;
}

void get_style(const StyleRecord& o, Style& st) {
    st.tint = get_color(o.tint);
    st.text_color = get_color(o.text_color);
    st.accent = get_color(o.accent);
    st.sprite_region = o.sprite_region;
    st.text_size = o.text_size;
    st.slice = Slice{o.slice[0], o.slice[1], o.slice[2], o.slice[3]};
    st.letter_spacing = o.letter_spacing;
    st.text_outline_color = get_color(o.text_outline_color);
    st.text_outline = o.text_outline;
    st.text_shadow_color = get_color(o.text_shadow_color);
    st.text_shadow = get_vec(o.text_shadow);
    st.draw_quad = o.draw_quad == 1;
}

bool style_record_ok(const StyleRecord& o) { return o.draw_quad <= 1 && o.pad[0] == 0 && o.pad[1] == 0 && o.pad[2] == 0; }

StrRef add_string(Array<byte>& blob, StringView s) {
    const StrRef r{static_cast<u32>(blob.size()), static_cast<u32>(s.size())};
    for (char c : s) blob.push_back(static_cast<byte>(c));
    return r;
}

void append(Array<byte>& out, const void* p, usize n) {
    const usize at = out.size();
    out.resize_uninitialized(at + n);
    std::memcpy(out.data() + at, p, n);
}

template <usize N>
bool read_string(const char* blob, u32 blob_size, StrRef r, FixedString<N>& out) {
    if (r.offset > blob_size || r.size > blob_size - r.offset || r.size >= N) return false;
    out.assign(StringView(blob + r.offset, r.size));
    return true;
}

bool valid_name(StringView s) { return !s.empty() && s.find('/') == StringView::npos; }

}  // namespace

void builder_defaults(WidgetKind kind, WidgetData& out) {
    const ThemeData& t = default_theme();
    for (u32 st = 0; st < kWidgetStateCount; ++st) out.styles[st] = t.styles[static_cast<u32>(kind)][st];
    if (kind == WidgetKind::Toggle) {
        out.lo = 0.0f;
        out.hi = 1.0f;
        out.step = 1.0f;
    }
}

void theme_from_palette(const Palette& p, ThemeData& out) {
    out.palette = p;
    for (u32 k = 0; k < kWidgetKindCount; ++k) {
        Style all;
        all.accent = p.accent;
        Style(&styles)[kWidgetStateCount] = out.styles[k];
        switch (static_cast<WidgetKind>(k)) {
            case WidgetKind::Button:
            case WidgetKind::Toggle:
            case WidgetKind::Slider: {
                Style normal = all;
                normal.tint = p.button;
                normal.text_color = p.text;
                Style hover = normal;
                hover.tint = p.button_hover;
                Style pressed = normal;
                pressed.tint = p.button_pressed;
                Style focused = normal;
                focused.tint = p.button_focused;
                focused.text_color = p.text_focused;
                Style disabled = normal;
                disabled.tint = p.button_disabled;
                disabled.text_color = p.text_disabled;
                styles[static_cast<u32>(WidgetState::Normal)] = normal;
                styles[static_cast<u32>(WidgetState::Hover)] = hover;
                styles[static_cast<u32>(WidgetState::Pressed)] = pressed;
                styles[static_cast<u32>(WidgetState::Focused)] = focused;
                styles[static_cast<u32>(WidgetState::Disabled)] = disabled;
                continue;
            }
            case WidgetKind::Panel: all.tint = p.panel; break;
            case WidgetKind::Console:
                all.tint = p.console;
                all.text_color = p.console_text;
                break;
            case WidgetKind::Text:
                all.draw_quad = false;
                all.text_color = p.label;
                break;
            case WidgetKind::Stack:
            case WidgetKind::Grid:
            case WidgetKind::Scroll:
            case WidgetKind::Tabs: all.draw_quad = false; break;
            case WidgetKind::Image: break;
        }
        for (Style& st : styles) st = all;
    }
}

const ThemeData& default_theme() {
    static const ThemeData t = [] {
        ThemeData d;
        theme_from_palette(Palette{}, d);
        return d;
    }();
    return t;
}

const ThemeData& high_contrast_theme() {
    static const ThemeData t = [] {
        Palette p;
        p.panel = {0, 0, 0, 1};
        p.button = {0, 0, 0, 1};
        p.button_hover = {0.2f, 0.2f, 0.2f, 1};
        p.button_pressed = {0.3f, 0.3f, 0.3f, 1};
        p.button_focused = {1, 0.9f, 0, 1};
        p.button_disabled = {0, 0, 0, 1};
        p.label = {1, 1, 1, 1};
        p.text = {1, 1, 1, 1};
        p.text_focused = {0, 0, 0, 1};
        p.text_disabled = {0.6f, 0.6f, 0.6f, 1};
        p.accent = {1, 0.9f, 0, 1};
        p.console = {0, 0, 0, 1};
        p.console_text = {1, 1, 1, 1};
        ThemeData d;
        theme_from_palette(p, d);
        return d;
    }();
    return t;
}

Style high_contrast_style(WidgetKind kind, WidgetState state, const Style& st) {
    const Style& hc = high_contrast_theme().styles[static_cast<u32>(kind)][static_cast<u32>(state)];
    Style out = st;
    out.tint = hc.tint;
    out.text_color = hc.text_color;
    out.accent = hc.accent;
    // Text over game art (labels without a quad) keeps its contrast with an outline; text on a
    // high-contrast quad already has it.
    if (!st.draw_quad || kind == WidgetKind::Text) {
        out.text_outline = st.text_outline > 1.0f ? st.text_outline : 1.0f;
        out.text_outline_color = {0, 0, 0, 1};
    }
    return out;
}

namespace {

bool same(const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
bool same(vec2 a, vec2 b) { return a.x == b.x && a.y == b.y; }
bool same(const Slice& a, const Slice& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

// Each field of `st` still at `def` takes `theme`'s.
// `own` (StyleOwn bits) names the fields the widget keeps even at the default.
void theme_style(Style& st, const Style& def, const Style& theme, u8 own) {
    auto open = [own](StyleOwn bit) { return (own & bit) == 0; };
    if (open(OwnTint) && same(st.tint, def.tint)) st.tint = theme.tint;
    if (open(OwnTextColor) && same(st.text_color, def.text_color)) st.text_color = theme.text_color;
    if (open(OwnAccent) && same(st.accent, def.accent)) st.accent = theme.accent;
    if (open(OwnSprite)) {
        if (st.sprite_region == def.sprite_region) st.sprite_region = theme.sprite_region;
        if (st.draw_quad == def.draw_quad) st.draw_quad = theme.draw_quad;
        if (same(st.slice, def.slice)) st.slice = theme.slice;
    }
    if (open(OwnTextSize) && st.text_size == def.text_size) st.text_size = theme.text_size;
    if (open(OwnLetterSpacing) && st.letter_spacing == def.letter_spacing) st.letter_spacing = theme.letter_spacing;
    if (open(OwnTextOutline)) {
        if (same(st.text_outline_color, def.text_outline_color)) st.text_outline_color = theme.text_outline_color;
        if (st.text_outline == def.text_outline) st.text_outline = theme.text_outline;
    }
    if (open(OwnTextShadow)) {
        if (same(st.text_shadow_color, def.text_shadow_color)) st.text_shadow_color = theme.text_shadow_color;
        if (same(st.text_shadow, def.text_shadow)) st.text_shadow = theme.text_shadow;
    }
}

}  // namespace

void apply_theme(const ThemeData& theme, DocumentData& d) {
    const ThemeData& def = default_theme();
    for (WidgetData& w : d.widgets) {
        const u32 k = static_cast<u32>(w.kind);
        for (u32 s = 0; s < kWidgetStateCount; ++s) theme_style(w.styles[s], def.styles[k][s], theme.styles[k][s], w.own_style);
    }
    if (d.sprites.is_null() && !theme.sprites.is_null()) {
        d.sprites = theme.sprites;
        d.white_region = theme.white_region;
    }
}

FontRole resolve_font_role(WidgetKind kind, FontRole role) {
    if (role != FontRole::Auto) return role;
    return kind == WidgetKind::Button || kind == WidgetKind::Toggle || kind == WidgetKind::Slider ? FontRole::Button : FontRole::Body;
}

void modal_defaults(WidgetData& out) {
    Style veil;
    veil.tint = Color{0.0f, 0.0f, 0.0f, 0.55f};
    for (Style& st : out.styles) st = veil;
    out.anchors = Anchors{};
    out.anchors.max = vec2(1, 1);
    out.anchors.to = AnchorTo::Screen;
    out.blocks_pointer = true;
    out.visible = false;
}

u32 DocumentData::find(StringView path) const {
    u32 parent = kNoWidgetIndex;
    while (!path.empty()) {
        const usize slash = path.find('/');
        const StringView part = path.substr(0, slash);
        u32 hit = kNoWidgetIndex;
        for (u32 i = 0; i < widgets.size(); ++i)
            if (widgets[i].parent == parent && widgets[i].name.view() == part) {
                hit = i;
                break;
            }
        if (hit == kNoWidgetIndex) return kNoWidgetIndex;
        parent = hit;
        path = slash == StringView::npos ? StringView() : path.substr(slash + 1);
    }
    return parent;
}

bool valid_number_format(StringView f) {
    u32 conversions = 0;
    for (usize i = 0; i < f.size(); ++i) {
        if (f[i] != '%') continue;
        ++i;
        if (i < f.size() && f[i] == '%') continue;
        while (i < f.size() && (f[i] == '-' || f[i] == '+' || f[i] == ' ' || f[i] == '#' || f[i] == '0')) ++i;
        for (u32 n = 0; i < f.size() && f[i] >= '0' && f[i] <= '9'; ++i)
            if (++n > 2) return false;
        if (i < f.size() && f[i] == '.') {
            ++i;
            for (u32 n = 0; i < f.size() && f[i] >= '0' && f[i] <= '9'; ++i)
                if (++n > 2) return false;
        }
        if (i >= f.size()) return false;
        const char c = f[i];
        if (c != 'f' && c != 'F' && c != 'e' && c != 'E' && c != 'g' && c != 'G') return false;
        ++conversions;
    }
    return f.empty() || conversions == 1;
}

void DocumentData::clear() {
    font = {};
    sprites = {};
    white_region = 0;
    modal = true;
    default_focus = kNoWidgetIndex;
    widgets.clear();
}

void encode_ui_document(const DocumentData& d, Array<byte>& out) {
    Array<byte> blob;
    Array<UiWidgetRecord> records;
    for (const WidgetData& w : d.widgets) {
        UiWidgetRecord r{};
        r.name = add_string(blob, w.name.view());
        r.text = add_string(blob, w.text.view());
        r.convar = add_string(blob, w.convar.view());
        r.action = add_string(blob, w.action.view());
        r.bind = add_string(blob, w.bind.view());
        r.format = add_string(blob, w.format.view());
        r.parent = w.parent;
        r.kind = static_cast<u8>(w.kind);
        r.flags =
            static_cast<u8>((w.visible ? WidgetFlag::Visible : 0) | (w.interactable ? WidgetFlag::Interactable : 0) |
                            (w.blocks_pointer ? WidgetFlag::BlocksPointer : 0));
        r.anchor_to = static_cast<u8>(w.anchors.to);
        put_vec(r.anchors + 0, w.anchors.min);
        put_vec(r.anchors + 2, w.anchors.max);
        put_vec(r.anchors + 4, w.anchors.offset_min);
        put_vec(r.anchors + 6, w.anchors.offset_max);
        put_vec(r.anchors + 8, w.anchors.pivot);
        for (u32 s = 0; s < kWidgetStateCount; ++s) put_style(r.styles[s], w.styles[s]);
        r.font_role = static_cast<u8>(w.font_role);
        r.text_align = static_cast<u8>(w.text_align);
        r.own_style = w.own_style;
        r.value = w.value;
        r.lo = w.lo;
        r.hi = w.hi;
        r.step = w.step;
        r.stack_axis = static_cast<u8>(w.stack.axis);
        r.stack_align = static_cast<u8>(w.stack.align);
        r.stack_spacing = w.stack.spacing;
        r.stack_padding = w.stack.padding;
        r.grid_columns = w.grid.columns;
        put_vec(r.grid_cell, w.grid.cell);
        put_vec(r.grid_spacing, w.grid.spacing);
        for (u32 e = 0; e < NavEdgeCount; ++e) r.nav[e] = w.nav[e];
        records.push_back(r);
    }
    UiDocHeader h{};
    h.font_hi = d.font.hi;
    h.font_lo = d.font.lo;
    h.sprites_hi = d.sprites.hi;
    h.sprites_lo = d.sprites.lo;
    h.widget_count = static_cast<u32>(records.size());
    h.default_focus = d.default_focus;
    h.white_region = d.white_region;
    h.strings_size = static_cast<u32>(blob.size());
    h.modal = d.modal ? 1 : 0;
    h.theme_hi = d.theme.hi;
    h.theme_lo = d.theme.lo;
    out.clear();
    append(out, &h, sizeof(h));
    if (!records.empty()) append(out, records.data(), records.size() * sizeof(UiWidgetRecord));
    if (!blob.empty()) append(out, blob.data(), blob.size());
}

Result<void> decode_ui_document(Span<const byte> payload, DocumentData& out) {
    out.clear();
    if (payload.size() < sizeof(UiDocHeader)) return Error(Errc::Truncated, kStage);
    UiDocHeader h;
    std::memcpy(&h, payload.data(), sizeof(h));
    for (u8 p : h.pad)
        if (p != 0) return Error(Errc::ReservedNonZero, kStage);
    if (h.modal > 1 || h.widget_count > kMaxDocumentWidgets) return Error(Errc::OutOfBounds, kStage);
    const u64 need = sizeof(h) + u64(h.widget_count) * sizeof(UiWidgetRecord) + h.strings_size;
    if (payload.size() < need) return Error(Errc::Truncated, kStage, {}, static_cast<u32>(payload.size()));
    if (h.default_focus != kNoWidgetIndex && h.default_focus >= h.widget_count) return Error(Errc::OutOfBounds, kStage);
    const byte* records = payload.data() + sizeof(h);
    const char* blob = reinterpret_cast<const char*>(records + u64(h.widget_count) * sizeof(UiWidgetRecord));
    out.font = asset::Guid{h.font_hi, h.font_lo};
    out.sprites = asset::Guid{h.sprites_hi, h.sprites_lo};
    out.white_region = h.white_region;
    out.theme = asset::Guid{h.theme_hi, h.theme_lo};
    out.modal = h.modal == 1;
    out.default_focus = h.default_focus;
    out.widgets.reserve(h.widget_count);
    for (u32 i = 0; i < h.widget_count; ++i) {
        UiWidgetRecord r;
        std::memcpy(&r, records + u64(i) * sizeof(UiWidgetRecord), sizeof(r));
        auto fail = [&](Errc code) {
            out.clear();
            return Error(code, kStage, {}, i);
        };
        bool pad_ok = true;
        for (const StyleRecord& s : r.styles) pad_ok = pad_ok && s.pad[0] == 0 && s.pad[1] == 0 && s.pad[2] == 0;
        if (!pad_ok || (r.flags & ~u8(WidgetFlag::Visible | WidgetFlag::Interactable | WidgetFlag::BlocksPointer)) != 0)
            return fail(Errc::ReservedNonZero);
        WidgetData w;
        if (!read_string(blob, h.strings_size, r.name, w.name) || !read_string(blob, h.strings_size, r.text, w.text) ||
            !read_string(blob, h.strings_size, r.convar, w.convar) ||
            !read_string(blob, h.strings_size, r.action, w.action) || !read_string(blob, h.strings_size, r.bind, w.bind) ||
            !read_string(blob, h.strings_size, r.format, w.format))
            return fail(Errc::OutOfBounds);
        if (!valid_name(w.name.view()) || !valid_number_format(w.format.view())) return fail(Errc::InvalidArgument);
        if (r.parent != kNoWidgetIndex && r.parent >= i) return fail(Errc::OutOfBounds);  // parents come first
        if (r.kind > static_cast<u8>(WidgetKind::Tabs) || r.anchor_to > static_cast<u8>(AnchorTo::Screen) ||
            r.stack_axis > static_cast<u8>(Axis::Horizontal) || r.stack_align > static_cast<u8>(Align::Stretch) ||
            r.font_role > static_cast<u8>(FontRole::Button) || r.text_align > static_cast<u8>(TextAlign::Right))
            return fail(Errc::InvalidArgument);
        for (u32 e = 0; e < NavEdgeCount; ++e)
            if (r.nav[e] != kNoWidgetIndex && r.nav[e] >= h.widget_count) return fail(Errc::OutOfBounds);
        for (u32 s = 0; s < kWidgetStateCount; ++s)
            if (r.styles[s].draw_quad > 1) return fail(Errc::InvalidArgument);
        // Depth-first order: a widget's parent is the previous widget or one of its ancestors.
        if (i > 0) {
            u32 a = i - 1;
            while (a != kNoWidgetIndex && a != r.parent) a = out.widgets[a].parent;
            if (a != r.parent) return fail(Errc::InvalidArgument);
        } else if (r.parent != kNoWidgetIndex) {
            return fail(Errc::OutOfBounds);
        }
        for (u32 j = 0; j < i; ++j)
            if (out.widgets[j].parent == r.parent && out.widgets[j].name.view() == w.name.view())
                return fail(Errc::AlreadyExists);
        w.kind = static_cast<WidgetKind>(r.kind);
        w.parent = r.parent;
        w.visible = (r.flags & WidgetFlag::Visible) != 0;
        w.interactable = (r.flags & WidgetFlag::Interactable) != 0;
        w.blocks_pointer = (r.flags & WidgetFlag::BlocksPointer) != 0;
        w.anchors.min = get_vec(r.anchors + 0);
        w.anchors.max = get_vec(r.anchors + 2);
        w.anchors.offset_min = get_vec(r.anchors + 4);
        w.anchors.offset_max = get_vec(r.anchors + 6);
        w.anchors.pivot = get_vec(r.anchors + 8);
        w.anchors.to = static_cast<AnchorTo>(r.anchor_to);
        for (u32 s = 0; s < kWidgetStateCount; ++s) get_style(r.styles[s], w.styles[s]);
        w.font_role = static_cast<FontRole>(r.font_role);
        w.text_align = static_cast<TextAlign>(r.text_align);
        w.own_style = r.own_style;
        w.value = r.value;
        w.lo = r.lo;
        w.hi = r.hi;
        w.step = r.step;
        w.stack.axis = static_cast<Axis>(r.stack_axis);
        w.stack.align = static_cast<Align>(r.stack_align);
        w.stack.spacing = r.stack_spacing;
        w.stack.padding = r.stack_padding;
        w.grid.columns = r.grid_columns;
        w.grid.cell = get_vec(r.grid_cell);
        w.grid.spacing = get_vec(r.grid_spacing);
        for (u32 e = 0; e < NavEdgeCount; ++e) w.nav[e] = r.nav[e];
        out.widgets.push_back(w);
    }
    return ok();
}

namespace {

struct UiThemeHeader {
    u64 fonts[kThemeFontCount][2];  // hi, lo
    u64 sprites_hi, sprites_lo;
    u32 white_region;
    u32 kind_count;   // kWidgetKindCount when written; a reader takes what it knows
    u32 state_count;  // kWidgetStateCount
    u32 pad;
};
static_assert(sizeof(UiThemeHeader) == 80);

constexpr u32 kPaletteColors = sizeof(Palette) / sizeof(Color);
static_assert(sizeof(Palette) == kPaletteColors * sizeof(Color));

}  // namespace

void encode_ui_theme(const ThemeData& t, Array<byte>& out) {
    UiThemeHeader h{};
    for (u32 i = 0; i < kThemeFontCount; ++i) {
        h.fonts[i][0] = t.fonts[i].hi;
        h.fonts[i][1] = t.fonts[i].lo;
    }
    h.sprites_hi = t.sprites.hi;
    h.sprites_lo = t.sprites.lo;
    h.white_region = t.white_region;
    h.kind_count = kWidgetKindCount;
    h.state_count = kWidgetStateCount;
    out.clear();
    append(out, &h, sizeof(h));
    const Color* colors = &t.palette.panel;
    for (u32 i = 0; i < kPaletteColors; ++i) {
        f32 c[4];
        put_color(c, colors[i]);
        append(out, c, sizeof(c));
    }
    for (u32 k = 0; k < kWidgetKindCount; ++k)
        for (u32 s = 0; s < kWidgetStateCount; ++s) {
            StyleRecord r{};
            put_style(r, t.styles[k][s]);
            append(out, &r, sizeof(r));
        }
}

Result<void> decode_ui_theme(Span<const byte> payload, ThemeData& out) {
    constexpr const char* kThemeStage = "ui.theme";
    out = default_theme();
    if (payload.size() < sizeof(UiThemeHeader)) return Error(Errc::Truncated, kThemeStage);
    UiThemeHeader h;
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.pad != 0) return Error(Errc::ReservedNonZero, kThemeStage);
    if (h.kind_count == 0 || h.kind_count > 256 || h.state_count != kWidgetStateCount) return Error(Errc::OutOfBounds, kThemeStage);
    const u64 need = sizeof(h) + u64(kPaletteColors) * 16 + u64(h.kind_count) * h.state_count * sizeof(StyleRecord);
    if (payload.size() < need) return Error(Errc::Truncated, kThemeStage, {}, static_cast<u32>(payload.size()));
    for (u32 i = 0; i < kThemeFontCount; ++i) out.fonts[i] = asset::Guid{h.fonts[i][0], h.fonts[i][1]};
    out.sprites = asset::Guid{h.sprites_hi, h.sprites_lo};
    out.white_region = h.white_region;
    const byte* p = payload.data() + sizeof(h);
    Color* colors = &out.palette.panel;
    for (u32 i = 0; i < kPaletteColors; ++i, p += 16) {
        f32 c[4];
        std::memcpy(c, p, sizeof(c));
        colors[i] = get_color(c);
    }
    for (u32 k = 0; k < h.kind_count; ++k)
        for (u32 s = 0; s < kWidgetStateCount; ++s, p += sizeof(StyleRecord)) {
            StyleRecord r;
            std::memcpy(&r, p, sizeof(r));
            if (!style_record_ok(r)) return Error(Errc::InvalidArgument, kThemeStage, {}, k);
            if (k < kWidgetKindCount) get_style(r, out.styles[k][s]);  // kinds this build does not know are skipped
        }
    return ok();
}

}  // namespace mce::ui
