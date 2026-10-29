// scene_find (M-SCENEFIND step 3): the entities of a scene that match a name part, a component or a prefab link,
// each with its hierarchy path. The same hits `mce scene find` prints.

#include "mce/cook/fs.h"
#include "mce/editor/scene_find.h"
#include "mce/refl/type.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

bool scene_find(ToolCall& c) {
    StringView path;
    StringView name;
    StringView component;
    if (!arg_string(c, "path", path, false) || !arg_string(c, "name", name, false) ||
        !arg_string(c, "component", component, false))
        return false;
    editor::FindQuery query;
    query.name = name;
    if (!arg_bool(c, "prefab", query.prefab_instances_only)) return false;
    if (!component.empty()) {
        query.component = refl::find_type(StringId(component));
        if (query.component == nullptr) {
            c.error += "unknown component ";
            c.error += component;
            return false;
        }
    }
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
    const Array<editor::FindHit> hits = editor::find_entities(*doc, query);
    c.out.begin_object().key("scene").str(path.empty() ? StringView("the open scene") : path);
    c.out.key("count").num_u(hits.size()).key("hits").begin_array();
    for (const editor::FindHit& h : hits)
        c.out.begin_object().key("entity").num_u(h.id).key("name").str(h.name.view()).key("path").str(h.path.view()).end_object();
    c.out.end_array().key("report").str(editor::format_hits(hits.span()).view()).end_object();
    return true;
}

}  // namespace mce::mcp
