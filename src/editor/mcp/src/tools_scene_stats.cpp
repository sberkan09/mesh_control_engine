// scene_stats (M-SCENESTATS step 3): what a scene holds at a glance (entities, roots, hierarchy depth, prefab
// instances, the box the local positions span, the components in use). The same numbers `mce scene stats` prints.

#include "mce/cook/fs.h"
#include "mce/editor/scene_stats.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

bool scene_stats(ToolCall& c) {
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
    const editor::SceneStats st = editor::scene_stats(*doc);
    c.out.begin_object().key("scene").str(path.empty() ? StringView("the open scene") : path);
    c.out.key("entities").num_u(st.entities).key("roots").num_u(st.roots).key("max_depth").num_u(st.max_depth);
    c.out.key("prefab_instances").num_u(st.prefab_instances).key("has_bounds").boolean(st.has_bounds);
    if (st.has_bounds) {
        c.out.key("min").begin_array().num_f(st.min.x).num_f(st.min.y).num_f(st.min.z).end_array();
        c.out.key("max").begin_array().num_f(st.max.x).num_f(st.max.y).num_f(st.max.z).end_array();
    }
    c.out.key("components").begin_array();
    for (const editor::ComponentUse& u : st.components)
        c.out.begin_object().key("type").str(StringView(u.type->name)).key("entities").num_u(u.entities).end_object();
    c.out.end_array().key("report").str(editor::format_stats(st).view()).end_object();
    return true;
}

}  // namespace mce::mcp
