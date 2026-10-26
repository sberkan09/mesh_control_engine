// scene_lint (M-SCENELINT step 3): mistakes in a scene that load fine and misbehave later (an entity without a name,
// siblings that share a name, a scale of zero, a transform that is not a number, a missing parent). The same
// findings `mce scene lint` prints.

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

}  // namespace mce::mcp
