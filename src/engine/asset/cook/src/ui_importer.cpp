// .mcui -> CHNK_UIDOC: one UI document, a widget tree (M-UI-DATA step 1; mce/ui/document_data.h).
// The chunk is the one ui::decode_ui_document reads, written by encode_ui_document and decoded
// again before it is stored, so a document the runtime would reject never cooks.
//
// {
//   "format": "mce-ui", "version": 1, "guid": "<uuid>",
//   "font": "guid:<Font>", "sprites": "guid:<SpriteSheet>", "white_region": 0,   // the skin; omitted = built-in
//   "theme": "guid:<UiTheme>",                           // a .mcuitheme (M-UI-DATA 3); omitted = the default look
//   "modal": true,                                       // false: a HUD, never takes focus
//   "widgets": [
//     { "name": "menu", "kind": "Stack",                 // a UiWidgetKind; default Panel
//       "at": [0.5, 0.5, 320, 0],                        // point anchor + size (0 = preferred), or
//       // "fill": true, or "anchors": { UiAnchors fields: "min", "max", "offset_min", "offset_max", "pivot", "to" }
//       "stack": { "spacing": 12 },                      // UiStackLayout; "grid": UiGridLayout
//       "children": [
//         { "name": "play", "kind": "Button", "text": "PLAY", "default_focus": true,
//           "style": { "text_size": 24 },                // UiStyle fields over every state
//           "styles": { "Hover": { "tint": [0.3, 0.3, 0.4, 1] } },  // one state's fields over that
//           "nav": { "down": "menu/volume" } },
//         { "name": "volume", "kind": "Slider", "range": [0, 1, 0.05], "value": 0.8, "convar": "snd.volume" },
//         { "name": "tabs", "kind": "Tabs", "value": 0,
//           "tabs": [ { "name": "audio", "label": "AUDIO", "children": [ ... ] } ] },
//         { "name": "confirm", "modal": true, "children": [ ... ] } ] } ]   // a Panel as Builder::modal's veil
// }
//
// A widget starts from the look and values ui::Builder gives its kind (builder_defaults; with
// "modal": true, modal_defaults), then takes the keys it names: so "style" changes only the fields
// it lists. "kind_defaults": false starts from the C++ defaults instead (what write_ui_document
// emits). Other keys: "visible", "interactable", "blocks_pointer". Gameplay reaches a document only
// through data: "action" names the input action the widget's Confirmed / ValueChanged raises in the
// player, and "bind": "Component/field" (+ "format", printf) shows a field read-only. "font_role":
// Body | Title | Button picks the theme's font for the widget's text (default Auto: Button for
// buttons, toggles and sliders, Body otherwise). Names are unique among siblings
// and contain no '/'; "nav" edges and Document::find take paths of names from the root.
//
// A Tabs widget lists its pages under "tabs" (a bar button and a page each, as Builder::tab), or
// spells out its two children (a Stack "bar" of buttons and a Panel "pages", one page per button)
// under "children". Its "value" is the page shown.

#include "importer_common.h"
#include "mce/asset/formats.h"
#include "mce/cook/scene_text.h"
#include "mce/cook/ui_text.h"
#include "mce/refl/field_traits.h"
#include "mce/ui/document_data.h"

namespace mce::cook {

namespace {

constexpr const char* kStage = "cook.ui";

using ui::kNoWidgetIndex;
using ui::kWidgetStateCount;
using ui::WidgetData;
using ui::WidgetKind;

constexpr const char* kEdgeNames[ui::NavEdgeCount] = {"up", "down", "left", "right"};

const refl::EnumRecord& kind_enum() { return ui::mce_refl_enum_record_UiWidgetKind; }
const refl::EnumRecord& state_enum() { return ui::mce_refl_enum_record_UiWidgetState; }
const refl::EnumRecord& font_role_enum() { return ui::mce_refl_enum_record_UiFontRole; }
const refl::EnumRecord& text_align_enum() { return ui::mce_refl_enum_record_UiTextAlign; }

struct PendingNav {
    u32 widget;
    u32 edge;
    JsonValue path;
};

class Reader {
public:
    Reader(StringView file, ui::DocumentData& out, DiagList& diags) : file_(file), out_(out), diags_(diags) {}

    void error(JsonValue at, StringView msg) { detail::error_at(diags_, Errc::ParseError, file_, at, kStage, msg); }

