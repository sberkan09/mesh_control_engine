// asset_used_by (M-ASSETUSEDBY): the assets whose files name a given asset, so an agent can see what a delete or a
// rewrite would break. The same answer as the Assets panel's "Used by...".

#include "mce/editor/asset_refs.h"
#include "tool_args.h"
#include "tools_agent.h"

namespace mce::mcp {

bool asset_used_by(ToolCall& c) {
    if (!need_project(c)) return false;
    StringView path;
    StringView guid_text;
    if (!arg_string(c, "path", path, false) || !arg_string(c, "guid", guid_text, false)) return false;
    if (path.empty() == guid_text.empty()) {
        c.error += "give one of 'path' (under assets/) or 'guid' (\"guid:<uuid>\")";
        return false;
    }
    const editor::EditorProject& project = c.server.session().project();
    const editor::ProjectAsset* target = nullptr;
    if (!path.empty()) {
        for (const editor::ProjectAsset& a : project.assets())
            if (a.path.view() == path) target = &a;
        if (target == nullptr) {
            c.error += "no registered asset at that path (asset_list lists them)";
            return false;
        }
    } else {
        if (guid_text.size() > 5 && guid_text.substr(0, 5) == "guid:") guid_text = guid_text.substr(5);
        const Result<asset::Guid> g = asset::parse_guid(guid_text);
        if (!g) {
            c.error += "argument 'guid' must be \"guid:<uuid>\"";
            return false;
        }
        target = project.find(g.value());
        if (target == nullptr) {
            c.error += "no registered asset with that guid (asset_list lists them)";
            return false;
        }
    }
    editor::AssetRefs refs;
    refs.build(project);
    Array<cook::String> users;
    refs.used_by(target->guid, users);
    c.out.begin_object().key("asset").str(target->path.view()).key("count").num_u(users.size()).key("used_by").begin_array();
    for (const cook::String& u : users) c.out.str(u.view());
    c.out.end_array().end_object();
    return true;
}

}  // namespace mce::mcp
