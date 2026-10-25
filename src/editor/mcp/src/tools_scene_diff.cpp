// scene_diff (M-SCENEDIFF step 3): what changed between two versions of a scene, in the document's own terms
// (entities by id, components and fields by reflected name). The same report `mce scene diff` prints.

#include "mce/cook/fs.h"
#include "mce/editor/scene_diff.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

namespace {

// The scene file at `rel` under assets/, parsed.
bool load_scene(ToolCall& c, StringView rel, editor::SceneDocument& doc) {
    cook::String abs;
    if (!scene_path(c.server, rel, abs, c.error)) return false;
    cook::String text;
    if (!cook::fs::read_text(abs.view(), text)) {
        c.error += "cannot read ";
        c.error += rel;
        return false;
    }
    cook::DiagList diags;
    if (!doc.parse(text.view(), rel, diags)) {
        c.error += rel;
        c.error += " does not parse";
        append_diags(c.error, diags, 5);
        return false;
    }
    return true;
}

}  // namespace

bool scene_diff(ToolCall& c) {
    StringView a, b;
    if (!arg_string(c, "a", a, true) || !arg_string(c, "b", b, false)) return false;
    editor::SceneDocument before, loaded;
    if (!load_scene(c, a, before)) return false;
    // Without b: the file on disk against the open scene, unsaved edits included.
    const editor::SceneDocument* after = &c.server.session().scene().doc();
    if (!b.empty()) {
        if (!load_scene(c, b, loaded)) return false;
        after = &loaded;
    }
    const editor::SceneDiff diff = editor::diff_scenes(before, *after);
    u32 added = 0, removed = 0, changed = 0;
    for (const editor::EntityDiff& e : diff.entities) {
        if (e.kind == editor::DiffKind::Added) ++added;
        else if (e.kind == editor::DiffKind::Removed) ++removed;
        else ++changed;
    }
    c.out.begin_object().key("identical").boolean(diff.empty());
    c.out.key("against").str(b.empty() ? StringView("the open scene") : b);
    c.out.key("header_changed").boolean(diff.header_changed).key("reordered").boolean(diff.reordered);
    c.out.key("added").num_u(added).key("removed").num_u(removed).key("changed").num_u(changed);
    c.out.key("report").str(editor::format_diff(diff).view()).end_object();
    return true;
}

}  // namespace mce::mcp