    bool read(JsonValue doc) {
        const u32 errors_before = diags_.error_count();
        if (!detail::check_keys(doc,
                                {"format", "version", "guid", "font", "sprites", "white_region", "theme", "modal", "widgets"},
                                file_, kStage, diags_))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-ui") {
            error(fmt.valid() ? fmt : doc, "\"format\" must be \"mce-ui\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kUiFormatVersion) {
            String msg;
            appendf(msg, "unsupported UI document version (this cook reads version %u)", kUiFormatVersion);
            detail::error_at(diags_, Errc::VersionTooNew, file_, ver.valid() ? ver : doc, kStage, msg);
            return false;
        }
        read_ref(doc.get("font"), out_.font, "font");
        read_ref(doc.get("sprites"), out_.sprites, "sprites");
        read_ref(doc.get("theme"), out_.theme, "theme");
        if (const JsonValue wr = doc.get("white_region"); wr.valid()) {
            if (!wr.is_integer() || wr.as_number() < 0 || wr.as_number() > 4294967294.0)
                error(wr, "'white_region' must be a sprite region index");
            else
                out_.white_region = static_cast<u32>(wr.as_number());
        }
        detail::read_bool(doc, "modal", true, out_.modal, file_, kStage, diags_);
        const JsonValue widgets = doc.get("widgets");
        if (widgets.valid() && !widgets.is_array())
            error(widgets, "'widgets' must be an array");
        else if (widgets.valid())
            read_list(widgets, kNoWidgetIndex);
        if (diags_.error_count() != errors_before) return false;
        for (const PendingNav& n : nav_) {
            const u32 target = out_.find(n.path.as_string());
            if (target == kNoWidgetIndex) {
                String msg("no widget at '");
                msg += n.path.as_string();
                msg += "' (a path of names from the root)";
                error(n.path, msg);
            } else {
                out_.widgets[n.widget].nav[n.edge] = target;
            }
        }
        return diags_.error_count() == errors_before;
    }

private:
    void read_ref(JsonValue v, asset::Guid& g, const char* key) {
        if (!v.valid() || v.is_null()) return;
        if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
            String msg;
            appendf(msg, "'%s' must be \"guid:<uuid>\" or null", key);
            error(v, msg);
        }
    }

    void read_list(JsonValue list, u32 parent) {
        for (u32 i = 0; i < list.size(); ++i) read_widget(list.at(i), parent);
    }

    bool read_struct(const refl::TypeRecord& type, JsonValue v, void* dst, const char* key) {
        return read_scene_component(type, v, dst, 0, file_, key, diags_);
    }

    u32 push(const WidgetData& d, JsonValue at) {
        for (const WidgetData& w : out_.widgets)
            if (w.parent == d.parent && w.name.view() == d.name.view()) {
                String msg("a sibling is already named '");
                msg += d.name.view();
                msg += "'";
                error(at, msg);
                return kNoWidgetIndex;
            }
        if (out_.widgets.size() >= ui::kMaxDocumentWidgets) {
            String msg;
            appendf(msg, "a document holds at most %u widgets", ui::kMaxDocumentWidgets);
            error(at, msg);
            return kNoWidgetIndex;
        }
        out_.widgets.push_back(d);
        return static_cast<u32>(out_.widgets.size() - 1);
    }

    // A generated widget (a tabs bar, its buttons and pages), with Builder's look.
    u32 add_generated(WidgetKind kind, StringView name, u32 parent, JsonValue at) {
        WidgetData d;
        ui::builder_defaults(kind, d);
        d.name.assign(name);
        d.kind = kind;
        d.parent = parent;
        return push(d, at);
    }

