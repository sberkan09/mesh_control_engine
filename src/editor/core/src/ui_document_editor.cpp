// UI document authoring (ROADMAP M-UI-DATA step 6; ui_document_editor.h).

#include "mce/editor/ui_document_editor.h"

#include <cstring>

#include "mce/asset/asset_system.h"
#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/cook/ui_text.h"
#include "mce/editor/preview_renderer.h"
#include "mce/log.h"
#include "mce/memory/allocator.h"
#include "mce/render/accessibility.h"
#include "mce/render/camera2d.h"

namespace mce::editor {

namespace {

constexpr UiPreviewSize kSizes[] = {
    {"Phone", {390, 844}},
    {"Phone landscape", {844, 390}},
    {"Tablet", {820, 1180}},
    {"Desktop", {1280, 720}},
};

const char* kind_name(ui::WidgetKind k) {
    const refl::EnumValue* v = ui::mce_refl_enum_record_UiWidgetKind.find(static_cast<i64>(k));
    return v ? v->name_str : "widget";
}

bool is_container(ui::WidgetKind k) {
    return k == ui::WidgetKind::Stack || k == ui::WidgetKind::Grid || k == ui::WidgetKind::Scroll;
}

template <usize N>
bool text_edit(DrawCtx& ui, const char* label, FixedString<N>& s) {
    char buf[N + 1];
    const usize n = s.view().size() < N ? s.view().size() : N;
    std::memcpy(buf, s.view().data(), n);
    buf[n] = 0;
    if (!ui.text_field(label, buf, N)) return false;
    s.assign(StringView(buf));
    return true;
}

bool float_edit(DrawCtx& ui, const char* label, f32& v, f64 lo = 0.0, f64 hi = 0.0) {
    f64 d = static_cast<f64>(v);
    if (!ui.float_field(label, &d, lo, hi)) return false;
    v = static_cast<f32>(d);
    return true;
}

}  // namespace

Span<const UiPreviewSize> ui_preview_sizes() {
    return Span<const UiPreviewSize>(kSizes, sizeof(kSizes) / sizeof(kSizes[0]));
}

// ---------------------------------------------------------------- the document

bool UiDocumentEditor::read_text(StringView text, ui::DocumentData& out) {
    cook::JsonDoc doc;
    cook::JsonError jerr;
    if (!doc.parse(text, jerr) || !doc.root().is_object()) {
        error_ = cook::String("not a JSON object");
        return false;
    }
    cook::DiagList diags;
    if (cook::read_ui_document(doc.root(), rel_.view(), out, diags)) return true;
    error_ = cook::String();
    u32 shown = 0;
    for (const cook::Diagnostic& d : diags.all()) {
        if (d.severity != cook::Severity::Error) continue;
        if (shown++ == 3) break;
        if (!error_.empty()) error_ += "; ";
        error_ += d.message.view();
    }
    if (error_.empty()) error_ = cook::String("the document does not read");
    return false;
}

cook::String UiDocumentEditor::text() const {
    cook::String t;
    cook::write_ui_document(data_, guid_, t);
    return t;
}

void UiDocumentEditor::set_data(const ui::DocumentData& d) {
    data_ = d;
    if (selected_ >= data_.widgets.size()) selected_ = ui::kNoWidgetIndex;
    ++revision_;
}

bool UiDocumentEditor::open(EditorSession& session, asset::Guid guid) {
    if (guid == guid_ && dirty_) return true;
    const ProjectAsset* a = session.project().find(guid);
    if (!a || a->type != asset::ChunkType::UiDocument) {
        error_ = cook::String("not a UI document (.mcui)");
        return false;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) {
        error_ = cook::String("the document could not be read");
        return false;
    }
    const cook::String rel_before = rel_;
    rel_ = a->path;
    ui::DocumentData d;
    if (!read_text(file.view(), d)) {
        rel_ = rel_before;
        return false;
    }
    if (guid != guid_) {
        undo_.clear();
        redo_.clear();
        selected_ = ui::kNoWidgetIndex;
    }
    guid_ = guid;
    disk_ = file;
    dirty_ = false;
    error_ = cook::String();
    set_data(d);
    return true;
}

void UiDocumentEditor::close() {
    guid_ = {};
    rel_ = cook::String();
    disk_ = cook::String();
    data_.clear();
    selected_ = ui::kNoWidgetIndex;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    ++revision_;
}

Result<asset::Guid> UiDocumentEditor::create(EditorSession& session, StringView rel_path, bool modal) {
    constexpr const char* kWhere = "editor.ui_create";
    if (!session.project().is_open()) return Error(Errc::InvalidArgument, kWhere, "no project is open");
    if (rel_path.size() < 6 || rel_path.substr(rel_path.size() - 5) != ".mcui")
        return Error(Errc::InvalidArgument, kWhere, "a UI document's path ends in .mcui");
    if (cook::fs::exists(session.project().asset_path(rel_path).view()))
        return Error(Errc::AlreadyExists, kWhere, rel_path);
    ui::DocumentData d;
    d.modal = modal;
    const asset::Guid guid = cook::generate_guid();
    cook::String t;
    cook::write_ui_document(d, guid, t);
    MCE_TRY(session.write_asset(rel_path, t.view()));
    if (!open(session, guid)) return Error(Errc::ImportFailed, kWhere, "the new document did not cook (see the log)");
    return guid;
}

bool UiDocumentEditor::sync(EditorSession& session) {
    if (!is_open() || dirty_) return false;
    const ProjectAsset* a = session.project().find(guid_);
    if (!a) {  // deleted (or moved away from the catalog)
        close();
        return true;
    }
    cook::String file;
    if (!cook::fs::read_text(session.project().asset_path(a->path.view()).view(), file)) return false;
    if (file.view() == disk_.view() && a->path.view() == rel_.view()) return false;
    rel_ = a->path;
    disk_ = file;
    ui::DocumentData d;
    if (!read_text(file.view(), d)) return false;  // keeps the last good document; error_ says why
    error_ = cook::String();
    set_data(d);
    return true;
}

// ---------------------------------------------------------------- the tree

cook::String UiDocumentEditor::widget_path(u32 i) const {
    u32 chain[64];
    u32 n = 0;
    for (u32 w = i; w < data_.widgets.size() && n < 64; w = data_.widgets[w].parent) chain[n++] = w;
    cook::String out;
    while (n > 0) {
        if (!out.empty()) out += '/';
        out += data_.widgets[chain[--n]].name.view();
    }
    return out;
}

u32 UiDocumentEditor::subtree_end(u32 i) const {
    const u32 n = static_cast<u32>(data_.widgets.size());
    if (i >= n) return n;
    u32 e = i + 1;
    while (e < n && is_inside(e, i)) ++e;
    return e;
}

bool UiDocumentEditor::is_inside(u32 widget, u32 ancestor) const {
    for (u32 w = widget; w < data_.widgets.size(); w = data_.widgets[w].parent)
        if (w == ancestor) return true;
    return false;
}

void UiDocumentEditor::reorder(const ui::DocumentData& in, Span<const u32> order, ui::DocumentData& out) {
    out.font = in.font;
    out.sprites = in.sprites;
    out.white_region = in.white_region;
    out.theme = in.theme;
    out.modal = in.modal;
    Array<u32> map;
    map.resize(in.widgets.size());
    for (u32& m : map) m = ui::kNoWidgetIndex;
    for (usize j = 0; j < order.size(); ++j) map[order[j]] = static_cast<u32>(j);
    auto remap = [&](u32 old) { return old < map.size() ? map[old] : ui::kNoWidgetIndex; };
    out.widgets.clear();
    for (const u32 old : order) {
        ui::WidgetData w = in.widgets[old];
        w.parent = remap(w.parent);
        for (u32& e : w.nav) e = remap(e);
        out.widgets.push_back(w);
    }
    out.default_focus = remap(in.default_focus);
}

// ---------------------------------------------------------------- edits

bool UiDocumentEditor::apply(const ui::DocumentData& next) {
    if (!is_open()) {
        error_ = cook::String("no document is open");
        return false;
    }
    cook::String t;
    cook::write_ui_document(next, guid_, t);
    ui::DocumentData back;
    if (!read_text(t.view(), back)) return false;
    error_ = cook::String();
    const cook::String before = text();
    cook::String after;
    cook::write_ui_document(back, guid_, after);
    if (after.view() == before.view()) return true;
    if (!dirty_) {
        undo_.push_back(before);
        if (undo_.size() > kMaxUndo) undo_.erase(0);
    }
    redo_.clear();
    dirty_ = true;
    set_data(back);
    return true;
}

bool UiDocumentEditor::add_widget(u32 parent, ui::WidgetKind kind) {
    const u32 n = static_cast<u32>(data_.widgets.size());
    if (parent != ui::kNoWidgetIndex && parent >= n) return false;
    // A free name among the parent's children: "button", "button2", ...
    char base[32];
    usize len = 0;
    for (const char* c = kind_name(kind); *c && len < 24; ++c)
        base[len++] = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c;
    base[len] = 0;
    FixedString<32> name;
    for (u32 k = 1;; ++k) {
        name.assign(StringView(base, len));
        if (k > 1) {
            char num[12];
            std::snprintf(num, sizeof(num), "%u", k);
            name.append(StringView(num));
        }
        bool taken = false;
        for (const ui::WidgetData& w : data_.widgets) taken |= w.parent == parent && w.name.view() == name.view();
        if (!taken) break;
    }
    ui::WidgetData w;
    ui::builder_defaults(kind, w);
    w.kind = kind;
    w.name = name;
    w.parent = parent;
    if (kind == ui::WidgetKind::Text || kind == ui::WidgetKind::Button || kind == ui::WidgetKind::Toggle)
        w.text.assign(kind_name(kind));
    // Placed in the middle of its parent, unless a layout places it.
    if (parent == ui::kNoWidgetIndex || !is_container(data_.widgets[parent].kind)) {
        const bool big = kind == ui::WidgetKind::Panel || is_container(kind) || kind == ui::WidgetKind::Tabs ||
                         kind == ui::WidgetKind::Console;
        const math::vec2 half = big ? math::vec2(160, 120) : math::vec2(100, 24);
        w.anchors.min = w.anchors.max = math::vec2(0.5f, 0.5f);
        w.anchors.offset_min = math::vec2(0.0f - half.x, 0.0f - half.y);
        w.anchors.offset_max = half;
    }
    ui::DocumentData grown = data_;
    grown.widgets.push_back(w);
    const u32 at = parent == ui::kNoWidgetIndex ? n : subtree_end(parent);
    Array<u32> order;
    for (u32 i = 0; i < at; ++i) order.push_back(i);
    order.push_back(n);
    for (u32 i = at; i < n; ++i) order.push_back(i);
    ui::DocumentData next;
    reorder(grown, order.span(), next);
    if (!apply(next)) return false;
    // The new widget is where it was inserted, unless the read-back moved it (it does not).
    selected_ = at < data_.widgets.size() ? at : ui::kNoWidgetIndex;
    return true;
}

bool UiDocumentEditor::remove_widget(u32 widget) {
    if (widget >= data_.widgets.size()) return false;
    const u32 end = subtree_end(widget);
    Array<u32> order;
    for (u32 i = 0; i < data_.widgets.size(); ++i)
        if (i < widget || i >= end) order.push_back(i);
    ui::DocumentData next;
    reorder(data_, order.span(), next);
    if (!apply(next)) return false;
    selected_ = ui::kNoWidgetIndex;
    return true;
}

bool UiDocumentEditor::move_widget(u32 widget, i32 dir) {
    const u32 n = static_cast<u32>(data_.widgets.size());
    if (widget >= n || dir == 0) return false;
    const u32 parent = data_.widgets[widget].parent;
    u32 a = 0, b = 0, c = 0;  // swap the blocks [a, b) and [b, c)
    if (dir < 0) {
        u32 prev = ui::kNoWidgetIndex;
        for (u32 i = 0; i < widget; ++i)
            if (data_.widgets[i].parent == parent) prev = i;
        if (prev == ui::kNoWidgetIndex) return false;
        a = prev, b = widget, c = subtree_end(widget);
    } else {
        const u32 next = subtree_end(widget);
        if (next >= n || data_.widgets[next].parent != parent) return false;
        a = widget, b = next, c = subtree_end(next);
    }
    Array<u32> order;
    for (u32 i = 0; i < a; ++i) order.push_back(i);
    for (u32 i = b; i < c; ++i) order.push_back(i);
    for (u32 i = a; i < b; ++i) order.push_back(i);
    for (u32 i = c; i < n; ++i) order.push_back(i);
    ui::DocumentData next;
    reorder(data_, order.span(), next);
    if (!apply(next)) return false;
    selected_ = dir < 0 ? a : a + (c - b);
    return true;
}

bool UiDocumentEditor::set_kind(u32 widget, ui::WidgetKind kind) {
    if (widget >= data_.widgets.size()) return false;
    ui::DocumentData next = data_;
    ui::WidgetData& w = next.widgets[widget];
    if (w.kind == kind) return true;
    w.kind = kind;
    ui::builder_defaults(kind, w);
    return apply(next);
}

Result<void> UiDocumentEditor::save(EditorSession& session) {
    if (!dirty_) return ok();
    const cook::String t = text();
    MCE_TRY(session.write_asset(rel_.view(), t.view()));
    disk_ = t;
    dirty_ = false;
    return ok();
}

bool UiDocumentEditor::undo(EditorSession& session) {
    if (undo_.empty()) return false;
    ui::DocumentData d;
    if (!read_text(undo_.back().view(), d)) return false;
    redo_.push_back(text());
    undo_.pop_back();
    set_data(d);
    dirty_ = true;
    return static_cast<bool>(save(session));
}

bool UiDocumentEditor::redo(EditorSession& session) {
    if (redo_.empty()) return false;
    ui::DocumentData d;
    if (!read_text(redo_.back().view(), d)) return false;
    if (dirty_) {
        if (Result<void> r = save(session); !r) return false;
    }
    undo_.push_back(text());
    redo_.pop_back();
    set_data(d);
    dirty_ = true;
    return static_cast<bool>(save(session));
}

// ---------------------------------------------------------------- the inspector

bool draw_ui_inspector(DrawCtx& ui, UiDocumentEditor& ed, u32& state, bool& every_state) {
    if (!ed.is_open()) return false;
    ui::DocumentData next = ed.data();
    const u32 i = ed.selected();
    bool changed = false;
    if (i == ui::kNoWidgetIndex) {
        if (ui.begin_group("Document", true)) {
            changed |= ui.checkbox("Modal", &next.modal);
            ui.help("Off: a HUD that never takes focus");
            changed |= ui.asset_field("Font", &next.font, asset::Font::name);
            ui.help("The body font; none: the theme's, else the built-in one");
            changed |= ui.asset_field("Theme", &next.theme, asset::UiTheme::name);
            ui.help("A .mcuitheme; none: the default look. A UiScreen's theme wins in the player");
            const cook::String focus =
                next.default_focus == ui::kNoWidgetIndex ? cook::String() : ed.widget_path(next.default_focus);
            ui.read_only("Default focus", focus.empty() ? StringView("(none)") : focus.view());
            ui.end_group();
        }
        return changed && ed.apply(next);
    }
    ui::WidgetData& w = next.widgets[i];
    if (!ui.begin_group("Widget", true)) return false;
    changed |= text_edit(ui, "Name", w.name);
    i64 kind = static_cast<i64>(w.kind);
    if (ui.enum_field("Kind", &kind, ui::mce_refl_enum_record_UiWidgetKind)) {
        ui.end_group();
        return ed.set_kind(i, static_cast<ui::WidgetKind>(kind));
    }
    ui.help("A new kind takes that kind's look");
    changed |= text_edit(ui, "Text", w.text);
    if (w.kind == ui::WidgetKind::Slider || w.kind == ui::WidgetKind::Toggle) {
        changed |= float_edit(ui, "Value", w.value);
        changed |= float_edit(ui, "Min", w.lo);
        changed |= float_edit(ui, "Max", w.hi);
        changed |= float_edit(ui, "Step", w.step);
    } else if (w.kind == ui::WidgetKind::Tabs) {
        i64 page = static_cast<i64>(w.value);
        if (ui.int_field("Page shown", &page, 0, 63)) {
            w.value = static_cast<f32>(page);
            changed = true;
        }
    }
    changed |= ui.checkbox("Visible", &w.visible);
    changed |= ui.checkbox("Interactable", &w.interactable);
    changed |= ui.checkbox("Blocks pointer", &w.blocks_pointer);
    bool focus = next.default_focus == i;
    if (ui.checkbox("Default focus", &focus)) {
        next.default_focus = focus ? i : ui::kNoWidgetIndex;
        changed = true;
    }
    changed |= text_edit(ui, "Action", w.action);
    ui.help("The input action its Confirmed / ValueChanged events raise");
    changed |= text_edit(ui, "Bind", w.bind);
    ui.help("<Component>/<field> shown read-only, e.g. Score/points");
    changed |= text_edit(ui, "Format", w.format);
    ui.help("printf with one number, e.g. SCORE %.0f; empty: %g");
    changed |= text_edit(ui, "Convar", w.convar);
    i64 role = static_cast<i64>(w.font_role);
    if (ui.enum_field("Font role", &role, ui::mce_refl_enum_record_UiFontRole)) {
        w.font_role = static_cast<ui::FontRole>(role);
        changed = true;
    }
    if (ui.begin_group("Anchors", true)) {
        changed |= draw_fields(ui, refl::type_of<ui::UiAnchors>(), &w.anchors);
        ui.end_group();
    }
    if (ui.begin_group("Style", true)) {
        i64 s = static_cast<i64>(state);
        if (ui.enum_field("State", &s, ui::mce_refl_enum_record_UiWidgetState) && s >= 0 &&
            s < i64(ui::kWidgetStateCount))
            state = static_cast<u32>(s);
        if (state >= ui::kWidgetStateCount) state = 0;
        ui.checkbox("Every state", &every_state);
        ui.help("A field changed here changes in every state");
        const ui::Style before = w.styles[state];
        if (draw_fields(ui, refl::type_of<ui::UiStyle>(), &w.styles[state])) {
            changed = true;
            if (every_state) {
                const refl::TypeRecord& t = refl::type_of<ui::UiStyle>();
                const u8* a = reinterpret_cast<const u8*>(&before);
                const u8* b = reinterpret_cast<const u8*>(&w.styles[state]);
                for (const refl::FieldRecord& f : t.fields) {
                    const usize bytes = usize(f.size) * (f.count > 1 ? f.count : 1);
                    if (std::memcmp(a + f.offset, b + f.offset, bytes) == 0) continue;
                    for (u32 k = 0; k < ui::kWidgetStateCount; ++k)
                        std::memcpy(reinterpret_cast<u8*>(&w.styles[k]) + f.offset, b + f.offset, bytes);
                }
            }
        }
        ui.end_group();
    }
    if (w.kind == ui::WidgetKind::Stack || w.kind == ui::WidgetKind::Scroll || w.kind == ui::WidgetKind::Tabs) {
        if (ui.begin_group("Stack", true)) {
            changed |= draw_fields(ui, refl::type_of<ui::UiStackLayout>(), &w.stack);
            ui.end_group();
        }
    }
    if (w.kind == ui::WidgetKind::Grid && ui.begin_group("Grid", true)) {
        changed |= draw_fields(ui, refl::type_of<ui::UiGridLayout>(), &w.grid);
        ui.end_group();
    }
    if (ui.begin_group("Nav", false)) {
        static constexpr const char* kEdges[ui::NavEdgeCount] = {"Up", "Down", "Left", "Right"};
        for (u32 e = 0; e < ui::NavEdgeCount; ++e) {
            FixedString<128> path;
            if (w.nav[e] != ui::kNoWidgetIndex) path.assign(ed.widget_path(w.nav[e]).view());
            if (!text_edit(ui, kEdges[e], path)) continue;
            const u32 to = path.empty() ? ui::kNoWidgetIndex : next.find(path.view());
            if (!path.empty() && to == ui::kNoWidgetIndex) continue;  // not a widget (yet): keep the edge
            w.nav[e] = to;
            changed = true;
        }
        ui.help("A widget path from the root, e.g. menu/quit");
        ui.end_group();
    }
    ui.end_group();
    return changed && ed.apply(next);
}

// ---------------------------------------------------------------- the preview

void UiDocumentPreview::reset() {
    delete doc_;
    doc_ = nullptr;
}

bool UiDocumentPreview::stale(PreviewRenderer& preview, const UiDocumentEditor& ed, u64 content) const {
    return guid_ != ed.guid() || revision_ != ed.revision() || content_ != content ||
           generation_ != preview.content_generation();
}

bool UiDocumentPreview::rebuild(PreviewRenderer& preview, const UiDocumentEditor& ed) {
    reset();
    const ui::Skin* base = ed.is_open() ? preview.ui_skin() : nullptr;
    if (!base) return false;
    // As the player's UiScreens: the theme over the document, the document's font for the body and
    // the theme's for the roles it leaves (ui_screens.cpp).
    ui::DocumentData data = ed.data();
    ui::ThemeData theme = ui::default_theme();
    asset::AssetSystem* assets = preview.assets();
    if (!data.theme.is_null() && assets) {
        const Result<asset::AssetHandle<asset::UiTheme>> th = assets->load_sync<asset::UiTheme>(data.theme);
        if (th) {
            const Result<void> tr = ui::decode_ui_theme(assets->resolve(th.value()), theme);
            assets->release(asset::AnyHandle(th.value()));
            if (tr)
                ui::apply_theme(theme, data);
            else
                theme = ui::default_theme();
        }
    }
    ui::Skin look = *base;
    auto font = [&](asset::Guid g) -> const render::Font* { return g.is_null() ? nullptr : preview.sprites().font(g); };
    if (const render::Font* f = font(!data.font.is_null() ? data.font : theme.font(ui::FontRole::Body))) look.font = f;
    look.title_font = font(theme.font(ui::FontRole::Title));
    look.button_font = font(theme.font(ui::FontRole::Button));
    if (!data.sprites.is_null()) {
        u32 regions = 0;
        const render::SpriteSetId set = preview.sprites().ui_sheet_set(data.sprites, &regions);
        if (set.valid() && data.white_region < regions) {
            look.set = set;
            look.white_region = data.white_region;
        }
    }
    doc_ = new ui::Document();
    doc_->set_skin(look);
    if (Result<void> r = ui::build_document(data, *doc_); !r) {
        reset();
        return false;
    }
    return true;
}

void UiDocumentPreview::record(rhi::CommandBuffer& cmd, PreviewRenderer& preview, const UiDocumentEditor& ed,
                               u64 content, rhi::TextureHandle target, rhi::Extent2D size, f32 ui_scale,
                               math::vec2 pointer, bool pointer_down) {
    if (stale(preview, ed, content)) {
        rebuild(preview, ed);  // a failure leaves an empty frame until the next change
        guid_ = ed.guid();
        revision_ = ed.revision();
        content_ = content;
        generation_ = preview.content_generation();
    }
    render::SpriteBatch batch(preview.renderer().sprite_sets(), mem::frame());
    batch.begin(render::compute_screen_view(size), render::PassId::Screen2D);
    if (doc_) {
        ui::UpdateContext ctx;
        ctx.viewport = size;
        ctx.ui_scale = ui_scale;
        // The player's accessibility settings (M-ACCESS): the preview shows what such a player sees.
        const render::AccessibilitySettings access = render::accessibility_settings();
        ctx.text_scale = access.text_scale;
        ctx.high_contrast = access.high_contrast;
        ctx.pointer = pointer;
        ctx.pointer_down = pointer_down;
        ctx.pointer_pressed = pointer_down && !was_down_;
        ctx.pointer_released = !pointer_down && was_down_;
        doc_->update(ctx);
        doc_->submit(batch, 100);
    }
    was_down_ = pointer_down;
    batch.build();
    preview.record_screen(cmd, batch, target, math::Color{0.16f, 0.17f, 0.2f, 1.0f});
}

}  // namespace mce::editor
