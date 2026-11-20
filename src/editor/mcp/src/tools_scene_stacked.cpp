// scene_find_stacked (M-STACKLINT step 2): the entities of a scene that are exact copies of each other, on one spot.

#include "mce/cook/fs.h"
#include "mce/editor/stacked_entities.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

bool scene_find_stacked(ToolCall& c) {
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
    const Array<editor::StackedGroup> groups = editor::find_stacked(*doc);
    c.out.begin_object().key("scene").str(path.empty() ? StringView("the open scene") : path);
    c.out.key("groups").begin_array();
    for (const editor::StackedGroup& g : groups) {
        c.out.begin_object().key("keep").num_u(g.ids[0]).key("copies").begin_array();
        for (usize i = 1; i < g.ids.size(); ++i) c.out.num_u(g.ids[i]);
        c.out.end_array().end_object();
    }
    c.out.end_array().key("report").str(
        editor::format_stacked(*doc, Span<const editor::StackedGroup>(groups.data(), groups.size())).view());
    c.out.end_object();
    return true;
}

}  // namespace mce::mcp