    void read_widget(JsonValue w, u32 parent) {
        if (!w.is_object()) {
            error(w, "a widget must be an object");
            return;
        }
        if (!detail::check_keys(w, {"name",    "kind",         "kind_defaults",  "modal",         "anchors", "fill",
                                    "at",      "style",        "styles",         "text",          "value",   "range",
                                    "visible", "interactable", "blocks_pointer", "default_focus", "stack",   "grid",
                                    "nav",     "convar",       "children",       "tabs",          "action",  "bind",
                                    "format",  "font_role", "text_align"},
                                file_, kStage, diags_))
            return;
        const JsonValue name = w.get("name");
        if (!name.is_string() || name.as_string().empty() || name.as_string().find('/') != StringView::npos ||
            name.as_string().size() > 31) {
            error(name.valid() ? name : w, "a widget needs a \"name\": 1 to 31 bytes, no '/'");
            return;
        }
        WidgetKind kind = WidgetKind::Panel;
        if (const JsonValue k = w.get("kind"); k.valid()) {
            const refl::EnumValue* ev = k.is_string() ? kind_enum().find(StringId(k.as_string())) : nullptr;
            if (!ev) {
                error(
                    k,
                    "'kind' must be a UiWidgetKind: Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Console, "
                    "Scroll or Tabs");
                return;
            }
            kind = static_cast<WidgetKind>(ev->value);
        }
        WidgetData d;
        bool defaults = true, modal = false;
        detail::read_bool(w, "kind_defaults", true, defaults, file_, kStage, diags_);
        detail::read_bool(w, "modal", false, modal, file_, kStage, diags_);
        if (defaults) ui::builder_defaults(kind, d);
        if (modal) {
            if (kind != WidgetKind::Panel) error(w.get("modal"), "\"modal\": true goes on a Panel");
            ui::modal_defaults(d);
        }
        d.name.assign(name.as_string());
        d.kind = kind;
        d.parent = parent;

        // Anchors: one of "anchors", "fill", "at".
        const JsonValue anchors = w.get("anchors"), fill = w.get("fill"), at = w.get("at");
        if (u32(anchors.valid()) + u32(fill.valid()) + u32(at.valid()) > 1)
            error(w, "a widget takes one of \"anchors\", \"fill\" and \"at\"");
        else if (anchors.valid())
            read_struct(refl::type_of<ui::UiAnchors>(), anchors, &d.anchors, "anchors");
        else if (fill.valid()) {
            if (!fill.is_bool() || !fill.as_bool()) error(fill, "'fill' must be true");
            d.anchors.min = math::vec2(0, 0);
            d.anchors.max = math::vec2(1, 1);
            d.anchors.offset_min = d.anchors.offset_max = math::vec2(0, 0);
        } else if (at.valid()) {
            bool ok = at.is_array() && at.size() == 4;
            for (u32 i = 0; ok && i < 4; ++i) ok = at.at(i).is_number();
            if (!ok)
                error(at, "'at' must be [anchor x, anchor y, width, height]");
            else {
                const math::vec2 a(static_cast<f32>(at.at(0).as_number()), static_cast<f32>(at.at(1).as_number()));
                const math::vec2 size(static_cast<f32>(at.at(2).as_number()), static_cast<f32>(at.at(3).as_number()));
                d.anchors.min = d.anchors.max = a;
                d.anchors.pivot = math::vec2(0.5f, 0.5f);
                d.anchors.offset_min = math::vec2(0.0f - size.x * 0.5f, 0.0f - size.y * 0.5f);  // as Builder::at
                d.anchors.offset_max = math::vec2(size.x * 0.5f, size.y * 0.5f);
            }
        }

        // Styles: "style" over every state, then "styles" per state.
        if (const JsonValue st = w.get("style"); st.valid()) {
            for (u32 s = 0; s < kWidgetStateCount; ++s)
                if (!read_struct(refl::type_of<ui::UiStyle>(), st, &d.styles[s], "style")) break;  // one report
        }
        if (const JsonValue st = w.get("styles"); st.valid()) {
            if (!st.is_object()) error(st, "'styles' must be an object of UiWidgetState names");
            for (u32 i = 0; st.is_object() && i < st.size(); ++i) {
                const refl::EnumValue* ev = state_enum().find(StringId(st.key_at(i)));
                if (!ev || ev->value >= static_cast<i64>(kWidgetStateCount)) {
                    error(st.at(i), "a 'styles' key must be Normal, Hover, Pressed, Focused or Disabled");
                    continue;
                }
                read_struct(refl::type_of<ui::UiStyle>(), st.at(i), &d.styles[ev->value], "styles");
            }
        }

        if (const JsonValue t = w.get("text"); t.valid()) {
            if (!t.is_string() || t.as_string().size() > 255)
                error(t, "'text' must be a string of at most 255 bytes");
            else
                d.text.assign(t.as_string());
        }
        if (const JsonValue r = w.get("range"); r.valid()) {
            bool ok = r.is_array() && r.size() == 3;
            for (u32 i = 0; ok && i < 3; ++i) ok = r.at(i).is_number();
            if (ok) {
                d.lo = static_cast<f32>(r.at(0).as_number());
                d.hi = static_cast<f32>(r.at(1).as_number());
                d.step = static_cast<f32>(r.at(2).as_number());
            }
            if (!ok || !(d.lo <= d.hi) || !(d.step >= 0.0f))
                error(r, "'range' must be [lo, hi, step] with lo <= hi and step >= 0");
        }
        const JsonValue value = w.get("value");
        if (value.valid()) {
            if (!value.is_number())
                error(value, "'value' must be a number");
            else
                d.value = static_cast<f32>(value.as_number());
            if (kind == WidgetKind::Tabs && (!value.is_integer() || value.as_number() < 0))
                error(value, "a tabs widget's 'value' is the page shown: an index from 0");
            else if (kind != WidgetKind::Tabs && value.is_number() && (d.value < d.lo || d.value > d.hi))
                error(value, "'value' is outside the widget's range");
        } else if (kind != WidgetKind::Tabs) {
            d.value = d.value < d.lo ? d.lo : (d.value > d.hi ? d.hi : d.value);
        }
        detail::read_bool(w, "visible", d.visible, d.visible, file_, kStage, diags_);
        detail::read_bool(w, "interactable", d.interactable, d.interactable, file_, kStage, diags_);
        detail::read_bool(w, "blocks_pointer", d.blocks_pointer, d.blocks_pointer, file_, kStage, diags_);
        if (const JsonValue st = w.get("stack"); st.valid())
            read_struct(refl::type_of<ui::UiStackLayout>(), st, &d.stack, "stack");
        if (const JsonValue g = w.get("grid"); g.valid())
            read_struct(refl::type_of<ui::UiGridLayout>(), g, &d.grid, "grid");
        if (const JsonValue c = w.get("convar"); c.valid()) {
            if (!c.is_string() || c.as_string().empty() || c.as_string().size() > 63)
                error(c, "'convar' must be a convar name of at most 63 bytes");
            else
                d.convar.assign(c.as_string());
        }
        if (const JsonValue f = w.get("font_role"); f.valid()) {
            const refl::EnumValue* ev = f.is_string() ? font_role_enum().find(StringId(f.as_string())) : nullptr;
            if (!ev)
                error(f, "'font_role' must be a UiFontRole: Auto, Body, Title or Button");
            else
                d.font_role = static_cast<ui::FontRole>(ev->value);
        }
        if (const JsonValue a = w.get("text_align"); a.valid()) {
            const refl::EnumValue* ev = a.is_string() ? text_align_enum().find(StringId(a.as_string())) : nullptr;
            if (!ev)
                error(a, "'text_align' must be a UiTextAlign: Left, Center or Right");
            else
                d.text_align = static_cast<ui::TextAlign>(ev->value);
        }
        if (const JsonValue a = w.get("action"); a.valid()) {
            if (!a.is_string() || a.as_string().empty() || a.as_string().size() > 31)
                error(a, "'action' must be an action name of at most 31 bytes");
            else
                d.action.assign(a.as_string());
        }
        if (const JsonValue b = w.get("bind"); b.valid()) {
            const StringView path = b.is_string() ? b.as_string() : StringView();
            usize slash = 0;
            while (slash < path.size() && path[slash] != '/') ++slash;
            if (path.size() > 63 || slash == 0 || slash + 1 >= path.size())
                error(b, "'bind' must be \"<Component>/<field>\" of at most 63 bytes");
            else
                d.bind.assign(path);
        }
        if (const JsonValue f = w.get("format"); f.valid()) {
            if (!f.is_string() || f.as_string().size() > 31 || !ui::valid_number_format(f.as_string()))
                error(f, "'format' must be a number format of at most 31 bytes: text and one %f / %e / %g");
            else if (d.bind.empty())
                error(f, "'format' needs a 'bind'");
            else
                d.format.assign(f.as_string());
        }

        const u32 index = push(d, name);
        if (index == kNoWidgetIndex) return;

        bool focus = false;
        detail::read_bool(w, "default_focus", false, focus, file_, kStage, diags_);
        if (focus) {
            if (out_.default_focus != kNoWidgetIndex)
                error(w.get("default_focus"), "another widget already has default_focus");
            out_.default_focus = index;
        }
        if (const JsonValue nav = w.get("nav"); nav.valid()) {
            if (!nav.is_object()) error(nav, "'nav' must be an object: up, down, left, right");
            for (u32 i = 0; nav.is_object() && i < nav.size(); ++i) {
                u32 edge = ui::NavEdgeCount;
                for (u32 e = 0; e < ui::NavEdgeCount; ++e)
                    if (nav.key_at(i) == StringView(kEdgeNames[e])) edge = e;
                if (edge == ui::NavEdgeCount)
                    error(nav.at(i), "a 'nav' key must be up, down, left or right");
                else if (!nav.at(i).is_string() || nav.at(i).as_string().empty())
                    error(nav.at(i), "a 'nav' edge is a widget path");
                else
                    nav_.push_back(PendingNav{index, edge, nav.at(i)});
            }
        }

        const JsonValue children = w.get("children"), tabs = w.get("tabs");
        if (tabs.valid() && kind != WidgetKind::Tabs)
            error(tabs, "'tabs' goes on a Tabs widget");
        else if (tabs.valid() && children.valid())
            error(tabs, "a Tabs widget takes \"tabs\" or \"children\", not both");
        if (children.valid()) {
            if (!children.is_array())
                error(children, "'children' must be an array");
            else
                read_list(children, index);
            if (kind == WidgetKind::Tabs) check_tabs(index, children);
        } else if (kind == WidgetKind::Tabs) {
            read_tabs(index, tabs, value.valid() ? value : w);
        }
    }

