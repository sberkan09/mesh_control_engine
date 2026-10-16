// UI document tools (ROADMAP M-UI-DATA step 5): ui_list, ui_create, ui_widget_add / set / remove,
// ui_theme_set; ui_preview (the capture) lives with the other player renders in tools_render.cpp.
//
// Every edit goes through the cook's own reader and writer: the document is read
// (cook::read_ui_document), written in the canonical .mcui form (cook::write_ui_document: every
// widget explicit, children spelled out), edited as JSON, read again (so a bad edit fails with the
// cook's file:line message and nothing is written) and saved canonically. Reading what was saved
// gives the same DocumentData, so a document round-trips through these tools unchanged.

#include "mce/cook/cook.h"
#include "mce/cook/fs.h"
#include "mce/cook/ui_text.h"
#include "mce/ui/document_data.h"
#include "mce/ui/types.h"
#include "tool_args.h"
#include "tools.h"

namespace mce::mcp {

namespace {

constexpr usize kMaxDiagnostics = 10;
constexpr const char* kStateNames[ui::kWidgetStateCount] = {"Normal", "Hover", "Pressed", "Focused", "Disabled"};

const char* kind_name(ui::WidgetKind k) {
    const refl::EnumValue* v = ui::mce_refl_enum_record_UiWidgetKind.find(static_cast<i64>(k));
    return v ? v->name_str : "?";
}

bool is_mcui(StringView rel) { return rel.size() > 5 && rel.substr(rel.size() - 5) == ".mcui"; }

// Reads `text` as a .mcui document; false with the cook's diagnostics in c.error.
bool read_doc(ToolCall& c, StringView rel, StringView text, ui::DocumentData& out, const char* what) {
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(text, err) || !doc.root().is_object()) {
        c.error += what;
        c.error += ": not a JSON object";
        return false;
    }
    cook::DiagList diags;
    if (cook::read_ui_document(doc.root(), rel, out, diags)) return true;
    c.error += what;
    c.error += ":";
    append_diags(c.error, diags, kMaxDiagnostics);
    return false;
}

// The UI document named by 'document' (a path under assets/ or "guid:<uuid>").
bool arg_document(ToolCall& c, const editor::ProjectAsset*& out, ui::DocumentData& data) {
    if (!need_project(c)) return false;
    const cook::JsonValue d = arg(c, "document");
    if (!d.is_string()) return arg_missing(c, "document", "a .mcui path under assets/ or \"guid:<uuid>\"");
    const StringView ref = d.as_string();
    const editor::EditorProject& project = c.server.session().project();
    out = nullptr;
    if (ref.size() > 5 && ref.substr(0, 5) == "guid:") {
        const Result<asset::Guid> g = asset::parse_guid(ref.substr(5));
        if (g) out = project.find(g.value());
    } else {
        for (const editor::ProjectAsset& a : project.assets())
            if (a.path.view() == ref) out = &a;
    }
    if (!out) return fail(c, "argument 'document': no such asset (ui_list lists the UI documents)");
    if (out->type != asset::ChunkType::UiDocument) return fail(c, "argument 'document' is not a UI document (.mcui)");
    cook::String text;
    if (!cook::fs::read_text(project.asset_path(out->path.view()).view(), text)) return fail(c, "the document could not be read");
    return read_doc(c, out->path.view(), text.view(), data, "the document on disk does not read");
}

// "menu/play" for widget i (names up the parent chain).
void widget_path(const ui::DocumentData& d, u32 i, cook::String& out) {
    u32 chain[64];
    u32 n = 0;
    for (u32 w = i; w != ui::kNoWidgetIndex && n < 64; w = d.widgets[w].parent) chain[n++] = w;
    out = cook::String();
    while (n > 0) {
        if (!out.empty()) out += '/';
        out += d.widgets[chain[--n]].name.view();
    }
}

// ---------------------------------------------------------------- the JSON edit

struct Edit {
    enum Kind { Add, Set, Remove } kind = Set;
    StringView target;        // Set / Remove: the widget; Add: the parent ("" = the root)
    cook::JsonValue value;    // Add: the widget object; Set: the fields
    u32 index = ~0u;          // Add: position among the siblings (~0: last)
    bool hit = false;         // the target was found
};

void key(cook::String& out, bool& first, StringView k) {
    if (!first) out += ',';
    first = false;
    cook::append_json_string(out, k);
    out += ':';
}

// `base`'s members with `over`'s on top (null in `over` drops the key): one level deep.
void merged(cook::JsonValue base, cook::JsonValue over, cook::String& out) {
    out += '{';
    bool first = true;
    for (u32 i = 0; base.is_object() && i < base.size(); ++i) {
        if (over.is_object() && over.get(base.key_at(i)).valid()) continue;
        key(out, first, base.key_at(i));
        cook::write_json(base.at(i), out, false);
    }
    for (u32 i = 0; over.is_object() && i < over.size(); ++i) {
        if (over.at(i).is_null()) continue;
        key(out, first, over.key_at(i));
        cook::write_json(over.at(i), out, false);
    }
    out += '}';
}

bool anchor_key(StringView k) { return k == "at" || k == "fill" || k == "anchors"; }

void emit_list(cook::JsonValue list, StringView parent, Edit& e, cook::String& out, bool strip_focus);

void emit_widget(cook::JsonValue w, StringView path, Edit& e, cook::String& out, bool strip_focus) {
    const bool set = e.kind == Edit::Set && path == e.target;
    const cook::JsonValue f = set ? e.value : cook::JsonValue();
    if (set) e.hit = true;
    const bool new_anchor = set && (f.get("at").valid() || f.get("fill").valid() || f.get("anchors").valid());
    const bool restyle = set && (f.get("style").valid() || f.get("styles").valid());
    out += '{';
    bool first = true;
    bool had_children = false;
    for (u32 i = 0; i < w.size(); ++i) {
        const StringView k = w.key_at(i);
        if (k == "children") {
            had_children = true;
            key(out, first, k);
            cook::String child_path(path);
            emit_list(w.at(i), child_path.view(), e, out, strip_focus);
            continue;
        }
        if (strip_focus && k == "default_focus" && !set) continue;
        if (set && (f.get(k).valid() || (new_anchor && anchor_key(k)) || (restyle && (k == "style" || k == "styles")))) continue;
        key(out, first, k);
        cook::write_json(w.at(i), out, false);
    }
    if (set) {
        for (u32 i = 0; i < f.size(); ++i) {
            const StringView k = f.key_at(i);
            if (k == "style" || k == "styles" || f.at(i).is_null()) continue;
            key(out, first, k);
            if (k == "nav") merged(w.get("nav"), f.at(i), out);
            else cook::write_json(f.at(i), out, false);
        }
        if (restyle) {
            // Every state's style: what the widget had, then "style" over all, then "styles" per state.
            key(out, first, "styles");
            out += '{';
            const cook::JsonValue had_all = w.get("style"), had = w.get("styles");
            const cook::JsonValue over_all = f.get("style"), over = f.get("styles");
            for (u32 s = 0; s < ui::kWidgetStateCount; ++s) {
                if (s) out += ',';
                cook::append_json_string(out, kStateNames[s]);
                out += ':';
                const cook::JsonValue base = had_all.valid() ? had_all : (had.is_object() ? had.get(kStateNames[s]) : cook::JsonValue());
                cook::String step;
                merged(base, over_all, step);
                cook::JsonDoc tmp;
                cook::JsonError err;
                tmp.parse(step.view(), err);
                merged(tmp.root(), over.is_object() ? over.get(kStateNames[s]) : cook::JsonValue(), out);
            }
            out += '}';
        }
    }
    if (e.kind == Edit::Add && path == e.target && !had_children) {
        key(out, first, "children");
        out += '[';
        cook::write_json(e.value, out, false);
        out += ']';
        e.hit = true;
    }
    out += '}';
}

void emit_list(cook::JsonValue list, StringView parent, Edit& e, cook::String& out, bool strip_focus) {
    out += '[';
    bool first = true;
    const bool add_here = e.kind == Edit::Add && parent == e.target;
    u32 n = 0;
    auto add_new = [&] {
        if (!first) out += ',';
        first = false;
        cook::write_json(e.value, out, false);
        e.hit = true;
    };
    for (u32 i = 0; list.is_array() && i < list.size(); ++i) {
        const cook::JsonValue w = list.at(i);
        cook::String path(parent);
        if (!path.empty()) path += '/';
        path += w.get("name").as_string();
        if (e.kind == Edit::Remove && path.view() == e.target) {
            e.hit = true;
            continue;
        }
        if (add_here && n == e.index) add_new();
        ++n;
        if (!first) out += ',';
        first = false;
        emit_widget(w, path.view(), e, out, strip_focus);
    }
    if (add_here && (e.index == ~0u || e.index >= n)) add_new();
    out += ']';
}

// Applies `e` to the document and saves it; false with c.error set (nothing written).
bool apply_edit(ToolCall& c, const editor::ProjectAsset& a, const ui::DocumentData& d, Edit& e) {
    cook::String canon;
    cook::write_ui_document(d, a.guid, canon);
    cook::JsonDoc doc;
    cook::JsonError err;
    if (!doc.parse(canon.view(), err)) return fail(c, "internal: the canonical document does not parse");
    const bool strip_focus = e.kind == Edit::Set && e.value.get("default_focus").is_bool() && e.value.get("default_focus").as_bool();
    cook::String text("{");
    bool first = true;
    const cook::JsonValue root = doc.root();
    for (u32 i = 0; i < root.size(); ++i) {
        const StringView k = root.key_at(i);
        if (k == "widgets") continue;
        key(text, first, k);
        cook::write_json(root.at(i), text, false);
    }
    key(text, first, "widgets");
    emit_list(root.get("widgets"), StringView(), e, text, strip_focus);
    text += '}';
    if (!e.hit) {
        c.error += e.kind == Edit::Add ? "no parent widget at '" : "no widget at '";
        c.error += e.target;
        c.error += "' (a path of names from the root; ui_list with 'document' lists them)";
        return false;
    }
    ui::DocumentData edited;
    if (!read_doc(c, a.path.view(), text.view(), edited, "the edit does not make a valid document (nothing was written)")) return false;
    cook::String out;
    cook::write_ui_document(edited, a.guid, out);
    if (!asset_validate_and_write(c, a.path.view(), a.guid, out.view())) return false;
    c.out.begin_object();
    c.out.key("document").str(a.path.view()).key("guid").str(asset_guid_ref(a.guid).view());
    c.out.key("widgets").num_u(edited.widgets.size());
    c.out.end_object();
    return true;
}

bool arg_widget_path(ToolCall& c, const char* name, StringView& out, bool required) {
    if (!arg_string(c, name, out, required)) return false;
    if (required && out.empty()) return arg_missing(c, name, "a widget path like \"menu/play\"");
    return true;
}

// ---------------------------------------------------------------- tools

bool ui_list(ToolCall& c) {
    if (!need_project(c)) return false;
    const editor::EditorProject& project = c.server.session().project();
    if (arg(c, "document").valid()) {
        const editor::ProjectAsset* a = nullptr;
        ui::DocumentData d;
        if (!arg_document(c, a, d)) return false;
        c.out.begin_object();
        c.out.key("document").str(a->path.view()).key("guid").str(asset_guid_ref(a->guid).view());
        c.out.key("modal").boolean(d.modal);
        c.out.key("font").str(d.font.is_null() ? StringView("built-in") : asset_guid_ref(d.font).view());
        c.out.key("theme").str(d.theme.is_null() ? StringView("default") : asset_guid_ref(d.theme).view());
        c.out.key("widgets").begin_array();
        cook::String path;
        for (u32 i = 0; i < d.widgets.size(); ++i) {
            const ui::WidgetData& w = d.widgets[i];
            widget_path(d, i, path);
            c.out.begin_object().key("path").str(path.view()).key("kind").str(kind_name(w.kind));
            if (!w.text.empty()) c.out.key("text").str(w.text.view());
            if (!w.visible) c.out.key("visible").boolean(false);
            if (!w.action.empty()) c.out.key("action").str(w.action.view());
            if (!w.bind.empty()) c.out.key("bind").str(w.bind.view());
            if (!w.convar.empty()) c.out.key("convar").str(w.convar.view());
            if (d.default_focus == i) c.out.key("default_focus").boolean(true);
            c.out.end_object();
        }
        c.out.end_array().end_object();
        return true;
    }
    c.out.begin_object().key("documents").begin_array();
    for (const editor::ProjectAsset& a : project.assets()) {
        if (a.type != asset::ChunkType::UiDocument) continue;
        c.out.begin_object().key("path").str(a.path.view()).key("guid").str(asset_guid_ref(a.guid).view());
        cook::String text;
        ui::DocumentData d;
        cook::JsonDoc doc;
        cook::JsonError err;
        cook::DiagList diags;
        if (cook::fs::read_text(project.asset_path(a.path.view()).view(), text) && doc.parse(text.view(), err) &&
            cook::read_ui_document(doc.root(), a.path.view(), d, diags)) {
            c.out.key("modal").boolean(d.modal).key("widgets").num_u(d.widgets.size());
            c.out.key("actions").begin_array();
            for (u32 i = 0; i < d.widgets.size(); ++i) {
                bool seen = d.widgets[i].action.empty();
                for (u32 j = 0; j < i && !seen; ++j) seen = d.widgets[j].action.view() == d.widgets[i].action.view();
                if (!seen) c.out.str(d.widgets[i].action.view());
            }
            c.out.end_array();
        } else {
            c.out.key("error").str("does not read: cook_errors names the problem");
        }
        c.out.end_object();
    }
    c.out.end_array().end_object();
    return true;
}

bool ui_create(ToolCall& c) {
    StringView rel;
    if (!need_project(c) || !arg_string(c, "path", rel, true)) return false;
    if (!asset_rel_valid(rel) || !is_mcui(rel))
        return fail(c, "argument 'path' must be a .mcui path under assets/ like \"ui/hud.mcui\" (forward slashes, no '..')");
    editor::EditorProject& project = c.server.session().project();
    if (cook::fs::exists(project.asset_path(rel).view())) return fail(c, "a file already exists there: the ui_widget_* tools change it");
    bool modal = true;
    if (!arg_bool(c, "modal", modal)) return false;
    const cook::JsonValue font = arg(c, "font"), widgets = arg(c, "widgets");
    if (font.valid() && !font.is_string()) return arg_missing(c, "font", "\"guid:<Font>\"");
    if (widgets.valid() && !widgets.is_array()) return arg_missing(c, "widgets", "an array of .mcui widgets");
    const asset::Guid guid = cook::generate_guid();
    cook::String text("{\"format\":\"mce-ui\",\"version\":");
    cook::appendf(text, "%u", cook::kUiFormatVersion);
    text += ",\"guid\":";
    cook::append_json_string(text, asset::to_string(guid).view());
    text += modal ? ",\"modal\":true" : ",\"modal\":false";
    if (font.valid()) {
        text += ",\"font\":";
        cook::append_json_string(text, font.as_string());
    }
    text += ",\"widgets\":";
    if (widgets.valid()) cook::write_json(widgets, text, false);
    else text += "[]";
    text += '}';
    ui::DocumentData d;
    if (!read_doc(c, rel, text.view(), d, "the document is not valid (nothing was written)")) return false;
    cook::String out;
    cook::write_ui_document(d, guid, out);
    if (!asset_validate_and_write(c, rel, guid, out.view())) return false;
    c.out.begin_object();
    c.out.key("document").str(rel).key("guid").str(asset_guid_ref(guid).view()).key("widgets").num_u(d.widgets.size());
    c.out.end_object();
    return true;
}

bool ui_widget_add(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    ui::DocumentData d;
    Edit e;
    e.kind = Edit::Add;
    if (!arg_document(c, a, d) || !arg_widget_path(c, "parent", e.target, false)) return false;
    e.value = arg(c, "widget");
    if (!e.value.is_object() || !e.value.get("name").is_string())
        return arg_missing(c, "widget", "a .mcui widget object with a \"name\" (and \"kind\", \"children\", ...)");
    u64 index = ~0ull;
    if (arg(c, "index").valid()) {
        if (!arg_uint(c, "index", index, 0, ui::kMaxDocumentWidgets, false)) return false;
    }
    e.index = index == ~0ull ? ~0u : static_cast<u32>(index);
    return apply_edit(c, *a, d, e);
}

bool ui_widget_set(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    ui::DocumentData d;
    Edit e;
    e.kind = Edit::Set;
    if (!arg_document(c, a, d) || !arg_widget_path(c, "widget", e.target, true)) return false;
    e.value = arg(c, "fields");
    if (!e.value.is_object() || e.value.size() == 0) return arg_missing(c, "fields", "an object of .mcui widget keys");
    for (const char* k : {"children", "tabs", "kind_defaults"})
        if (e.value.get(k).valid()) {
            c.error += "argument 'fields' cannot set '";
            c.error += k;
            c.error += "' (ui_widget_add / ui_widget_remove change the tree)";
            return false;
        }
    return apply_edit(c, *a, d, e);
}

bool ui_widget_remove(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    ui::DocumentData d;
    Edit e;
    e.kind = Edit::Remove;
    if (!arg_document(c, a, d) || !arg_widget_path(c, "widget", e.target, true)) return false;
    return apply_edit(c, *a, d, e);
}

// The document's theme: 'theme' is a .mcuitheme (path under assets/ or "guid:<uuid>"), or null for
// the default look. The edit is on the read document; it is written canonically, read back and saved
// like the widget edits.
bool ui_theme_set(ToolCall& c) {
    const editor::ProjectAsset* a = nullptr;
    ui::DocumentData d;
    if (!arg_document(c, a, d)) return false;
    const cook::JsonValue t = arg(c, "theme");
    if (!t.valid() || !(t.is_null() || t.is_string()))
        return arg_missing(c, "theme", "a .mcuitheme path under assets/, \"guid:<uuid>\", or null for the default look");
    const editor::EditorProject& project = c.server.session().project();
    const editor::ProjectAsset* theme = nullptr;
    if (t.is_string()) {
        const StringView ref = t.as_string();
        if (ref.size() > 5 && ref.substr(0, 5) == "guid:") {
            const Result<asset::Guid> g = asset::parse_guid(ref.substr(5));
            if (g) theme = project.find(g.value());
        } else {
            for (const editor::ProjectAsset& p : project.assets())
                if (p.path.view() == ref) theme = &p;
        }
        if (!theme) return fail(c, "argument 'theme': no such asset (asset_list type UiTheme lists the themes)");
        if (theme->type != asset::ChunkType::UiTheme) return fail(c, "argument 'theme' is not a UI theme (.mcuitheme)");
    }
    d.theme = theme ? theme->guid : asset::Guid();
    cook::String text;
    cook::write_ui_document(d, a->guid, text);
    ui::DocumentData edited;
    if (!read_doc(c, a->path.view(), text.view(), edited, "the edit does not make a valid document (nothing was written)")) return false;
    if (!asset_validate_and_write(c, a->path.view(), a->guid, text.view())) return false;
    c.out.begin_object();
    c.out.key("document").str(a->path.view()).key("guid").str(asset_guid_ref(a->guid).view());
    c.out.key("theme").str(edited.theme.is_null() ? StringView("default") : asset_guid_ref(edited.theme).view());
    if (theme) c.out.key("theme_path").str(theme->path.view());
    c.out.end_object();
    return true;
}

#define MCE_UI_DOC R"j("document":{"type":"string","description":"The UI document: a .mcui path under assets/ or \"guid:<uuid>\""})j"

const ToolDef kUiTools[] = {
    {"ui_list",
     "UI documents (.mcui): without 'document', every one in the project with its widget count and the input actions "
     "its widgets raise; with 'document', its widgets in order as paths (\"menu/play\") with kind, text, action, bind "
     "and convar, plus its font and theme. A scene shows a document through a UiScreen component.",
     R"j({"type":"object","properties":{)j" MCE_UI_DOC R"j(}})j",
     ui_list},
    {"ui_create",
     "Make a new UI document (.mcui) with a fresh GUID: 'widgets' in the .mcui widget format (asset_schema type "
     "UiDocument describes it: name, kind, at | fill | anchors, style, styles, text, action, bind, format, children, "
     "...). Checked by the cook before it is written; saved in the canonical form.",
     R"j({"type":"object","properties":{"path":{"type":"string","description":"New .mcui path under assets/, e.g. ui/hud.mcui"},)j"
     R"j("modal":{"type":"boolean","default":true,"description":"false: a HUD that never takes focus"},)j"
     R"j("font":{"type":"string","description":"\"guid:<Font>\"; default the built-in font"},)j"
     R"j("widgets":{"type":"array","description":".mcui widget objects"}},"required":["path"]})j",
     ui_create},
    {"ui_widget_add",
     "Add a widget (with any children) to a UI document under 'parent' (a widget path; omitted: the root), last or at "
     "'index' among the siblings. 'widget' is a .mcui widget object; it starts from the look its kind gets in code. "
     "Nav paths in it are paths from the document root.",
     R"j({"type":"object","properties":{)j" MCE_UI_DOC
     R"j(,"parent":{"type":"string","description":"Parent widget path, e.g. menu; omitted: the root"},)j"
     R"j("widget":{"type":"object","description":"A .mcui widget: {name, kind, text, at, style, action, bind, children, ...}"},)j"
     R"j("index":{"type":"integer","minimum":0,"description":"Position among the parent's children (default last)"}},"required":["document","widget"]})j",
     ui_widget_add},
    {"ui_widget_set",
     "Change a widget of a UI document: 'fields' are .mcui widget keys that replace the widget's own (null removes "
     "one, e.g. \"action\": null). \"style\" changes the listed UiStyle fields in every state, \"styles\": {\"Hover\": "
     "{...}} in one state, \"nav\" the listed edges; one of at / fill / anchors replaces the anchors. Renaming with "
     "\"name\" breaks nav paths that name the old path (the call then fails).",
     R"j({"type":"object","properties":{)j" MCE_UI_DOC
     R"j(,"widget":{"type":"string","description":"Widget path, e.g. menu/play"},)j"
     R"j("fields":{"type":"object","description":"e.g. {\"text\": \"START\", \"style\": {\"tint\": [0.2, 0.5, 0.3, 1]}, \"action\": \"Start\"}"}},"required":["document","widget","fields"]})j",
     ui_widget_set},
    {"ui_widget_remove",
     "Remove a widget and its children from a UI document. Fails when another widget's nav still names it.",
     R"j({"type":"object","properties":{)j" MCE_UI_DOC
     R"j(,"widget":{"type":"string","description":"Widget path, e.g. menu/quit"}},"required":["document","widget"]})j",
     ui_widget_remove},
    {"ui_theme_set",
     "Set the theme a UI document is drawn with: 'theme' is a .mcuitheme (asset_create type UiTheme makes one: a "
     "palette, fonts per role, per-kind looks) as a path under assets/ or \"guid:<uuid>\"; null goes back to the "
     "default look. A UiScreen whose theme is set still wins in the player. ui_preview shows the result.",
     R"j({"type":"object","properties":{)j" MCE_UI_DOC
     R"j(,"theme":{"type":["string","null"],"description":"A .mcuitheme path under assets/ or \"guid:<uuid>\"; null: the default look"}},"required":["document","theme"]})j",
     ui_theme_set},
    {"ui_preview",
     "See a UI document alone: the real player draws it over a plain background at a viewport size, safe area and UI "
     "scale, and the frame comes back as a PNG (offscreen software rendering when there is no display). Bound fields "
     "read the open (saved) scene, else the project's first scene. A project that does not cook (a broken scene) "
     "or has no saved scene is previewed from the document and what it references alone, with nothing bound "
     "('document_only' forces that and skips the project cook). 'save' also writes the PNG for image_diff.",
     R"j({"type":"object","properties":{)j" MCE_UI_DOC
     R"j(,"width":{"type":"integer","minimum":64,"maximum":3840,"default":960},"height":{"type":"integer","minimum":64,"maximum":3840,"default":540},)j"
     R"j("safe_area":{"type":"object","description":"Insets in pixels {left, top, right, bottom}: the UI lays out inside them; the image outlines them"},)j"
     R"j("ui_scale":{"type":"number","minimum":0.25,"maximum":4,"description":"The player's -ui_scale (default 1)"},)j"
     R"j("save":{"type":"string","description":"Also write the PNG inside the project folder, e.g. shots/hud.png"},)j"
     R"j("document_only":{"type":"boolean","description":"Cook only the document, its theme, fonts and sheets, and start an empty scene (bound fields show nothing)"},)j"
     R"j("software":{"type":"boolean","description":"Force the offscreen software renderer"}},"required":["document"]})j",
     ui_preview},
};

}  // namespace

bool ui_document_arg(ToolCall& c, const editor::ProjectAsset*& out) {
    ui::DocumentData d;
    return arg_document(c, out, d);
}

Span<const ToolDef> ui_tool_table() { return Span<const ToolDef>(kUiTools, sizeof(kUiTools) / sizeof(kUiTools[0])); }

}  // namespace mce::mcp
