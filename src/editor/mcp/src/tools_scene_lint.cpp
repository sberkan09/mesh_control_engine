// scene_lint (M-SCENELINT step 3): mistakes in a scene that load fine and misbehave later (an entity without a name,
// siblings that share a name, a scale of zero, a transform that is not a number, a missing parent). The same
// findings `mce scene lint` prints. scene_lint_fix repairs them in a saved file.

#include "mce/cook/fs.h"
#include "mce/editor/scene_lint.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

const char* kind_name(editor::LintKind k) {
    switch (k) {
        case editor::LintKind::EmptyName: return "empty_name";
        case editor::LintKind::DuplicateSiblingName: return "duplicate_sibling_name";
        case editor::LintKind::ZeroScale: return "zero_scale";
        case editor::LintKind::NonFiniteTransform: return "non_finite_transform";
        case editor::LintKind::MissingParent: return "missing_parent";
        case editor::LintKind::DanglingRef: return "dangling_entity_ref";
        case editor::LintKind::ParentCycle: return "parent_cycle";
    }
    return "";
}

}  // namespace

bool scene_lint(ToolCall& c) {
    StringView path;
    if (!arg_string(c, "path", path, false)) return false;
    editor::SceneDocument loaded;
    // Without a path: the open scene, unsaved edits included.
    const editor::SceneDocument* doc = &c.server.session().scene().doc();
    if (!path.empty()) {
        cook::String abs;
        if (!scene_path(c.server, path, abs, c.error)) return false;
        cook::String text;
        if (!cook::fs::read_text(abs.view(), text)) {
            c.error += "cannot read ";
            c.error += path;
            return false;
        }
        cook::DiagList diags;
        if (!loaded.parse(text.view(), path, diags)) {
            c.error += path;
            c.error += " does not parse";
            append_diags(c.error, diags, 5);
            return false;
        }
        doc = &loaded;
    }
    const Array<editor::LintFinding> found = editor::lint_scene(*doc);
    c.out.begin_object().key("clean").boolean(found.empty());
    c.out.key("scene").str(path.empty() ? StringView("the open scene") : path);
    c.out.key("findings").begin_array();
    for (const editor::LintFinding& f : found)
        c.out.begin_object().key("entity").num_u(f.id).key("name").str(f.name.view()).key("kind").str(kind_name(f.kind)).end_object();
    c.out.end_array().key("report").str(editor::format_lint(found.span()).view()).end_object();
    return true;
}

// scene_lint_fix (M-LINTFIX 3): repairs what scene_lint finds in a saved scene and writes the file in the canonical text.
bool scene_lint_fix(ToolCall& c) {
    StringView path;
    if (!arg_string(c, "path", path, true)) return false;
    cook::String abs;
    if (!scene_path(c.server, path, abs, c.error)) return false;
    cook::String text;
    if (!cook::fs::read_text(abs.view(), text)) {
        c.error += "cannot read ";
        c.error += path;
        return false;
    }
    editor::SceneDocument doc;
    cook::DiagList diags;
    if (!doc.parse(text.view(), path, diags)) {
        c.error += path;
        c.error += " does not parse";
        append_diags(c.error, diags, 5);
        return false;
    }
    const Array<editor::LintFinding> before = editor::lint_scene(doc);
    const cook::String report = editor::format_lint(before.span());
    const usize fixed = editor::fix_scene_lint(doc);
    if (fixed > 0) {
        cook::String out;
        doc.serialize(out);
        if (!cook::fs::write_file_atomic(abs.view(), Span<const byte>(reinterpret_cast<const byte*>(out.c_str()), out.size()))) {
            c.error += "could not write ";
            c.error += path;
            return false;
        }
    }
    c.out.begin_object().key("scene").str(path).key("fixed").num_u(fixed);
    c.out.key("written").boolean(fixed > 0);
    c.out.key("report").str(report.view()).end_object();
    return true;
}

// scene_lint_all (M-LINTALL 1): scene_lint over every scene and prefab under assets/, so one call answers "is the project tidy".
// With fix:true (M-LINTFIXALL 1) every file is repaired like scene_lint_fix and the rows say what was found and how many were fixed.
bool scene_lint_all(ToolCall& c) {
    bool fix = false;
    if (!arg_bool(c, "fix", fix)) return false;
    if (!c.server.session().project().is_open()) {
        c.error += "no project is open: start mce_mcp with --project <dir>";
        return false;
    }
    const cook::String assets_dir = c.server.session().project().asset_path("");
    Array<cook::String> files;
    (void)cook::fs::list_files(assets_dir.view(), files);
    auto ends_with = [](StringView s, StringView suffix) { return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix; };
    usize checked = 0, total = 0, fixed_total = 0;
    struct Row {
        cook::String path;
        cook::String report;
        usize findings = 0;
        usize fixed = 0;
        bool unreadable = false;
    };
    Array<Row> rows;
    for (const cook::String& rel : files) {
        if (!ends_with(rel.view(), ".mcscene") && !ends_with(rel.view(), ".mcprefab")) continue;
        ++checked;
        Row row;
        row.path = rel;
        cook::String abs = assets_dir;
        abs += rel.view();
        cook::String text;
        editor::SceneDocument doc;
        cook::DiagList diags;
        if (!cook::fs::read_text(abs.view(), text) || !doc.parse(text.view(), rel.view(), diags)) {
            row.unreadable = true;
            rows.push_back(row);
            continue;
        }
        const Array<editor::LintFinding> found = editor::lint_scene(doc);
        if (found.empty()) continue;
        row.findings = found.size();
        row.report = editor::format_lint(found.span());
        total += found.size();
        if (fix) {
            row.fixed = editor::fix_scene_lint(doc);
            fixed_total += row.fixed;
            cook::String out;
            doc.serialize(out);
            if (!cook::fs::write_file_atomic(abs.view(), Span<const byte>(reinterpret_cast<const byte*>(out.c_str()), out.size()))) {
                c.error += "could not write ";
                c.error += rel.view();
                return false;
            }
        }
        rows.push_back(row);
    }
    usize unreadable = 0;
    c.out.begin_object();
    for (const Row& r : rows) unreadable += r.unreadable ? 1 : 0;
    // After a fix the files are tidy, so "clean" is about what is on disk now.
    c.out.key("clean").boolean((fix || total == 0) && unreadable == 0);
    c.out.key("scenes_checked").num_u(checked).key("findings").num_u(total).key("unreadable").num_u(unreadable);
    if (fix) c.out.key("fixed").num_u(fixed_total);
    c.out.key("files").begin_array();
    for (const Row& r : rows) {
        c.out.begin_object().key("path").str(r.path.view());
        if (r.unreadable) c.out.key("error").str("cannot read or parse");
        else {
            c.out.key("findings").num_u(r.findings);
            if (fix) c.out.key("fixed").num_u(r.fixed);
            c.out.key("report").str(r.report.view());
        }
        c.out.end_object();
    }
    c.out.end_array().end_object();
    return true;
}

}  // namespace mce::mcp