    // Builder::tabs + Builder::tab, spelled out: the bar and its buttons, then the pages panel and
    // the pages (depth-first order).
    void read_tabs(u32 index, JsonValue tabs, JsonValue value_at) {
        if (tabs.valid() && !tabs.is_array()) {
            error(tabs, "'tabs' must be an array of { \"name\", \"label\", \"children\" }");
            return;
        }
        const JsonValue at = tabs.valid() ? tabs : value_at;
        const u32 count = tabs.valid() ? tabs.size() : 0;
        const u32 shown = static_cast<u32>(out_.widgets[index].value);
        if (count > 0 && shown >= count) error(value_at, "a tabs widget's 'value' must name one of its pages");
        const u32 bar = add_generated(WidgetKind::Stack, "bar", index, at);
        if (bar == kNoWidgetIndex) return;
        out_.widgets[bar].stack.axis = ui::Axis::Horizontal;
        out_.widgets[bar].stack.spacing = 4.0f;
        Array<JsonValue> names;
        for (u32 i = 0; i < count; ++i) {
            const JsonValue t = tabs.at(i);
            if (!t.is_object()) {
                error(t, "a tab must be an object");
                continue;
            }
            if (!detail::check_keys(t, {"name", "label", "children"}, file_, kStage, diags_)) continue;
            const JsonValue name = t.get("name"), label = t.get("label");
            if (!name.is_string() || name.as_string().empty() || name.as_string().size() > 31 ||
                name.as_string().find('/') != StringView::npos) {
                error(name.valid() ? name : t, "a tab needs a \"name\": 1 to 31 bytes, no '/'");
                continue;
            }
            if (label.valid() && (!label.is_string() || label.as_string().size() > 255)) {
                error(label, "'label' must be a string of at most 255 bytes");
                continue;
            }
            const u32 button = add_generated(WidgetKind::Button, name.as_string(), bar, name);
            if (button == kNoWidgetIndex) continue;
            if (label.valid()) out_.widgets[button].text.assign(label.as_string());
            names.push_back(t);
        }
        const u32 pages = add_generated(WidgetKind::Panel, "pages", index, at);
        if (pages == kNoWidgetIndex) return;
        for (ui::Style& s : out_.widgets[pages].styles) {
            s = ui::Style{};
            s.draw_quad = false;
        }
        for (u32 i = 0; i < names.size(); ++i) {
            const JsonValue t = names[i];
            const u32 page = add_generated(WidgetKind::Panel, t.get("name").as_string(), pages, t.get("name"));
            if (page == kNoWidgetIndex) continue;
            WidgetData& p = out_.widgets[page];
            for (ui::Style& s : p.styles) {
                s = ui::Style{};
                s.draw_quad = false;
            }
            p.anchors.min = math::vec2(0, 0);
            p.anchors.max = math::vec2(1, 1);
            p.visible = i == shown;
            const JsonValue children = t.get("children");
            if (children.valid() && !children.is_array())
                error(children, "'children' must be an array");
            else if (children.valid())
                read_list(children, page);
        }
    }

    // A Tabs widget's spelled-out children: a Stack bar of buttons, then a Panel of as many pages.
    void check_tabs(u32 index, JsonValue at) {
        u32 kids[3] = {kNoWidgetIndex, kNoWidgetIndex, kNoWidgetIndex};
        u32 n = 0;
        for (u32 i = index + 1; i < out_.widgets.size(); ++i)
            if (out_.widgets[i].parent == index) {
                if (n < 3) kids[n] = i;
                ++n;
            }
        auto count_children = [&](u32 p) {
            u32 c = 0;
            for (u32 i = p + 1; i < out_.widgets.size(); ++i) c += out_.widgets[i].parent == p ? 1 : 0;
            return c;
        };
        if (n != 2 || out_.widgets[kids[0]].kind != WidgetKind::Stack ||
            out_.widgets[kids[1]].kind != WidgetKind::Panel || count_children(kids[0]) != count_children(kids[1])) {
            error(at, "a Tabs widget's children are a Stack of tab buttons and a Panel with one page per button");
            return;
        }
        for (u32 i = kids[0] + 1; i < out_.widgets.size(); ++i)
            if (out_.widgets[i].parent == kids[0] && out_.widgets[i].kind != WidgetKind::Button) {
                error(at, "a Tabs widget's bar holds only buttons");
                return;
            }
        const u32 pages = count_children(kids[1]);
        if (pages > 0 && static_cast<u32>(out_.widgets[index].value) >= pages)
            error(at, "a tabs widget's 'value' must name one of its pages");
    }

    StringView file_;
    ui::DocumentData& out_;
    DiagList& diags_;
    Array<PendingNav> nav_;
};

// ---- writing ---------------------------------------------------------------------------------------

void indent(String& out, u32 depth) {
    for (u32 i = 0; i < depth; ++i) out += "  ";
}

void widget_path(const ui::DocumentData& d, u32 i, String& out) {
    if (d.widgets[i].parent != kNoWidgetIndex) {
        widget_path(d, d.widgets[i].parent, out);
        out += '/';
    }
    out += d.widgets[i].name.view();
}

void write_widget(const ui::DocumentData& d, u32 i, u32 depth, String& out) {
    const WidgetData& w = d.widgets[i];
    const WidgetData def;
    indent(out, depth);
    out += "{ \"name\": ";
    append_json_string(out, w.name.view());
    out += ", \"kind\": ";
    const refl::EnumValue* kind = kind_enum().find(static_cast<i64>(w.kind));
    append_json_string(out, kind ? StringView(kind->name_str) : StringView("Panel"));
    out += ", \"kind_defaults\": false";
    auto field = [&](const char* key) {
        out += ",\n";
        indent(out, depth + 1);
        append_json_string(out, key);
        out += ": ";
    };
    auto reflected = [&](const char* key, const refl::TypeRecord& type, const void* value) {
        String s;
        write_scene_component(type, value, s, nullptr, nullptr);
        if (s.view() == "{}") return;
        field(key);
        out += s.view();
    };
    reflected("anchors", refl::type_of<ui::UiAnchors>(), &w.anchors);
    String one[kWidgetStateCount];
    bool same = true;
    for (u32 s = 0; s < kWidgetStateCount; ++s) {
        write_scene_component(refl::type_of<ui::UiStyle>(), &w.styles[s], one[s], nullptr, nullptr);
        same = same && one[s].view() == one[0].view();
    }
    if (same && one[0].view() != "{}") {
        field("style");
        out += one[0].view();
    } else if (!same) {
        field("styles");
        for (u32 s = 0; s < kWidgetStateCount; ++s) {
            out += s == 0 ? "{\n" : ",\n";
            indent(out, depth + 2);
            const refl::EnumValue* st = state_enum().find(static_cast<i64>(s));
            append_json_string(out, st ? StringView(st->name_str) : StringView());
            out += ": ";
            out += one[s].view();
        }
        out += " }";
    }
    if (!w.text.empty()) {
        field("text");
        append_json_string(out, w.text.view());
    }
    if (w.lo != def.lo || w.hi != def.hi || w.step != def.step) {
        field("range");
        out += '[';
        append_json_f32(out, w.lo);
        out += ", ";
        append_json_f32(out, w.hi);
        out += ", ";
        append_json_f32(out, w.step);
        out += ']';
    }
    if (w.value != def.value) {
        field("value");
        append_json_f32(out, w.value);
    }
    if (!w.visible) {
        field("visible");
        out += "false";
    }
    if (!w.interactable) {
        field("interactable");
        out += "false";
    }
    if (w.blocks_pointer) {
        field("blocks_pointer");
        out += "true";
    }
    if (d.default_focus == i) {
        field("default_focus");
        out += "true";
    }
    reflected("stack", refl::type_of<ui::UiStackLayout>(), &w.stack);
    reflected("grid", refl::type_of<ui::UiGridLayout>(), &w.grid);
    bool any_nav = false;
    for (u32 e = 0; e < ui::NavEdgeCount; ++e) {
        if (w.nav[e] == kNoWidgetIndex) continue;
        if (!any_nav) field("nav");
        out += any_nav ? ", " : "{ ";
        any_nav = true;
        append_json_string(out, kEdgeNames[e]);
        out += ": ";
        String path;
        widget_path(d, w.nav[e], path);
        append_json_string(out, path.view());
    }
    if (any_nav) out += " }";
    if (!w.convar.empty()) {
        field("convar");
        append_json_string(out, w.convar.view());
    }
    if (!w.action.empty()) {
        field("action");
        append_json_string(out, w.action.view());
    }
    if (!w.bind.empty()) {
        field("bind");
        append_json_string(out, w.bind.view());
    }
    if (!w.format.empty()) {
        field("format");
        append_json_string(out, w.format.view());
    }
    if (w.font_role != ui::FontRole::Auto) {
        field("font_role");
        const refl::EnumValue* role = font_role_enum().find(static_cast<i64>(w.font_role));
        append_json_string(out, role ? StringView(role->name_str) : StringView("Auto"));
    }
    if (w.text_align != ui::TextAlign::Left) {
        field("text_align");
        const refl::EnumValue* al = text_align_enum().find(static_cast<i64>(w.text_align));
        append_json_string(out, al ? StringView(al->name_str) : StringView("Left"));
    }
    bool any_child = false;
    for (u32 c = i + 1; c < d.widgets.size(); ++c) {
        if (d.widgets[c].parent != i) continue;
        if (!any_child) {
            field("children");
            out += "[\n";
        } else {
            out += ",\n";
        }
        any_child = true;
        write_widget(d, c, depth + 2, out);
    }
    if (any_child) {
        out += '\n';
        indent(out, depth + 1);
        out += ']';
    }
    out += " }";
}

class UiImporter final : public IImporter {
public:
    StringView id() const override { return "ui"; }
    u32 version() const override { return 3; }
    ChunkType type() const override { return ChunkType::UiDocument; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcui"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        (void)diags;  // a malformed reference is reported by cook()
        struct Ref {
            const char* key;
            ChunkType type;
        };
        for (const Ref r : {Ref{"font", ChunkType::Font}, Ref{"sprites", ChunkType::SpriteSheet}, Ref{"theme", ChunkType::UiTheme}}) {
            const JsonValue v = in.settings.get(r.key);
            Guid g;
            if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) continue;
            DepRef d;
            d.guid = g;
            d.expected = r.type;
            d.line = v.line();
            d.col = v.col();
            d.where.assign(r.key);
            out.push_back(d);
        }
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        ui::DocumentData data;
        if (!read_ui_document(in.settings, in.display_path, data, diags)) return false;
        Array<byte> bytes;
        ui::encode_ui_document(data, bytes);
        ui::DocumentData check;
        if (Result<void> r = ui::decode_ui_document(bytes.span(), check); !r) {
            FixedString<512> why;
            format_error(r.error(), why);
            String msg("the UI document is not loadable: ");
            msg += why.view();
            detail::error_at(diags, Errc::ParseError, in.display_path, in.settings, kStage, msg);
            return false;
        }
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::UiDocument;
        d.version = asset::chunk_version(asset::UI_DOCUMENT_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

bool read_ui_document(JsonValue doc, StringView file, ui::DocumentData& out, DiagList& diags) {
    out.clear();
    Reader r(file, out, diags);
    return r.read(doc);
}

void write_ui_document(const ui::DocumentData& d, asset::Guid guid, String& out) {
    out += "{\n  \"format\": \"mce-ui\", \"version\": ";
    appendf(out, "%u", kUiFormatVersion);
    out += ", \"guid\": ";
    append_json_string(out, asset::to_string(guid).view());
    auto ref = [&](const char* key, asset::Guid g) {
        if (g.is_null()) return;
        out += ",\n  ";
        append_json_string(out, key);
        out += ": ";
        String s("guid:");
        s += asset::to_string(g).view();
        append_json_string(out, s.view());
    };
    ref("font", d.font);
    ref("sprites", d.sprites);
    ref("theme", d.theme);
    if (d.white_region != 0) appendf(out, ",\n  \"white_region\": %u", d.white_region);
    if (!d.modal) out += ",\n  \"modal\": false";
    out += ",\n  \"widgets\": [";
    bool first = true;
    for (u32 i = 0; i < d.widgets.size(); ++i) {
        if (d.widgets[i].parent != kNoWidgetIndex) continue;
        out += first ? "\n" : ",\n";
        first = false;
        write_widget(d, i, 2, out);
    }
    out += first ? "]\n}\n" : "\n  ]\n}\n";
}

mem::UniquePtr<IImporter> make_ui_importer() { return mem::make_unique<UiImporter>(mem::heap()); }

// ---- themes (M-UI-DATA step 3) ---------------------------------------------------------------------
//
// {
//   "format": "mce-uitheme", "version": 1, "guid": "<uuid>",
//   "palette": { "panel": [0.1, 0.1, 0.2, 0.9], "accent": [1, 0.5, 0.2, 1] },   // UiPalette fields
//   "fonts": { "body": "guid:<Font>", "title": "guid:<Font>", "button": "guid:<Font>" },
//   "sprites": "guid:<SpriteSheet>", "white_region": 0,
//   "styles": { "Button": { "style": { "text_size": 20 }, "styles": { "Hover": { ... } } } }   // per UiWidgetKind
// }
//
// Every kind's look starts from the palette (ui::theme_from_palette); "styles" changes only the
// UiStyle fields it lists, over every state ("style") or one ("styles"), as in a .mcui widget.

namespace {

constexpr const char* kThemeStage = "cook.uitheme";
constexpr const char* kFontRoleKeys[ui::kThemeFontCount] = {"body", "title", "button"};

class ThemeReader {
public:
    ThemeReader(StringView file, ui::ThemeData& out, DiagList& diags) : file_(file), out_(out), diags_(diags) {}

    void error(JsonValue at, StringView msg) { detail::error_at(diags_, Errc::ParseError, file_, at, kThemeStage, msg); }

    bool read(JsonValue doc) {
        const u32 errors_before = diags_.error_count();
        if (!detail::check_keys(doc, {"format", "version", "guid", "palette", "fonts", "sprites", "white_region", "styles"}, file_,
                                kThemeStage, diags_))
            return false;
        const JsonValue fmt = doc.get("format");
        if (!fmt.is_string() || fmt.as_string() != "mce-uitheme") {
            error(fmt.valid() ? fmt : doc, "\"format\" must be \"mce-uitheme\"");
            return false;
        }
        const JsonValue ver = doc.get("version");
        if (!ver.is_integer() || ver.as_number() != kUiThemeFormatVersion) {
            String msg;
            appendf(msg, "unsupported UI theme version (this cook reads version %u)", kUiThemeFormatVersion);
            detail::error_at(diags_, Errc::VersionTooNew, file_, ver.valid() ? ver : doc, kThemeStage, msg);
            return false;
        }
        ui::Palette palette;
        if (const JsonValue p = doc.get("palette"); p.valid())
            read_scene_component(refl::type_of<ui::UiPalette>(), p, &palette, 0, file_, "palette", diags_);
        ui::theme_from_palette(palette, out_);
        if (const JsonValue f = doc.get("fonts"); f.valid()) {
            if (!f.is_object()) error(f, "'fonts' must be an object: body, title, button");
            for (u32 i = 0; f.is_object() && i < f.size(); ++i) {
                u32 role = ui::kThemeFontCount;
                for (u32 r = 0; r < ui::kThemeFontCount; ++r)
                    if (f.key_at(i) == StringView(kFontRoleKeys[r])) role = r;
                if (role == ui::kThemeFontCount)
                    error(f.at(i), "a 'fonts' key must be body, title or button");
                else
                    read_ref(f.at(i), out_.fonts[role], "fonts");
            }
        }
        read_ref(doc.get("sprites"), out_.sprites, "sprites");
        if (const JsonValue wr = doc.get("white_region"); wr.valid()) {
            if (!wr.is_integer() || wr.as_number() < 0 || wr.as_number() > 4294967294.0)
                error(wr, "'white_region' must be a sprite region index");
            else
                out_.white_region = static_cast<u32>(wr.as_number());
        }
        if (const JsonValue st = doc.get("styles"); st.valid()) {
            if (!st.is_object()) error(st, "'styles' must be an object of UiWidgetKind names");
            for (u32 i = 0; st.is_object() && i < st.size(); ++i) read_kind(st.key_at(i), st.at(i));
        }
        return diags_.error_count() == errors_before;
    }

private:
    void read_ref(JsonValue v, asset::Guid& g, const char* key) {
        if (!v.valid() || v.is_null()) return;
        if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) {
            String msg;
            appendf(msg, "'%s' must be \"guid:<uuid>\" or null", key);
            error(v, msg);
        }
    }

    void read_kind(StringView name, JsonValue v) {
        const refl::EnumValue* ev = kind_enum().find(StringId(name));
        if (!ev) {
            error(v, "a 'styles' key must be a UiWidgetKind: Panel, Image, Text, Button, Toggle, Slider, Stack, Grid, Console, Scroll or Tabs");
            return;
        }
        if (!v.is_object() || !detail::check_keys(v, {"style", "styles"}, file_, kThemeStage, diags_)) {
            if (!v.is_object()) error(v, "a kind's look is { \"style\": {...}, \"styles\": { \"Hover\": {...} } }");
            return;
        }
        ui::Style(&styles)[kWidgetStateCount] = out_.styles[ev->value];
        if (const JsonValue st = v.get("style"); st.valid())
            for (u32 s = 0; s < kWidgetStateCount; ++s)
                if (!read_scene_component(refl::type_of<ui::UiStyle>(), st, &styles[s], 0, file_, "style", diags_)) break;
        if (const JsonValue st = v.get("styles"); st.valid()) {
            if (!st.is_object()) error(st, "'styles' must be an object of UiWidgetState names");
            for (u32 i = 0; st.is_object() && i < st.size(); ++i) {
                const refl::EnumValue* sv = state_enum().find(StringId(st.key_at(i)));
                if (!sv || sv->value >= static_cast<i64>(kWidgetStateCount)) {
                    error(st.at(i), "a 'styles' key must be Normal, Hover, Pressed, Focused or Disabled");
                    continue;
                }
                read_scene_component(refl::type_of<ui::UiStyle>(), st.at(i), &styles[sv->value], 0, file_, "styles", diags_);
            }
        }
    }

    StringView file_;
    ui::ThemeData& out_;
    DiagList& diags_;
};

class UiThemeImporter final : public IImporter {
public:
    StringView id() const override { return "uitheme"; }
    u32 version() const override { return 1; }
    ChunkType type() const override { return ChunkType::UiTheme; }
    bool native() const override { return true; }
    bool handles_extension(StringView ext) const override { return ext == ".mcuitheme"; }

    void scan_dependencies(const ImportInput& in, Array<DepRef>& out, DiagList& diags) const override {
        (void)diags;  // a malformed reference is reported by cook()
        auto add = [&](JsonValue v, ChunkType type, const char* where) {
            Guid g;
            if (!v.is_string() || !parse_guid_ref(v.as_string(), g)) return;
            DepRef d;
            d.guid = g;
            d.expected = type;
            d.line = v.line();
            d.col = v.col();
            d.where.assign(where);
            out.push_back(d);
        };
        if (const JsonValue f = in.settings.get("fonts"); f.is_object())
            for (u32 i = 0; i < f.size(); ++i) add(f.at(i), ChunkType::Font, "fonts");
        add(in.settings.get("sprites"), ChunkType::SpriteSheet, "sprites");
    }

    bool cook(const ImportInput& in, asset::ContainerWriter& out, DiagList& diags) const override {
        ui::ThemeData theme;
        if (!read_ui_theme(in.settings, in.display_path, theme, diags)) return false;
        Array<byte> bytes;
        ui::encode_ui_theme(theme, bytes);
        asset::ContainerWriter::ChunkDesc d;
        d.type = ChunkType::UiTheme;
        d.version = asset::chunk_version(asset::UI_THEME_VERSION, 0);
        d.guid = in.guid;
        out.add(d, bytes.span());
        return true;
    }
};

}  // namespace

bool read_ui_theme(JsonValue doc, StringView file, ui::ThemeData& out, DiagList& diags) {
    out = ui::default_theme();
    ThemeReader r(file, out, diags);
    return r.read(doc);
}

mem::UniquePtr<IImporter> make_ui_theme_importer() { return mem::make_unique<UiThemeImporter>(mem::heap()); }

}  // namespace mce::cook
